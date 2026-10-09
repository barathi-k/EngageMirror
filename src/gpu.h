// AirMirror - Direct3D 11 device plus a DirectComposition-backed, per-pixel
// transparent swap chain. Transparency is what lets the device frame sit on
// the desktop as a real rounded object with a soft shadow instead of a
// rectangle with fake corners.
#pragma once

#include "common.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>

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

// Renders a UTF-8 string to a premultiplied-alpha RGBA texture using GDI.
// Used only for the idle/status overlay, so the cost is irrelevant and it
// avoids pulling Direct2D/DirectWrite into the build.
bool MakeTextTexture(Gpu &gpu, const std::wstring &text, int pixelHeight, bool bold,
                     Com<ID3D11ShaderResourceView> &outSrv, int &outW, int &outH);
