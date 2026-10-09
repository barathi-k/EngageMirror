// AirMirror - macOS entry point and window shell.
#include "app.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdio>

static App g_app;

// Borderless windows refuse key status by default; we need it for shortcuts.
@interface AMWindow : NSWindow
@end
@implementation AMWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

// A view backed by a CAMetalLayer. Flipped, so view coordinates match the
// top-left-origin pixel space App and the renderer work in.
@interface AMView : NSView
@end
@implementation AMView
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }
- (BOOL)wantsUpdateLayer { return YES; }
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }

- (NSPoint)pixelsFor:(NSEvent *)e {
    const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    const CGFloat s = self.window.backingScaleFactor;
    return NSMakePoint(p.x * s, p.y * s);
}

- (void)mouseDown:(NSEvent *)e {
    const NSPoint p = [self pixelsFor:e];
    if (g_app.HitsDevice((int)p.x, (int)p.y)) [self.window performWindowDragWithEvent:e];
}

- (void)rightMouseDown:(NSEvent *)e {
    const NSPoint p = [self pixelsFor:e];
    if (g_app.HitsDevice((int)p.x, (int)p.y)) g_app.ShowContextMenu((int)p.x, (int)p.y);
}

- (void)scrollWheel:(NSEvent *)e {
    // A trackpad sends many small precise deltas; a wheel sends whole lines.
    const CGFloat dy = e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / 10.0 : e.scrollingDeltaY;
    if (dy == 0) return;
    g_app.Zoom(powf(1.08f, (float)dy));
    g_app.RunTick();
}

- (void)keyDown:(NSEvent *)e {
    if (e.keyCode == 53) { // Escape
        g_app.Command(Cmd::Quit);
        return;
    }
    NSString *k = e.charactersIgnoringModifiers.lowercaseString;
    if ([k isEqualToString:@"l"]) g_app.Command(Cmd::Flip);
    else if ([k isEqualToString:@"m"]) g_app.Command(Cmd::Mute);
    else if ([k isEqualToString:@"r"]) g_app.Command(Cmd::SwapStream);
    else if ([k isEqualToString:@"+"] || [k isEqualToString:@"="]) g_app.Command(Cmd::ZoomIn);
    else if ([k isEqualToString:@"-"]) g_app.Command(Cmd::ZoomOut);
    else if ([k isEqualToString:@"0"]) g_app.Command(Cmd::ResetZoom);
    else [super keyDown:e];
    g_app.RunTick();
}
@end

@interface AMDelegate : NSObject <NSApplicationDelegate>
@property(nonatomic, strong) AMWindow *window;
@end

@implementation AMDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }
- (void)applicationWillTerminate:(NSNotification *)n { g_app.Shutdown(); }
- (void)showAbout:(id)sender { g_app.Command(Cmd::About); }

- (void)screenChanged:(NSNotification *)n {
    g_app.OnDisplayChanged();
    g_app.RunTick();
}

// The window is a square larger than the device; only the device itself
// should catch the mouse. Re-evaluated whenever the mouse moves (no polling):
// over the transparent margin the window ignores mouse events and clicks fall
// through to whatever is behind it.
- (void)updateClickThrough {
    NSWindow *w = self.window;
    const NSPoint inWin = [w convertPointFromScreen:[NSEvent mouseLocation]];
    const NSPoint inView = [w.contentView convertPoint:inWin fromView:nil];
    const CGFloat s = w.backingScaleFactor;
    const bool hit = g_app.HitsDevice((int)(inView.x * s), (int)(inView.y * s));
    if (w.ignoresMouseEvents == hit) w.ignoresMouseEvents = !hit;
}
@end

static NSMenu *MakeMainMenu() {
    NSMenu *bar = [NSMenu new];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"AirMirror"];
    // nil-targeted: the app delegate is on the responder chain.
    [appMenu addItemWithTitle:@"About AirMirror" action:@selector(showAbout:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide AirMirror" action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItemWithTitle:@"Quit AirMirror" action:@selector(terminate:) keyEquivalent:@"q"];
    NSMenuItem *appItem = [NSMenuItem new];
    appItem.submenu = appMenu;
    [bar addItem:appItem];
    return bar;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        AppOptions opts;
        bool showHelp = false;
        std::vector<std::string> args(argv + 1, argv + argc);
        if (!ParseAppOptions(args, opts, showHelp) || showHelp) {
            fprintf(showHelp ? stdout : stderr, "%s\n", kUsage);
            if (showHelp) return 0;
        }

        NSApplication *nsapp = [NSApplication sharedApplication];
        nsapp.activationPolicy = NSApplicationActivationPolicyRegular;
        nsapp.mainMenu = MakeMainMenu();
        AMDelegate *delegate = [AMDelegate new];
        nsapp.delegate = delegate;

        AMWindow *window = [[AMWindow alloc] initWithContentRect:NSMakeRect(0, 0, 420, 860)
                                                       styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.opaque = NO;
        window.backgroundColor = NSColor.clearColor;
        window.hasShadow = NO; // the renderer draws its own contact shadow
        window.title = @"AirMirror";
        window.acceptsMouseMovedEvents = YES;
        window.releasedWhenClosed = NO;
        AMView *view = [[AMView alloc] initWithFrame:window.contentLayoutRect];
        view.wantsLayer = YES;
        window.contentView = view;
        delegate.window = window;

        if (!g_app.Init(window, opts)) {
            NSAlert *alert = [NSAlert new];
            alert.messageText = @"AirMirror could not start.";
            alert.informativeText = @"Check that no other AirPlay receiver is using the "
                                    @"network and that Wi-Fi or Ethernet is connected.";
            [alert runModal];
            g_app.Shutdown();
            return 1;
        }

        NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
        for (NSNotificationName name in @[
                 NSWindowDidChangeBackingPropertiesNotification,
                 NSWindowDidChangeScreenNotification,
                 NSApplicationDidChangeScreenParametersNotification ]) {
            [nc addObserver:delegate selector:@selector(screenChanged:) name:name object:nil];
        }
        const NSEventMask moves = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged;
        [NSEvent addGlobalMonitorForEventsMatchingMask:moves
                                               handler:^(NSEvent *) {
                                                 [delegate updateClickThrough];
                                               }];
        [NSEvent addLocalMonitorForEventsMatchingMask:moves
                                              handler:^NSEvent *(NSEvent *e) {
                                                [delegate updateClickThrough];
                                                return e;
                                              }];

        [window center];
        [window makeKeyAndOrderFront:nil];
        [nsapp activateIgnoringOtherApps:YES];
        g_app.RunTick();
        [nsapp run];
    }
    return 0;
}
