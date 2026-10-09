// EngageMirror - entry point and window shell.
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

static bool ParseArgs(AppOptions &opts, bool &showHelp) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return true;
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.push_back(Narrow(argv[i]));
    LocalFree(argv);
    return ParseAppOptions(args, opts, showHelp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    EnableDpiAwareness();

    AppOptions opts;
    bool showHelp = false;
    if (!ParseArgs(opts, showHelp) || showHelp) {
        MessageBoxW(nullptr, Widen(kUsage).c_str(), L"EngageMirror",
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
    wc.lpszClassName = L"EngageMirrorWindow";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register the window class.", L"EngageMirror",
                    MB_ICONERROR);
        return 1;
    }

    // WS_EX_NOREDIRECTIONBITMAP is what allows the DirectComposition swap chain
    // to be genuinely transparent instead of compositing onto a black window.
    HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW,
                                wc.lpszClassName, L"EngageMirror", WS_POPUP, CW_USEDEFAULT,
                                CW_USEDEFAULT, 420, 860, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create the window.", L"EngageMirror", MB_ICONERROR);
        return 1;
    }

    App app;
    g_app = &app;
    if (!app.Init(hwnd, opts)) {
        MessageBoxW(hwnd, L"EngageMirror could not start.\n\nCheck that no other AirPlay "
                          L"receiver is running and that a network adapter is active.",
                    L"EngageMirror", MB_ICONERROR);
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
        // signals a frame or a message arrives. Nothing is polled while idle.
        if (!app.Tick()) {
            HANDLE h = app.FrameEvent();
            const DWORD timeout = app.Animating() ? 8 : INFINITE;
            MsgWaitForMultipleObjects(1, &h, FALSE, timeout, QS_ALLINPUT);
        }
    }

    g_app = nullptr;
    app.Shutdown();
    CoUninitialize();
    return 0;
}
