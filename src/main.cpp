// AirMirror - entry point and window shell.
#include "app.h"

#include <objbase.h>
#include <shellapi.h>

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

static App *g_app = nullptr;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app) {
        bool handled = false;
        LRESULT r = g_app->HandleMessage(msg, wp, lp, handled);
        if (handled) return r;
    }
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1; // the swap chain owns every pixel
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Per-monitor DPI v2 without needing a manifest.
static void EnableDpiAwareness() {
    using SetCtxFn = BOOL(WINAPI *)(HANDLE);
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (!user32) return;
    auto setCtx = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (setCtx) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        setCtx((HANDLE)-4);
    } else {
        using SetDpiFn = BOOL(WINAPI *)(void);
        auto setDpi = (SetDpiFn)GetProcAddress(user32, "SetProcessDPIAware");
        if (setDpi) setDpi();
    }
}

static const wchar_t *kUsage =
    L"AirMirror - AirPlay screen mirroring receiver\n\n"
    L"  --name <text>     Name shown in the iPhone/iPad AirPlay list\n"
    L"                    (default: this PC's name)\n"
    L"  --size <WxH>      Display size advertised to the client.\n"
    L"                    Default 1920x1080. Use 1440x1080 to ask a 4:3 iPad\n"
    L"                    for a 4:3 stream instead of a 16:9 one.\n"
    L"  --fps <n>         Advertised refresh rate / max FPS (default 60)\n"
    L"  --scale <f>       Starting window scale, 0.3 - 3.0 (default 1.0)\n"
    L"  --h265            Advertise H.265 support (needed for 4K sources)\n"
    L"  --device <model>  Preview a device frame with no client attached,\n"
    L"                    e.g. --device iPad13,4\n"
    L"  --preview <WxH>   Fake stream size for the preview, e.g. 2732x2048\n"
    L"  --uvtest          Self-check: show a synthetic 810x1080 picture inside\n"
    L"                    the padded 816x1088 surface a decoder would allocate.\n"
    L"                    Any green edge means padding is being sampled.\n"
    L"  --help            Show this message\n\n"
    L"In the window: drag to move, wheel to resize, right-click for a menu.\n"
    L"L flips landscape orientation, M mutes, 0 resets size, Esc quits.";

static bool ParseArgs(AppOptions &opts, bool &showHelp) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return true;

    bool ok = true;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&](std::wstring &out) {
            if (i + 1 >= argc) { ok = false; return false; }
            out = argv[++i];
            return true;
        };
        std::wstring v;
        if (a == L"--help" || a == L"-h" || a == L"/?") {
            showHelp = true;
        } else if (a == L"--name" && next(v)) {
            opts.serviceName = Narrow(v);
        } else if (a == L"--size" && next(v)) {
            int w = 0, h = 0;
            if (swscanf(v.c_str(), L"%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                opts.adWidth = (unsigned short)w;
                opts.adHeight = (unsigned short)h;
            } else {
                ok = false;
            }
        } else if (a == L"--fps" && next(v)) {
            int f = _wtoi(v.c_str());
            if (f > 0 && f < 256) opts.adFps = (unsigned short)f;
        } else if (a == L"--scale" && next(v)) {
            opts.startScale = (float)_wtof(v.c_str());
        } else if (a == L"--h265") {
            opts.allowH265 = true;
        } else if (a == L"--uvtest") {
            opts.uvTest = true;
        } else if (a == L"--device" && next(v)) {
            opts.previewModel = Narrow(v);
        } else if (a == L"--preview" && next(v)) {
            int w = 0, h = 0;
            if (swscanf(v.c_str(), L"%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                opts.previewW = w;
                opts.previewH = h;
            } else {
                ok = false;
            }
        } else if (!a.empty() && a[0] == L'-') {
            ok = false;
        }
    }
    LocalFree(argv);
    return ok;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    EnableDpiAwareness();

    AppOptions opts;
    bool showHelp = false;
    if (!ParseArgs(opts, showHelp) || showHelp) {
        MessageBoxW(nullptr, kUsage, L"AirMirror",
                    showHelp ? MB_ICONINFORMATION : MB_ICONWARNING);
        if (showHelp) return 0;
    }

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hrCom)) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"AirMirrorWindow";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register the window class.", L"AirMirror",
                    MB_ICONERROR);
        return 1;
    }

    // WS_EX_NOREDIRECTIONBITMAP is what allows the DirectComposition swap chain
    // to be genuinely transparent instead of compositing onto a black window.
    HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW,
                                wc.lpszClassName, L"AirMirror", WS_POPUP, CW_USEDEFAULT,
                                CW_USEDEFAULT, 420, 860, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create the window.", L"AirMirror", MB_ICONERROR);
        return 1;
    }

    App app;
    g_app = &app;
    if (!app.Init(hwnd, opts)) {
        MessageBoxW(hwnd, L"AirMirror could not start.\n\nCheck that no other AirPlay "
                          L"receiver is running and that a network adapter is active.",
                    L"AirMirror", MB_ICONERROR);
        app.Shutdown();
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        // Draw when there is something new; otherwise sleep until the decoder
        // signals a frame or a message arrives.
        if (!app.Tick()) {
            HANDLE h = app.FrameEvent();
            const DWORD timeout = app.Animating() ? 8 : 250;
            MsgWaitForMultipleObjects(1, &h, FALSE, timeout, QS_ALLINPUT);
        }
    }

    g_app = nullptr;
    app.Shutdown();
    CoUninitialize();
    return 0;
}
