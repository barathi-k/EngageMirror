// EngageMirror - macOS platform layer for App: window sizing, menus, the UI
// thread hand-off and the frame loop. Shared logic is in app.cpp.
#include "app.h"

#import <AppKit/AppKit.h>
#include <Network/Network.h>

#include <cmath>

// Target for context-menu items; the tag carries the Cmd.
@interface AMMenuTarget : NSObject
@property(nonatomic, assign) App *app;
- (void)fire:(NSMenuItem *)item;
@end

@implementation AMMenuTarget
- (void)fire:(NSMenuItem *)item {
    self.app->Command((Cmd)item.tag);
}
@end

static AMMenuTarget *g_menuTarget = nil;
static id<NSObject> g_activity = nil; // App Nap / display-sleep hold while mirroring

bool App::InitPlatform() {
    NSView *view = window_.contentView;
    return gpu_.Init((CAMetalLayer *)view.layer, (float)window_.backingScaleFactor);
}

std::string App::DefaultServiceName() const {
    // Suffixed so it does not collide with macOS's own AirPlay Receiver, which
    // advertises under the bare computer name.
    NSString *host = [[NSHost currentHost] localizedName];
    if (host.length == 0) return std::string();
    return std::string(host.UTF8String) + " (EngageMirror)";
}

void App::WorkAreaPx(float &w, float &h) const {
    NSScreen *screen = window_.screen ?: [NSScreen mainScreen];
    if (!screen) return;
    const NSRect vis = screen.visibleFrame;
    const CGFloat s = screen.backingScaleFactor;
    w = (float)(vis.size.width * s);
    h = (float)(vis.size.height * s);
}

void App::SetWindowSizeKeepCenter(int w, int h) {
    if (w <= 0 || h <= 0) return;
    const CGFloat s = window_.backingScaleFactor;
    gpu_.SetScale((float)s);
    // Whole points, so the drawable maps 1:1 onto the window's pixels.
    const CGFloat ptW = std::ceil(w / s), ptH = std::ceil(h / s);

    NSRect fr = window_.frame;
    if (fr.size.width != ptW || fr.size.height != ptH) {
        NSRect next = NSMakeRect(NSMidX(fr) - ptW / 2, NSMidY(fr) - ptH / 2, ptW, ptH);
        // The square is a good deal larger than the device, so growing around
        // the centre can push it off the screen - nudge it back on.
        if (NSScreen *screen = window_.screen ?: [NSScreen mainScreen]) {
            const NSRect vis = screen.visibleFrame;
            if (ptW <= vis.size.width)
                next.origin.x = std::clamp(next.origin.x, NSMinX(vis), NSMaxX(vis) - ptW);
            if (ptH <= vis.size.height)
                next.origin.y = std::clamp(next.origin.y, NSMinY(vis), NSMaxY(vis) - ptH);
        }
        next.origin.x = std::round(next.origin.x);
        next.origin.y = std::round(next.origin.y);
        [window_ setFrame:next display:NO];
    }
    gpu_.Resize((uint32_t)(ptW * s), (uint32_t)(ptH * s));
}

// ---------------------------------------------------------------------------
// Frame loop. Nothing here runs on a timer while idle: a tick is requested
// when a frame is decoded or state changes, and only an animation in progress
// schedules the next one.
// ---------------------------------------------------------------------------
void App::WakeUi() {
    if (tickQueued_.exchange(true)) return; // one pending tick is enough
    dispatch_async(dispatch_get_main_queue(), ^{
      tickQueued_.store(false);
      RunTick();
    });
}

void App::RunTick() {
    Tick();
    if (Animating() && !animArmed_) {
        animArmed_ = true;
        // ponytail: ~120 Hz one-shot while animating; a CADisplayLink would pace
        // to the display exactly but needs macOS 14.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 120),
                       dispatch_get_main_queue(), ^{
                         animArmed_ = false;
                         RunTick();
                       });
    }
}

void App::PostRelayout() {
    dispatch_async(dispatch_get_main_queue(), ^{
      OnGeometryChanged();
      RunTick();
    });
}

void App::PostClientChanged() {
    dispatch_async(dispatch_get_main_queue(), ^{
      UpdateIdleText();
      needsRedraw_ = true;
      RunTick();
    });
}

void App::SetMirroringActive(bool active) {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (active && !g_activity) {
          // Mirroring is latency-critical and on screen: no App Nap timer
          // coalescing, and the display stays awake like a video player.
          g_activity = [[NSProcessInfo processInfo]
              beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical |
                                       NSActivityIdleDisplaySleepDisabled
                                reason:@"AirPlay mirroring session"];
      } else if (!active && g_activity) {
          [[NSProcessInfo processInfo] endActivity:g_activity];
          g_activity = nil;
      }
    });
}

// macOS 15+ gates the local network per app. Until the user allows it, our
// Bonjour registration is silently withheld - and one made before the grant
// stays dead, so clicking Allow would not make the Mac appear until a relaunch.
// A Bonjour browser is how an app can observe that permission: it raises the
// prompt, sits in "waiting" (PolicyDenied) while access is off, and turns
// "ready" when it is granted. On that transition we re-advertise; once our own
// service shows up in the results the browser is cancelled, so this costs
// nothing afterwards. Event-driven throughout - nothing is polled.
void App::WatchLocalNetwork() {
    static nw_browser_t browser = nullptr;
    if (browser) return;

    nw_browse_descriptor_t desc =
        nw_browse_descriptor_create_bonjour_service("_airplay._tcp", "local.");
    nw_parameters_t params = nw_parameters_create();
    browser = nw_browser_create(desc, params);
    nw_browser_set_queue(browser, dispatch_get_main_queue());

    const std::string ownName = server_.ServiceName();
    __block bool wasBlocked = false;
    nw_browser_set_state_changed_handler(browser, ^(nw_browser_state_t state, nw_error_t err) {
      if (state == nw_browser_state_waiting) {
          if (!wasBlocked) LOGW("local network access is off - waiting for permission");
          wasBlocked = true;
      } else if (state == nw_browser_state_ready && wasBlocked) {
          wasBlocked = false;
          LOGI("local network access granted");
          server_.Readvertise();
      } else if (state == nw_browser_state_failed) {
          LOGW("local network watch failed (%d)", err ? nw_error_get_error_code(err) : 0);
      }
    });
    nw_browser_set_browse_results_changed_handler(
        browser, ^(nw_browse_result_t, nw_browse_result_t now, bool) {
          if (!now) return;
          nw_endpoint_t ep = nw_browse_result_copy_endpoint(now);
          const char *name = nw_endpoint_get_bonjour_service_name(ep);
          if (name && ownName == name) {
              LOGI("advertisement visible on the local network");
              nw_browser_cancel(browser);
          }
        });
    nw_browser_start(browser);
}

void App::PlatformCommand(Cmd c) {
    if (c == Cmd::Quit) {
        [NSApp terminate:nil];
    } else if (c == Cmd::About) {
        NSString *notice = [NSString stringWithUTF8String:kAboutText];
        NSMutableAttributedString *credits = [[NSMutableAttributedString alloc]
            initWithString:notice
                attributes:@{NSFontAttributeName : [NSFont systemFontOfSize:11]}];
        NSString *url = [NSString stringWithUTF8String:kSourceUrl];
        const NSRange r = [notice rangeOfString:url];
        if (r.location != NSNotFound) {
            [credits addAttribute:NSLinkAttributeName value:[NSURL URLWithString:url] range:r];
        }
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp orderFrontStandardAboutPanelWithOptions:@{
            NSAboutPanelOptionCredits : credits,
        }];
    }
}

void App::ShowContextMenu(int x, int y) {
    if (!g_menuTarget) g_menuTarget = [AMMenuTarget new];
    g_menuTarget.app = this;

    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    menu.autoenablesItems = NO;
    auto add = [&](NSString *title, Cmd c, NSString *key) {
        NSMenuItem *it = [menu addItemWithTitle:title action:@selector(fire:) keyEquivalent:key];
        it.keyEquivalentModifierMask = 0;
        it.target = g_menuTarget;
        it.tag = (NSInteger)c;
        return it;
    };
    add(@"Flip Landscape Orientation", Cmd::Flip, @"l");
    add(@"Mute Audio", Cmd::Mute, @"m").state =
        audio_.Muted() ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:[NSMenuItem separatorItem]];
    add(@"Zoom In", Cmd::ZoomIn, @"+");
    add(@"Zoom Out", Cmd::ZoomOut, @"-");
    add(@"Actual Size", Cmd::ResetZoom, @"0");
    [menu addItem:[NSMenuItem separatorItem]];
    add(@"About EngageMirror", Cmd::About, @"");
    add(@"Quit EngageMirror", Cmd::Quit, @"q").keyEquivalentModifierMask =
        NSEventModifierFlagCommand;

    NSView *view = window_.contentView;
    const CGFloat s = window_.backingScaleFactor;
    [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(x / s, y / s) inView:view];
}

// Same check as the D3D build: a synthetic 810x1080 picture in the 816x1088
// surface a decoder pads it to, padding zeroed (bright green through BT.709).
bool App::MakeUvTestFrame() {
    const int w = 810, h = 1080;
    const int tw = 816, th = 1088;

    std::vector<uint8_t> luma((size_t)tw * th, 0), chroma((size_t)tw * th / 2, 0);
    for (int y = 0; y < h; y++) memset(luma.data() + (size_t)y * tw, 235, w);
    for (int y = 0; y < h / 2; y++) memset(chroma.data() + (size_t)y * tw, 128, w);

    auto plane = [&](MTLPixelFormat fmt, int pw, int ph, const uint8_t *src, TexView &out) {
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                               width:(NSUInteger)pw
                                                              height:(NSUInteger)ph
                                                           mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        out.tex = [gpu_.Device() newTextureWithDescriptor:td];
        [out.tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)pw, (NSUInteger)ph)
                   mipmapLevel:0
                     withBytes:src
                   bytesPerRow:(NSUInteger)tw];
        return out.tex != nil;
    };
    TexView y, uv;
    if (!plane(MTLPixelFormatR8Unorm, tw, th, luma.data(), y) ||
        !plane(MTLPixelFormatRG8Unorm, tw / 2, th / 2, chroma.data(), uv)) {
        LOGE("uvtest: texture creation failed");
        return false;
    }

    renderer_.SetVideoNV12(y, uv, w, h, tw, th, 0, 0, false);
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        streamW_ = (float)w;
        streamH_ = (float)h;
    }
    OnGeometryChanged();
    LOGI("uvtest: %dx%d picture in a %dx%d surface; any green edge is a bug", w, h, tw, th);
    return true;
}
