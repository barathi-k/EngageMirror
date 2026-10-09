// EngageMirror - Metal device, transparent CAMetalLayer, logging and text (macOS).
#include "gpu.h"

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>

#include <cstdarg>
#include <ctime>
#include <sys/time.h>
#include <vector>

// ---------------------------------------------------------------------------
// Logging / small helpers
// ---------------------------------------------------------------------------
// ~/Library/Logs/EngageMirror.log, where Console.app finds it.
static FILE *LogFile() {
    static FILE *f = [] {
        NSString *dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs"];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
        NSString *path = [dir stringByAppendingPathComponent:@"EngageMirror.log"];
        FILE *fp = fopen(path.fileSystemRepresentation, "w");
        // Unbuffered: the log has to survive the process being killed, and it
        // is only a handful of lines a session.
        if (fp) setvbuf(fp, nullptr, _IONBF, 0);
        return fp;
    }();
    return f;
}

void LogLine(const char *level, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    timeval tv{};
    gettimeofday(&tv, nullptr);
    tm t{};
    localtime_r(&tv.tv_sec, &t);
    char line[1240];
    snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%s] %s\n", t.tm_hour, t.tm_min,
             t.tm_sec, (int)(tv.tv_usec / 1000), level, msg);
    fputs(line, stderr);
    if (FILE *f = LogFile()) fputs(line, f);
}

double NowSeconds() {
    static const double start = CACurrentMediaTime();
    return CACurrentMediaTime() - start;
}

// ---------------------------------------------------------------------------
// Gpu
// ---------------------------------------------------------------------------
bool Gpu::Init(CAMetalLayer *layer, float scale) {
    device_ = MTLCreateSystemDefaultDevice();
    if (!device_) {
        LOGE("no Metal device");
        return false;
    }
    queue_ = [device_ newCommandQueue];
    if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device_, nullptr,
                                  &texCache_) != kCVReturnSuccess) {
        LOGE("CVMetalTextureCacheCreate failed");
        return false;
    }

    layer_ = layer;
    layer_.device = device_;
    layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer_.framebufferOnly = YES;
    // Per-pixel transparency: the shader writes premultiplied alpha and the
    // window server composites it over the desktop.
    layer_.opaque = NO;
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    layer_.colorspace = srgb;
    CGColorSpaceRelease(srgb);
    SetScale(scale);

    const CGSize sz = layer_.drawableSize;
    width_ = (uint32_t)sz.width;
    height_ = (uint32_t)sz.height;
    LOGI("gpu: %s", device_.name.UTF8String);
    return true;
}

void Gpu::SetScale(float scale) {
    scale_ = scale > 0.0f ? scale : 1.0f;
    layer_.contentsScale = scale_;
}

bool Gpu::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return true;
    width_ = width;
    height_ = height;
    layer_.drawableSize = CGSizeMake(width, height);
    return true;
}

void Gpu::Shutdown() {
    if (texCache_) {
        CFRelease(texCache_);
        texCache_ = nullptr;
    }
    layer_ = nil;
    queue_ = nil;
    device_ = nil;
}

// ---------------------------------------------------------------------------
// CoreText text -> premultiplied BGRA texture
// ---------------------------------------------------------------------------
bool MakeTextTexture(Gpu &gpu, const std::string &utf8, int pixelHeight, bool bold,
                     TexView &outSrv, int &outW, int &outH) {
    outSrv.reset();
    if (utf8.empty()) return false;

    const CGFloat size = pixelHeight * gpu.Scale();
    NSFont *font = [NSFont systemFontOfSize:size
                                     weight:bold ? NSFontWeightSemibold : NSFontWeightRegular];
    NSString *text = [NSString stringWithUTF8String:utf8.c_str()];
    NSDictionary *attrs = @{
        (__bridge id)kCTFontAttributeName : font,
        (__bridge id)kCTForegroundColorFromContextAttributeName : @YES,
    };
    CFAttributedStringRef as = (__bridge_retained CFAttributedStringRef)
        [[NSAttributedString alloc] initWithString:text attributes:attrs];
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CFRelease(as);

    CGFloat ascent = 0, descent = 0, leading = 0;
    const double width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    const int pad = (int)std::ceil(2 * gpu.Scale());
    const int w = (int)std::ceil(width) + 2 * pad;
    const int h = (int)std::ceil(ascent + descent) + 2 * pad;
    if (w <= 0 || h <= 0) {
        CFRelease(line);
        return false;
    }

    // Premultiplied BGRA, white text on transparent, which is the layout the
    // shader samples on both platforms.
    std::vector<uint8_t> pixels((size_t)w * h * 4, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(
        pixels.data(), (size_t)w, (size_t)h, 8, (size_t)w * 4, cs,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!ctx) {
        CFRelease(line);
        return false;
    }
    CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
    CGContextSetTextPosition(ctx, pad, pad + descent);
    CTLineDraw(line, ctx);
    CGContextRelease(ctx);
    CFRelease(line);

    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:(NSUInteger)w
                                                          height:(NSUInteger)h
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [gpu.Device() newTextureWithDescriptor:td];
    if (!tex) return false;
    [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
           mipmapLevel:0
             withBytes:pixels.data()
           bytesPerRow:(NSUInteger)w * 4];

    outSrv.tex = tex;
    outW = w;
    outH = h;
    return true;
}
