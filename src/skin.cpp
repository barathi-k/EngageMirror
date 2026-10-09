#include "skin.h"

#include <vector>

#ifdef _WIN32
#include <wincodec.h>

// Declared locally so we do not depend on which GUIDs MinGW's libuuid exports.
static const CLSID kCLSID_WICImagingFactory = {
    0xCACAF262, 0x9370, 0x4615, {0xA1, 0x3B, 0x9F, 0x55, 0x39, 0xDA, 0x4C, 0x0A}};
static const IID kIID_IWICImagingFactory = {
    0xEC5EC8A9, 0xC395, 0x4314, {0x9C, 0x77, 0x54, 0xD7, 0xA9, 0x35, 0xFF, 0x70}};
static const GUID kWICPixelFormat32bppPBGRA = {
    0x6FDDC324, 0x4E03, 0x4BFE, {0xB1, 0x85, 0x3D, 0x77, 0x76, 0x8D, 0xC9, 0x10}};

#else
#import <Foundation/Foundation.h>
#include <ImageIO/ImageIO.h>
#include <sys/stat.h>
#endif

namespace {

#ifdef _WIN32
std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : s.substr(0, slash);
}

bool FileExists(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Decodes to premultiplied BGRA, which is exactly what our blend state and the
// DirectComposition swap chain expect.
bool DecodePng(const std::wstring &path, std::vector<uint8_t> &pixels, int &w, int &h) {
    Com<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(kCLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  kIID_IWICImagingFactory, factory.putVoid());
    if (FAILED(hr)) {
        LOGE("skin: WIC factory unavailable (0x%08lX)", (unsigned long)hr);
        return false;
    }

    Com<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnLoad, decoder.put());
    if (FAILED(hr)) return false;

    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return false;

    Com<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(conv.put()))) return false;
    if (FAILED(conv->Initialize(frame.get(), kWICPixelFormat32bppPBGRA,
                                WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return false;

    UINT uw = 0, uh = 0;
    if (FAILED(conv->GetSize(&uw, &uh)) || uw == 0 || uh == 0) return false;

    pixels.resize((size_t)uw * uh * 4);
    if (FAILED(conv->CopyPixels(nullptr, uw * 4, (UINT)pixels.size(), pixels.data())))
        return false;

    w = (int)uw;
    h = (int)uh;
    return true;
}

std::string FindSkin(const std::string &name) {
    const std::wstring file = Widen(name) + L".png";
    const std::wstring exe = ExeDir();
    const std::wstring candidates[] = {
        exe + L"\\assets\\" + file,
        exe + L"\\" + file,
        exe + L"\\..\\assets\\" + file,
        exe + L"\\..\\" + file,
        L"assets\\" + file,
        file,
    };
    for (const auto &c : candidates) {
        if (FileExists(c)) return Narrow(c);
    }
    return std::string();
}

bool UploadSkin(Gpu &gpu, const std::vector<uint8_t> &pixels, int w, int h, TexView &srv) {
    // Full mip chain: the skin is far larger than the window (2290px wide shown
    // at ~800px), and plain bilinear minification of a fine bezel shimmers.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    Com<ID3D11Texture2D> tex;
    if (FAILED(gpu.Device()->CreateTexture2D(&td, nullptr, tex.put()))) {
        LOGE("skin: CreateTexture2D failed");
        return false;
    }
    if (FAILED(gpu.Device()->CreateShaderResourceView(tex.get(), nullptr, srv.put()))) {
        LOGE("skin: CreateShaderResourceView failed");
        return false;
    }
    GpuLock lk(gpu);
    gpu.Context()->UpdateSubresource(tex.get(), 0, nullptr, pixels.data(), (UINT)w * 4, 0);
    gpu.Context()->GenerateMips(srv.get());
    return true;
}
#else
std::string FindSkin(const std::string &name) {
    const std::string file = name + ".png";
    std::vector<std::string> candidates;
    if (NSString *res = [[NSBundle mainBundle] resourcePath]) {
        candidates.push_back(std::string(res.UTF8String) + "/assets/" + file);
    }
    candidates.push_back("assets/" + file);
    candidates.push_back(file);
    for (const auto &c : candidates) {
        struct stat st {};
        if (stat(c.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return c;
    }
    return std::string();
}

// Decodes to premultiplied BGRA, the same layout the Windows path produces.
bool DecodePng(const std::string &path, std::vector<uint8_t> &pixels, int &w, int &h) {
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    if (!src) return false;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img) return false;

    w = (int)CGImageGetWidth(img);
    h = (int)CGImageGetHeight(img);
    pixels.assign((size_t)w * h * 4, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(
        pixels.data(), (size_t)w, (size_t)h, 8, (size_t)w * 4, cs,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (ctx) {
        CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
        CGContextRelease(ctx);
    }
    CGImageRelease(img);
    return ctx != nullptr && w > 0 && h > 0;
}

bool UploadSkin(Gpu &gpu, const std::vector<uint8_t> &pixels, int w, int h, TexView &srv) {
    // Full mip chain: the skin is far larger than the window, and plain
    // bilinear minification of a fine bezel shimmers.
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:(NSUInteger)w
                                                          height:(NSUInteger)h
                                                       mipmapped:YES];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [gpu.Device() newTextureWithDescriptor:td];
    if (!tex) {
        LOGE("skin: texture creation failed");
        return false;
    }
    [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
           mipmapLevel:0
             withBytes:pixels.data()
           bytesPerRow:(NSUInteger)w * 4];
    id<MTLCommandBuffer> cb = [gpu.Queue() commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit generateMipmapsForTexture:tex];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    srv.tex = tex;
    return true;
}
#endif

// Finds the opaque body extent and the transparent hole inside it. A pixel
// counts as "interior" when the body wraps it on all four sides, which
// separates the screen cutout from the transparent margin around the device.
bool Analyse(const std::vector<uint8_t> &px, int w, int h, Skin &s) {
    auto opaque = [&](int x, int y) { return px[((size_t)y * w + x) * 4 + 3] > 8; };

    std::vector<int> rowL(h, -1), rowR(h, -1), colT(w, -1), colB(w, -1);
    int bx0 = w, bx1 = -1, by0 = h, by1 = -1;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (!opaque(x, y)) continue;
            if (rowL[y] < 0) rowL[y] = x;
            rowR[y] = x;
            if (colT[x] < 0) colT[x] = y;
            colB[x] = y;
            if (x < bx0) bx0 = x;
            if (x > bx1) bx1 = x;
            if (y < by0) by0 = y;
            if (y > by1) by1 = y;
        }
    }
    if (bx1 < bx0 || by1 < by0) return false;

    int sx0 = w, sx1 = -1, sy0 = h, sy1 = -1;
    for (int y = by0; y <= by1; y++) {
        if (rowL[y] < 0) continue;
        for (int x = rowL[y] + 1; x < rowR[y]; x++) {
            if (opaque(x, y)) continue;
            if (colT[x] < 0 || y <= colT[x] || y >= colB[x]) continue;
            if (x < sx0) sx0 = x;
            if (x > sx1) sx1 = x;
            if (y < sy0) sy0 = y;
            if (y > sy1) sy1 = y;
        }
    }
    if (sx1 < sx0 || sy1 < sy0) return false;

    s.imgW = w;
    s.imgH = h;
    s.bodyX0 = bx0; s.bodyY0 = by0; s.bodyX1 = bx1; s.bodyY1 = by1;
    s.scrX0 = sx0; s.scrY0 = sy0; s.scrX1 = sx1; s.scrY1 = sy1;

    // Corner radius: how far down the body's left edge before it turns opaque.
    s.cornerRadius = (colT[bx0] >= 0) ? (float)(colT[bx0] - by0) : 0.0f;
    return true;
}

} // namespace

bool LoadSkin(Gpu &gpu, const std::string &name, Skin &out) {
    out = Skin();
    out.name = name;

    const std::string path = FindSkin(name);
    if (path.empty()) {
        LOGW("skin: %s.png not found (looked in the assets folder)", name.c_str());
        return false;
    }

    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
#ifdef _WIN32
    const bool decoded = DecodePng(Widen(path), pixels, w, h);
#else
    const bool decoded = DecodePng(path, pixels, w, h);
#endif
    if (!decoded) {
        LOGE("skin: failed to decode %s", path.c_str());
        return false;
    }
    if (!Analyse(pixels, w, h, out)) {
        LOGE("skin: %s.png has no transparent screen area", name.c_str());
        return false;
    }
    if (!UploadSkin(gpu, pixels, w, h, out.srv)) return false;

    out.valid = true;
    LOGI("skin: %s.png %dx%d  body %dx%d  screen %dx%d (%.4f) at (%d,%d)  radius %.0f",
         name.c_str(), w, h, out.bodyX1 - out.bodyX0 + 1, out.bodyY1 - out.bodyY0 + 1,
         out.ScreenW(), out.ScreenH(), out.ScreenAspect(), out.scrX0, out.scrY0,
         out.cornerRadius);
    return true;
}
