// AirMirror - GPU device plus a per-pixel transparent presentation surface.
// Transparency is what lets the device frame sit on the desktop as a real
// rounded object with a soft shadow instead of a rectangle with fake corners.
//
// Windows: Direct3D 11 + a DirectComposition-backed swap chain.
// macOS:   Metal + a non-opaque CAMetalLayer.
#pragma once

#include "common.h"

#include <string>

#ifdef _WIN32

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>

// Something the shaders can sample.
using TexView = Com<ID3D11ShaderResourceView>;

class Gpu {
  public:
    bool Init(HWND hwnd);
    void Shutdown();

    // Recreates the back buffer. Safe to call with unchanged size (no-op).
    bool Resize(uint32_t width, uint32_t height);
    void Present(bool vsync);

    ID3D11Device *Device() const { return device_.get(); }
    ID3D11DeviceContext *Context() const { return context_.get(); }
    ID3D11RenderTargetView *BackBufferRTV() const { return rtv_.get(); }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    // Physical pixels per layout pixel. The Windows build works in raw pixels.
    float Scale() const { return 1.0f; }

    // The immediate context is shared with the FFmpeg decoder thread; the
    // device is created with multithread protection, and every caller of
    // Context() must additionally hold this lock.
    void Lock() { EnterCriticalSection(&lock_); }
    void Unlock() { LeaveCriticalSection(&lock_); }

    // Compiles HLSL at runtime (d3dcompiler_47.dll ships with Windows), which
    // keeps the build free of an fxc step.
    bool CompileVS(const char *src, const char *entry, Com<ID3D11VertexShader> &out,
                   Com<ID3D11InputLayout> *layout = nullptr,
                   const D3D11_INPUT_ELEMENT_DESC *elems = nullptr, UINT numElems = 0);
    bool CompilePS(const char *src, const char *entry, Com<ID3D11PixelShader> &out);

  private:
    bool CreateBackBufferViews();

    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<IDXGISwapChain1> swapChain_;
    Com<ID3D11RenderTargetView> rtv_;

    Com<IDCompositionDevice> dcompDevice_;
    Com<IDCompositionTarget> dcompTarget_;
    Com<IDCompositionVisual> dcompVisual_;

    CRITICAL_SECTION lock_{};
    bool lockInit_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

// Scoped helper for Gpu::Lock/Unlock.
struct GpuLock {
    explicit GpuLock(Gpu &g) : g_(g) { g_.Lock(); }
    ~GpuLock() { g_.Unlock(); }
    GpuLock(const GpuLock &) = delete;
    GpuLock &operator=(const GpuLock &) = delete;
    Gpu &g_;
};

#else // macOS - compiled as Objective-C++ with ARC

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <CoreVideo/CVMetalTextureCache.h>

// A texture plus whatever keeps its backing store alive (a CVMetalTexture and
// the decoder's CVPixelBuffer). ARC retains both, so copying a TexView into a
// command buffer's completion handler keeps the surface from being recycled
// by the decoder while the GPU is still reading it.
struct TexView {
    id<MTLTexture> tex = nil;
    id keep = nil;

    void reset() {
        tex = nil;
        keep = nil;
    }
    explicit operator bool() const { return tex != nil; }
};

class Gpu {
  public:
    bool Init(CAMetalLayer *layer, float scale);
    void Shutdown();

    // Sets the drawable size in physical pixels.
    bool Resize(uint32_t width, uint32_t height);
    void SetScale(float scale);
    // Presentation is scheduled by the renderer's command buffer.
    void Present(bool) {}

    id<MTLDevice> Device() const { return device_; }
    id<MTLCommandQueue> Queue() const { return queue_; }
    CAMetalLayer *Layer() const { return layer_; }
    CVMetalTextureCacheRef TextureCache() const { return texCache_; }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    // Physical pixels per point (2 on a Retina display). Everything the app
    // lays out is in physical pixels; this only converts to/from AppKit.
    float Scale() const { return scale_; }

  private:
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    CAMetalLayer *layer_ = nil;
    CVMetalTextureCacheRef texCache_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    float scale_ = 1.0f;
};

#endif

// Renders a UTF-8 string to a premultiplied-alpha BGRA texture, white on
// transparent. Used only for the idle/status overlay, so the cost is
// irrelevant. pixelHeight is in layout pixels and is scaled by Gpu::Scale().
bool MakeTextTexture(Gpu &gpu, const std::string &text, int pixelHeight, bool bold,
                     TexView &outSrv, int &outW, int &outH);
