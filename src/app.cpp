#include "app.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace {
enum MenuId {
    kMenuFlip = 1000,
    kMenuMute,
    kMenuZoomIn,
    kMenuZoomOut,
    kMenuResetZoom,
    kMenuQuit,
};

// Wrap into (-180, 180] so a rotation always takes the short way round.
float WrapDeg(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d <= -180.0f) d += 360.0f;
    return d;
}

float EaseInOut(float t) {
    t = Clampf(t, 0.0f, 1.0f);
    return (t < 0.5f) ? 4.0f * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) * 0.5f;
}

float Smooth01(float x) {
    x = Clampf(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// The window is a square big enough for the device in any orientation, plus
// room for the shadow. Keeping it orientation-independent is what lets a pivot
// happen without a single SetWindowPos / ResizeBuffers along the way.
float WindowSideFor(const DeviceLayout &L) {
    float hx, hy;
    DeviceExtent(L, hx, hy);
    return 2.0f * std::max(hx, hy) + 2.0f * ShadowMargin(L);
}

// Two stream sizes count as the same picture if they are the same way up and
// within 1.5% on aspect. Only the ratio is ever used - for the panel shape and
// the letterbox fit - so 1920x1080 and the macroblock-padded 1920x1088 must not
// read as a change, or they would fight each other forever.
bool SameShape(float aw, float ah, float bw, float bh) {
    if (aw <= 0.0f || ah <= 0.0f || bw <= 0.0f || bh <= 0.0f) return false;
    if ((aw > ah) != (bw > bh)) return false;
    const float ra = aw / ah, rb = bw / bh;
    return std::fabs(ra - rb) <= 0.015f * std::max(ra, rb);
}

float SdRoundRect(float px, float py, float hx, float hy, float r) {
    r = std::min(r, std::min(hx, hy));
    const float qx = std::fabs(px) - hx + r;
    const float qy = std::fabs(py) - hy + r;
    const float outside =
        std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) +
                  std::max(qy, 0.0f) * std::max(qy, 0.0f));
    return std::min(std::max(qx, qy), 0.0f) + outside - r;
}
} // namespace

bool App::Init(HWND hwnd, const AppOptions &opts) {
    hwnd_ = hwnd;
    userScale_ = Clampf(opts.startScale, 0.3f, 3.0f);
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!gpu_.Init(hwnd)) return false;
    if (!renderer_.Init(&gpu_)) return false;
    if (!decoder_.Init(&gpu_)) return false;
    audio_.Init();

    profile_ = ResolveDevice(opts.previewModel.empty() ? "iPhone16,1" : opts.previewModel,
                             "");
    streamW_ = (float)opts.previewW;
    streamH_ = (float)opts.previewH;

    AirPlayConfig cfg;
    cfg.width = opts.adWidth;
    cfg.height = opts.adHeight;
    cfg.refreshRate = opts.adFps;
    cfg.maxFps = opts.adFps;
    cfg.allowH265 = opts.allowH265;

    if (!opts.serviceName.empty()) {
        cfg.serviceName = opts.serviceName;
    } else {
        wchar_t host[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD hostLen = MAX_COMPUTERNAME_LENGTH + 1;
        if (GetComputerNameW(host, &hostLen) && hostLen > 0) {
            cfg.serviceName = Narrow(host);
        } else {
            cfg.serviceName = "AirMirror";
        }
    }
    if (!server_.Start(cfg, this)) {
        LOGE("AirPlay server failed to start");
        return false;
    }

    OnGeometryChanged();
    UpdateIdleText();
    if (opts.uvTest) MakeUvTestFrame();
    return true;
}

void App::Shutdown() {
    server_.Stop();
    audio_.Shutdown();
    current_.Reset();
    decoder_.Shutdown();
    renderer_.Shutdown();
    gpu_.Shutdown();
    if (frameEvent_) {
        CloseHandle(frameEvent_);
        frameEvent_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Geometry and orientation
// ---------------------------------------------------------------------------
const Skin *App::SkinFor(const DeviceProfile &p) {
    if (p.skinName.empty()) return nullptr;

    auto it = skins_.find(p.skinName);
    if (it == skins_.end()) {
        Skin s;
        LoadSkin(gpu_, p.skinName, s); // failure leaves s.valid == false
        it = skins_.emplace(p.skinName, std::move(s)).first;
    }
    return it->second.valid ? &it->second : nullptr;
}

float App::BaseLongEdge() const {
    RECT work{0, 0, 1920, 1080};
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(MONITORINFO)};
    if (GetMonitorInfoW(mon, &mi)) work = mi.rcWork;

    const float workH = (float)(work.bottom - work.top);
    return workH * 0.80f * userScale_;
}

// Largest square window that still sits on the monitor. Zooming in is allowed
// to exceed it, otherwise `+` would stop doing anything.
float App::WorkAreaLimit() const {
    RECT work{0, 0, 1920, 1080};
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(MONITORINFO)};
    if (GetMonitorInfoW(mon, &mi)) work = mi.rcWork;

    const float m = std::min((float)(work.right - work.left),
                             (float)(work.bottom - work.top));
    return m * 0.97f * std::max(userScale_, 1.0f);
}

float App::TargetAngle() const {
    const bool landscape = (appliedW_ > appliedH_);
    if (!landscape) return 0.0f;
    // -90 puts the device's top edge at the left of the window.
    return landscapeFlip_ ? 90.0f : -90.0f;
}

void App::RebuildDevice() {
    DeviceProfile profile;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        profile = profile_;
    }
    const Skin *skin = SkinFor(profile);

    float longPx = BaseLongEdge();
    dev_ = ComputeDeviceLayout(profile, skin, appliedW_, appliedH_, longPx);

    // The square window has to fit the monitor; everything scales linearly with
    // longPx, so one correction pass lands on it.
    const float limit = WorkAreaLimit();
    const float side = WindowSideFor(dev_);
    if (side > limit && side > 1.0f) {
        longPx *= limit / side;
        dev_ = ComputeDeviceLayout(profile, skin, appliedW_, appliedH_, longPx);
    }

    DeviceExtent(dev_, devHx_, devHy_);
    fitHalf_ = std::max(devHx_, devHy_);

    renderer_.SetProfile(profile);
    renderer_.SetSkin(skin);
    renderer_.SetDevice(dev_);
    ResizeWindow();
}

void App::RecomputeVideoRect() {
    // PanelRectAt is only meaningful at a quadrant angle, so always ask for the
    // settled one; mid-pivot the picture is dark anyway and FinishRotation will
    // pick this up.
    videoTo_ = FitVideo(PanelRectAt(dev_, TargetAngle()), appliedW_, appliedH_);
    if (!rotating_) videoNow_ = videoTo_;
}

// Records what the stream currently claims to be. Nothing here changes the UI:
// the shape has to hold steady first (see MaybeApplyGeometry).
void App::OnGeometryChanged() {
    float sw, sh;
    bool fromStream = true;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        sw = streamW_;
        sh = streamH_;
    }

    // With no stream yet, fall back to the profile's nominal shape so the idle
    // frame still looks like the right device.
    if (sw <= 0.0f || sh <= 0.0f) {
        DeviceProfile profile;
        {
            std::lock_guard<std::mutex> lk(stateMutex_);
            profile = profile_;
        }
        sw = profile.defaultAspect;
        sh = 1.0f;
        fromStream = false;
    }

    // Agreement with what is on screen cancels any pending change - that is what
    // makes this a confirmation filter rather than a plain delay.
    if (SameShape(sw, sh, appliedW_, appliedH_)) {
        wantW_ = wantH_ = 0.0f;
        return;
    }

    // Nothing to confirm against before the first real frame of a session.
    if (appliedW_ <= 0.0f || (fromStream && !streamShapeKnown_)) {
        if (fromStream) streamShapeKnown_ = true;
        ApplyGeometry(sw, sh);
        return;
    }

    const double now = NowSeconds();
    if (!SameShape(sw, sh, wantW_, wantH_)) {
        wantW_ = sw;
        wantH_ = sh;
        wantSince_ = now;
        LOGI("stream proposes %.0fx%.0f (showing %.0fx%.0f)", sw, sh, appliedW_, appliedH_);
    }
    MaybeApplyGeometry(now);
}

// Acts on a proposal once it has held steady. iOS emits a short burst of
// disagreeing sizes around a turn - trailing frames encoded before the SPS
// change, and (on this iPad) a spell of 16:9 before it settles back to 4:3.
// Reacting to each one is what made the picture flick back and forth.
void App::MaybeApplyGeometry(double now) {
    if (wantW_ <= 0.0f || rotating_) return;
    if ((now - wantSince_) < kConfirmSeconds) return;

    // A flip also has to wait out the tail of the previous turn.
    const bool flip = (wantW_ > wantH_) != (appliedW_ > appliedH_);
    if (flip && (now - orientSettled_) < kOrientHold) return;

    ApplyGeometry(wantW_, wantH_);
}

void App::ApplyGeometry(float sw, float sh) {
    const bool first = (appliedW_ <= 0.0f);
    const bool wasLandscape = (appliedW_ > appliedH_);
    const bool nowLandscape = (sw > sh);
    const bool flip = !first && (wasLandscape != nowLandscape);

    if (!first) {
        LOGI("stream shape %.0fx%.0f accepted%s (%d frame%s held back)", sw, sh,
             flip ? " - rotating" : "", droppedWhileConfirming_,
             droppedWhileConfirming_ == 1 ? "" : "s");
    }
    droppedWhileConfirming_ = 0;
    wantW_ = wantH_ = 0.0f;

    appliedW_ = sw;
    appliedH_ = sh;
    RebuildDevice();

    if (flip) {
        StartRotation(true);
    } else {
        angleDeg_ = TargetAngle();
        scaleNow_ = 1.0f;
        rotating_ = false;
        orientSettled_ = NowSeconds();
        RecomputeVideoRect();
    }
    needsRedraw_ = true;
}

void App::StartRotation(bool contentWillChange) {
    const float target = TargetAngle();
    const float delta = WrapDeg(target - angleDeg_);
    const double now = NowSeconds();

    videoTo_ = FitVideo(PanelRectAt(dev_, target), appliedW_, appliedH_);

    // A real turn means the stream restarts in the new orientation; nothing is
    // shown until that content lands. An L flip reuses the picture we already
    // have, so it only needs to fade back in at the end.
    if (contentWillChange) {
        awaitingFrame_ = true;
        awaitStart_ = now;
    }

    if (std::fabs(delta) < 0.5f) {
        rotating_ = false;
        angleDeg_ = target;
        scaleNow_ = 1.0f;
        videoNow_ = videoTo_;
        return;
    }

    angleFrom_ = angleDeg_;
    angleTo_ = angleDeg_ + delta;
    rotStart_ = now;
    rotSeconds_ = kRotBaseSeconds + 0.0016 * std::fabs(delta);
    rotating_ = true;
}

void App::FinishRotation() {
    rotating_ = false;
    angleDeg_ = WrapDeg(angleTo_);
    scaleNow_ = 1.0f;
    videoNow_ = videoTo_;
    orientSettled_ = NowSeconds();

    // Release the frame that was held back through the turn - but only if it is
    // actually the new shape; a straggler from before the turn would just be the
    // old picture squashed into the new panel.
    if (pending_.Valid()) {
        if (SameShape((float)pending_.width, (float)pending_.height, appliedW_, appliedH_)) {
            AdoptFrame(std::move(pending_));
            awaitingFrame_ = false;
        }
        pending_.Reset();
    }
    // Nothing to do for the fade: clearing rotating_/awaitingFrame_ is what
    // lets the panel come back up on its own.
}

void App::AdoptFrame(VideoFrameRef &&f) {
    current_ = std::move(f);
    if (current_.isRGB) {
        renderer_.SetVideoBGRA(current_.rgba, current_.width, current_.height);
    } else {
        renderer_.SetVideoNV12(current_.y, current_.uv, current_.width, current_.height,
                               current_.texW, current_.texH, current_.cropX, current_.cropY,
                               current_.fullRange);
    }
}

void App::SetWindowSizeKeepCenter(int w, int h) {
    if (w <= 0 || h <= 0) return;
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    const int curW = rc.right - rc.left;
    const int curH = rc.bottom - rc.top;

    if (curW != w || curH != h) {
        int x = rc.left + curW / 2 - w / 2;
        int y = rc.top + curH / 2 - h / 2;

        // The square is a good deal larger than the device, so growing around
        // the centre can push it off the monitor - nudge it back on.
        MONITORINFO mi{sizeof(MONITORINFO)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
            const RECT &wa = mi.rcWork;
            if (w <= wa.right - wa.left) x = (int)Clampf((float)x, (float)wa.left,
                                                         (float)(wa.right - w));
            if (h <= wa.bottom - wa.top) y = (int)Clampf((float)y, (float)wa.top,
                                                         (float)(wa.bottom - h));
        }
        SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    gpu_.Resize((uint32_t)w, (uint32_t)h);
}

void App::ResizeWindow() {
    const int side = (int)std::ceil(WindowSideFor(dev_));
    SetWindowSizeKeepCenter(side, side);
}

bool App::HitsDevice(int clientX, int clientY) const {
    const float inv = 1.0f / std::max(scaleNow_, 0.05f);
    const float wx = ((float)clientX - gpu_.Width() * 0.5f) * inv;
    const float wy = ((float)clientY - gpu_.Height() * 0.5f) * inv;
    const float rad = angleDeg_ * 3.14159265358979f / 180.0f;
    const float c = std::cos(rad), s = std::sin(rad);
    const float dx = wx * c + wy * s;
    const float dy = -wx * s + wy * c;
    return SdRoundRect(dx, dy, dev_.bodyHx, dev_.bodyHy, dev_.bodyRadius) <= 1.0f;
}

// Reproduces the one condition that is hard to catch by eye: a picture whose
// width is not a multiple of 16, living in the larger surface D3D11VA hands
// back. The padding is zeroed, which through the BT.709 matrix is bright green.
bool App::MakeUvTestFrame() {
    const int w = 810, h = 1080;    // a real portrait iPad mirroring size
    const int tw = 816, th = 1088;  // what the decoder actually allocates for it

    std::vector<uint8_t> data((size_t)tw * th * 3 / 2, 0); // padding stays zero
    for (int y = 0; y < h; y++) memset(data.data() + (size_t)y * tw, 235, w);
    uint8_t *chroma = data.data() + (size_t)tw * th;
    for (int y = 0; y < h / 2; y++) memset(chroma + (size_t)y * tw, 128, w);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)tw;
    td.Height = (UINT)th;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = data.data();
    sd.SysMemPitch = (UINT)tw;

    if (FAILED(gpu_.Device()->CreateTexture2D(&td, &sd, uvTestTex_.put()))) {
        LOGE("uvtest: NV12 texture creation failed");
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    Com<ID3D11ShaderResourceView> y, uv;
    vd.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(gpu_.Device()->CreateShaderResourceView(uvTestTex_.get(), &vd, y.put())))
        return false;
    vd.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(gpu_.Device()->CreateShaderResourceView(uvTestTex_.get(), &vd, uv.put())))
        return false;

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

void App::UpdateIdleText() {
    std::string name;
    bool conn;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        conn = connected_;
        name = clientName_;
    }
    if (conn) {
        renderer_.SetStatusText(Widen(name.empty() ? "Connected" : name),
                                L"Waiting for video…");
    } else {
        renderer_.SetStatusText(Widen(server_.ServiceName()),
                                L"Control Centre → Screen Mirroring");
    }
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------
bool App::Animating() const {
    // Also true while a shape proposal is outstanding, so the confirmation
    // window is ticked at frame rate rather than at the idle poll interval.
    return rotating_ || fadeValue_ != fadeTarget_ || wantW_ > 0.0f ||
           NowSeconds() < flipDimUntil_;
}

bool App::Tick() {
    const double now = NowSeconds();
    const float dt = (lastTick_ > 0.0) ? (float)std::min(now - lastTick_, 0.10) : 1.0f / 60.0f;
    lastTick_ = now;

    if (clearVideoPending_.exchange(false)) {
        renderer_.ClearVideo();
        current_.Reset();
        pending_.Reset();
        streamShapeKnown_ = false;
        wantW_ = wantH_ = 0.0f;
        needsRedraw_ = true;
    }

    VideoFrameRef fresh;
    const bool haveNew = decoder_.PullNewest(fresh);
    if (haveNew) {
        // Every decoded frame is a vote on the stream's shape, alongside
        // video_report_size. Neither is trusted on its own.
        {
            std::lock_guard<std::mutex> lk(stateMutex_);
            streamW_ = (float)fresh.width;
            streamH_ = (float)fresh.height;
        }
        OnGeometryChanged();

        if (!SameShape((float)fresh.width, (float)fresh.height, appliedW_, appliedH_)) {
            // Either encoded before a change we are still confirming, or part of
            // one we have not accepted. Drawing it would stretch or letterbox the
            // picture for a moment, which is the flicker we are removing; holding
            // the last good frame instead is invisible over ~180 ms.
            droppedWhileConfirming_++;
            fresh.Reset();
        } else if (rotating_) {
            pending_ = std::move(fresh); // shown when the chassis settles
        } else {
            AdoptFrame(std::move(fresh));
            if (awaitingFrame_) {
                awaitingFrame_ = false;
                videoNow_ = videoTo_;
            }
        }
    }
    MaybeApplyGeometry(now);

    // Safety net: if the expected new-orientation frame never turns up, bring
    // back whatever we have rather than sitting on a dark panel forever.
    if (awaitingFrame_ && !rotating_ && (now - awaitStart_) > kAwaitTimeout) {
        awaitingFrame_ = false;
        videoNow_ = videoTo_;
        needsRedraw_ = true;
    }

    // ---- rotation ---------------------------------------------------------
    if (rotating_) {
        const float t = (float)((now - rotStart_) / rotSeconds_);
        if (t >= 1.0f) {
            FinishRotation();
        } else {
            angleDeg_ = Lerpf(angleFrom_, angleTo_, EaseInOut(t));
            // Shrink just enough to stay inside the fixed square window. Peaks
            // around 45 degrees and is exactly 1 at either end, so the window
            // never has to be resized part-way through the turn.
            scaleNow_ = SweepScale(devHx_, devHy_, fitHalf_, angleDeg_);
        }
    }

    // Anything meaning "the picture we hold is not what the device is showing"
    // dims the panel. The fade begins the moment a flip is *proposed*, so the
    // confirmation window is spent going dark rather than frozen on a stale
    // frame.
    //
    // The dim is held for kFlipDimHold rather than tracking the proposal
    // directly: confirmation works by letting a stale frame veto the proposal,
    // so wantW_ is armed, cleared and re-armed several times across one real
    // turn. Following that literally makes the panel bob back up between vetoes
    // - a visible flash right before the turn.
    if ((wantW_ > 0.0f) && ((wantW_ > wantH_) != (appliedW_ > appliedH_))) {
        flipDimUntil_ = now + kFlipDimHold;
    }
    fadeTarget_ =
        (rotating_ || awaitingFrame_ || now < flipDimUntil_) ? 0.0f : 1.0f;

    const bool animating = rotating_ || fadeValue_ != fadeTarget_;
    if (!haveNew && !animating && !needsRedraw_) return false;

    if (fadeValue_ < fadeTarget_) {
        fadeValue_ = std::min(fadeTarget_, fadeValue_ + dt / (float)kFadeInSeconds);
    } else {
        fadeValue_ = std::max(fadeTarget_, fadeValue_ - dt / (float)kFadeOutSeconds);
    }

    FrameParams fp;
    fp.centerX = gpu_.Width() * 0.5f;
    fp.centerY = gpu_.Height() * 0.5f;
    fp.angleDeg = angleDeg_;
    fp.scale = scaleNow_;
    fp.video = videoNow_;
    fp.video.cx *= scaleNow_;
    fp.video.cy *= scaleNow_;
    fp.video.hx *= scaleNow_;
    fp.video.hy *= scaleNow_;
    fp.videoAlpha = Smooth01(fadeValue_);

    renderer_.SetDevice(dev_);
    renderer_.SetFrame(fp);
    renderer_.Render();
    gpu_.Present(true);
    needsRedraw_ = false;
    return true;
}

// ---------------------------------------------------------------------------
// Window messages
// ---------------------------------------------------------------------------
LRESULT App::HandleMessage(UINT msg, WPARAM wp, LPARAM lp, bool &handled) {
    handled = true;
    switch (msg) {
    case WM_AM_RELAYOUT:
        OnGeometryChanged();
        return 0;

    case WM_AM_CLIENT:
        UpdateIdleText();
        needsRedraw_ = true;
        return 0;

    case WM_NCHITTEST: {
        // The window is a square that fits the device either way up, so it is
        // always bigger than the chassis; only the chassis catches the mouse.
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        return HitsDevice(pt.x, pt.y) ? HTCAPTION : HTTRANSPARENT;
    }

    case WM_RBUTTONUP:
        ShowContextMenu(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kMenuFlip:
            landscapeFlip_ = !landscapeFlip_;
            StartRotation(false);
            needsRedraw_ = true;
            break;
        case kMenuMute:
            audio_.SetMuted(!audio_.Muted());
            break;
        case kMenuZoomIn:
            userScale_ = Clampf(userScale_ * 1.1f, 0.3f, 3.0f);
            RebuildDevice();
            RecomputeVideoRect();
            needsRedraw_ = true;
            break;
        case kMenuZoomOut:
            userScale_ = Clampf(userScale_ / 1.1f, 0.3f, 3.0f);
            RebuildDevice();
            RecomputeVideoRect();
            needsRedraw_ = true;
            break;
        case kMenuResetZoom:
            userScale_ = 1.0f;
            RebuildDevice();
            RecomputeVideoRect();
            needsRedraw_ = true;
            break;
        case kMenuQuit:
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        }
        return 0;

    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        userScale_ = Clampf(userScale_ * (delta > 0 ? 1.08f : 1.0f / 1.08f), 0.3f, 3.0f);
        RebuildDevice();
        RecomputeVideoRect();
        needsRedraw_ = true;
        return 0;
    }

    case WM_KEYDOWN:
        switch (wp) {
        case 'L':
            landscapeFlip_ = !landscapeFlip_;
            StartRotation(false);
            needsRedraw_ = true;
            break;
        case 'M':
            audio_.SetMuted(!audio_.Muted());
            break;
        case 'R': {
            // Swap the stream's orientation. Mainly a preview aid for checking
            // the pivot; with a live client the next frame corrects it.
            std::lock_guard<std::mutex> lk(stateMutex_);
            std::swap(streamW_, streamH_);
            PostMessageW(hwnd_, WM_AM_RELAYOUT, 0, 0);
            break;
        }
        case VK_OEM_PLUS:
        case VK_ADD:
            userScale_ = Clampf(userScale_ * 1.1f, 0.3f, 3.0f);
            RebuildDevice();
            RecomputeVideoRect();
            needsRedraw_ = true;
            break;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            userScale_ = Clampf(userScale_ / 1.1f, 0.3f, 3.0f);
            RebuildDevice();
            RecomputeVideoRect();
            needsRedraw_ = true;
            break;
        case '0':
            userScale_ = 1.0f;
            RebuildDevice();
            RecomputeVideoRect();
            needsRedraw_ = true;
            break;
        case VK_ESCAPE:
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        }
        return 0;

    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        RebuildDevice();
        RecomputeVideoRect();
        needsRedraw_ = true;
        return 0;
    }

    handled = false;
    return 0;
}

void App::ShowContextMenu(int x, int y) {
    POINT pt{x, y};
    ClientToScreen(hwnd_, &pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuFlip, L"Flip landscape orientation\tL");
    AppendMenuW(menu, MF_STRING | (audio_.Muted() ? MF_CHECKED : 0), kMenuMute,
                L"Mute audio\tM");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuZoomIn, L"Zoom in\t+");
    AppendMenuW(menu, MF_STRING, kMenuZoomOut, L"Zoom out\t-");
    AppendMenuW(menu, MF_STRING, kMenuResetZoom, L"Reset size\t0");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit\tEsc");

    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

// ---------------------------------------------------------------------------
// AirPlaySink - all of these arrive on libairplay threads
// ---------------------------------------------------------------------------
void App::OnClientRequest(const std::string &deviceId, const std::string &model,
                          const std::string &name, bool &admit) {
    (void)deviceId;
    admit = true;
    LOGI("client connecting: %s (%s)", name.c_str(), model.c_str());

    DeviceProfile p = ResolveDevice(model, name);
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        profile_ = p;
        clientName_ = name.empty() ? p.displayName : name;
    }
    PostMessageW(hwnd_, WM_AM_RELAYOUT, 0, 0);
    PostMessageW(hwnd_, WM_AM_CLIENT, 0, 0);
}

void App::OnConnectionOpened() {
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        connected_ = true;
    }
    PostMessageW(hwnd_, WM_AM_CLIENT, 0, 0);
}

void App::OnConnectionClosed() {
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        connected_ = false;
    }
    decoder_.Flush();
    audio_.Flush();

    // renderer_ and current_ belong to the UI thread - tearing down the video
    // views from here would race with a Render() in progress.
    clearVideoPending_.store(true);
    PostMessageW(hwnd_, WM_AM_CLIENT, 0, 0);
    SetEvent(frameEvent_);
}

bool App::OnVideoCodec(bool h265) { return decoder_.SetCodec(h265); }

void App::OnVideoSize(float srcW, float srcH, float w, float h) {
    const float useW = (w > 0.0f) ? w : srcW;
    const float useH = (h > 0.0f) ? h : srcH;
    if (useW <= 0.0f || useH <= 0.0f) return;

    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (std::fabs(streamW_ - useW) > 0.5f || std::fabs(streamH_ - useH) > 0.5f) {
            streamW_ = useW;
            streamH_ = useH;
            changed = true;
        }
    }
    // This arrives with the new SPS/PPS, a frame or two before the first
    // picture in the new orientation - the earliest possible cue to start the
    // pivot, which is why the animation is driven from here and not from the
    // decoder output.
    if (changed) {
        LOGI("stream geometry now %.0fx%.0f", useW, useH);
        PostMessageW(hwnd_, WM_AM_RELAYOUT, 0, 0);
    }
}

void App::OnVideoData(const uint8_t *data, int len) {
    decoder_.Decode(data, len);
    SetEvent(frameEvent_);
}

void App::OnVideoFlush() {
    decoder_.Flush();
    SetEvent(frameEvent_);
}

void App::OnVideoPause(bool paused) {
    std::lock_guard<std::mutex> lk(stateMutex_);
    paused_ = paused;
}

void App::OnAudioFormat(unsigned char ct) { audio_.SetFormat(ct); }

void App::OnAudioData(const uint8_t *data, int len) { audio_.Submit(data, len); }

void App::OnAudioFlush() { audio_.Flush(); }

void App::OnVolume(float db) { audio_.SetVolumeDb(db); }
