#include "gpu.h"

#include <d3dcompiler.h>
#include <cstdarg>
#include <vector>

const GUID IID_MultithreadLite = {
    0x9B7E4E00, 0x342C, 0x4106, {0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0}};

// ---------------------------------------------------------------------------
// Logging / small helpers (defined here so every TU gets them via common.h)
// ---------------------------------------------------------------------------
// Opened next to the executable on first use. stderr is only there when the
// app was started from a console, which is rarely how it runs.
static FILE *LogFile() {
    static FILE *f = [] {
        wchar_t path[MAX_PATH] = {};
        if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return (FILE *)nullptr;
        wchar_t *slash = wcsrchr(path, L'\\');
        if (!slash) return (FILE *)nullptr;
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"engagemirror.log");
        FILE *fp = _wfopen(path, L"w");
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

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1240];
    snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [%s] %s\n", t.wHour, t.wMinute,
             t.wSecond, t.wMilliseconds, level, msg);
    OutputDebugStringA(line);
    fputs(line, stderr);
    if (FILE *f = LogFile()) fputs(line, f);
}

std::wstring Widen(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring &s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string a(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &a[0], n, nullptr, nullptr);
    return a;
}

double NowSeconds() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    static LARGE_INTEGER start = [] {
        LARGE_INTEGER s;
        QueryPerformanceCounter(&s);
        return s;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart - start.QuadPart) / double(freq.QuadPart);
}

// ---------------------------------------------------------------------------
// Gpu
// ---------------------------------------------------------------------------
bool Gpu::Init(HWND hwnd) {
    InitializeCriticalSection(&lock_);
    lockInit_ = true;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#if !defined(NDEBUG) && defined(ENGAGEMIRROR_D3D_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};

    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   device_.put(), &got, context_.put());
    if (FAILED(hr)) {
        LOGW("hardware D3D11 device failed (0x%08lX), falling back to WARP", (unsigned long)hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, device_.put(), &got,
                               context_.put());
    }
    if (FAILED(hr)) {
        LOGE("D3D11CreateDevice failed: 0x%08lX", (unsigned long)hr);
        return false;
    }

    // The decoder runs on the AirPlay network thread while we render on the UI
    // thread; both touch the immediate context.
    Com<IMultithreadLite> mt;
    if (SUCCEEDED(device_->QueryInterface(IID_MultithreadLite, mt.putVoid()))) {
        mt->SetMultithreadProtected(TRUE);
    }

    Com<IDXGIDevice> dxgiDevice;
    if (FAILED(device_->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.putVoid()))) {
        LOGE("QueryInterface(IDXGIDevice) failed");
        return false;
    }
    Com<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.put()))) return false;
    Com<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), factory.putVoid()))) {
        LOGE("IDXGIFactory2 unavailable");
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    width_ = (uint32_t)(rc.right - rc.left);
    height_ = (uint32_t)(rc.bottom - rc.top);
    if (width_ == 0) width_ = 640;
    if (height_ == 0) height_ = 480;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED; // required for DComp
    desc.Scaling = DXGI_SCALING_STRETCH;

    hr = factory->CreateSwapChainForComposition(device_.get(), &desc, nullptr,
                                                swapChain_.put());
    if (FAILED(hr)) {
        LOGE("CreateSwapChainForComposition failed: 0x%08lX", (unsigned long)hr);
        return false;
    }

    hr = DCompositionCreateDevice(dxgiDevice.get(), __uuidof(IDCompositionDevice),
                                  dcompDevice_.putVoid());
    if (FAILED(hr)) {
        LOGE("DCompositionCreateDevice failed: 0x%08lX", (unsigned long)hr);
        return false;
    }
    if (FAILED(dcompDevice_->CreateTargetForHwnd(hwnd, TRUE, dcompTarget_.put()))) return false;
    if (FAILED(dcompDevice_->CreateVisual(dcompVisual_.put()))) return false;
    if (FAILED(dcompVisual_->SetContent(swapChain_.get()))) return false;
    if (FAILED(dcompTarget_->SetRoot(dcompVisual_.get()))) return false;
    if (FAILED(dcompDevice_->Commit())) return false;

    return CreateBackBufferViews();
}

bool Gpu::CreateBackBufferViews() {
    rtv_.reset();
    Com<ID3D11Texture2D> back;
    if (FAILED(swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), back.putVoid()))) {
        LOGE("swapchain GetBuffer failed");
        return false;
    }
    if (FAILED(device_->CreateRenderTargetView(back.get(), nullptr, rtv_.put()))) {
        LOGE("CreateRenderTargetView failed");
        return false;
    }
    return true;
}

bool Gpu::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return true;
    if (width == width_ && height == height_ && rtv_) return true;

    GpuLock lk(*this);
    rtv_.reset();
    context_->ClearState();
    context_->Flush();

    HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        LOGE("ResizeBuffers(%u,%u) failed: 0x%08lX", width, height, (unsigned long)hr);
        return false;
    }
    width_ = width;
    height_ = height;
    return CreateBackBufferViews();
}

void Gpu::Present(bool vsync) {
    if (!swapChain_) return;
    swapChain_->Present(vsync ? 1 : 0, 0);
}

void Gpu::Shutdown() {
    if (context_) context_->ClearState();
    rtv_.reset();
    dcompVisual_.reset();
    dcompTarget_.reset();
    dcompDevice_.reset();
    swapChain_.reset();
    context_.reset();
    device_.reset();
    if (lockInit_) {
        DeleteCriticalSection(&lock_);
        lockInit_ = false;
    }
}

static bool CompileBlob(const char *src, const char *entry, const char *target,
                        Com<ID3DBlob> &out) {
    Com<ID3DBlob> err;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target,
                            flags, 0, out.put(), err.put());
    if (FAILED(hr)) {
        LOGE("shader %s/%s failed: %s", entry, target,
             err ? (const char *)err->GetBufferPointer() : "unknown");
        return false;
    }
    return true;
}

bool Gpu::CompileVS(const char *src, const char *entry, Com<ID3D11VertexShader> &out,
                    Com<ID3D11InputLayout> *layout, const D3D11_INPUT_ELEMENT_DESC *elems,
                    UINT numElems) {
    Com<ID3DBlob> blob;
    if (!CompileBlob(src, entry, "vs_5_0", blob)) {
        if (!CompileBlob(src, entry, "vs_4_0", blob)) return false;
    }
    if (FAILED(device_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                           nullptr, out.put())))
        return false;
    if (layout && elems && numElems) {
        if (FAILED(device_->CreateInputLayout(elems, numElems, blob->GetBufferPointer(),
                                              blob->GetBufferSize(), layout->put())))
            return false;
    }
    return true;
}

bool Gpu::CompilePS(const char *src, const char *entry, Com<ID3D11PixelShader> &out) {
    Com<ID3DBlob> blob;
    if (!CompileBlob(src, entry, "ps_5_0", blob)) {
        if (!CompileBlob(src, entry, "ps_4_0", blob)) return false;
    }
    return SUCCEEDED(device_->CreatePixelShader(blob->GetBufferPointer(),
                                                blob->GetBufferSize(), nullptr, out.put()));
}

// ---------------------------------------------------------------------------
// GDI text -> premultiplied RGBA texture
// ---------------------------------------------------------------------------
bool MakeTextTexture(Gpu &gpu, const std::string &utf8, int pixelHeight, bool bold,
                     TexView &outSrv, int &outW, int &outH) {
    outSrv.reset();
    if (utf8.empty()) return false;
    const std::wstring text = Widen(utf8);

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc) return false;

    HFONT font = CreateFontW(-pixelHeight, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE,
                             FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH,
                             L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);

    RECT measure{0, 0, 0, 0};
    DrawTextW(dc, text.c_str(), (int)text.size(), &measure,
              DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int w = measure.right - measure.left + 8;
    int h = measure.bottom - measure.top + 4;
    if (w <= 0 || h <= 0) {
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)w * h * 4);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rc{4, 2, w, h};
    DrawTextW(dc, text.c_str(), (int)text.size(), &rc, DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();

    // White text was drawn on black: use luminance as the alpha channel and
    // premultiply (which for pure white text means colour == alpha).
    std::vector<uint8_t> pixels((size_t)w * h * 4);
    const uint8_t *src = (const uint8_t *)bits;
    for (int i = 0; i < w * h; i++) {
        uint8_t b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
        uint8_t a = (uint8_t)((r * 77 + g * 151 + b * 28) >> 8);
        pixels[i * 4 + 0] = a; // B
        pixels[i * 4 + 1] = a; // G
        pixels[i * 4 + 2] = a; // R
        pixels[i * 4 + 3] = a; // A
    }

    SelectObject(dc, oldBmp);
    SelectObject(dc, oldFont);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(dc);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = pixels.data();
    sd.SysMemPitch = (UINT)w * 4;

    Com<ID3D11Texture2D> tex;
    if (FAILED(gpu.Device()->CreateTexture2D(&td, &sd, tex.put()))) return false;
    if (FAILED(gpu.Device()->CreateShaderResourceView(tex.get(), nullptr, outSrv.put())))
        return false;

    outW = w;
    outH = h;
    return true;
}
