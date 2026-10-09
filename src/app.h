// AirMirror - application state: owns the AirPlay server, the media pipeline
// and the renderer, and pivots the whole device when the real one is turned.
#pragma once

#include "airplay_server.h"
#include "audio_player.h"
#include "common.h"
#include "device_db.h"
#include "gpu.h"
#include "renderer.h"
#include "skin.h"
#include "video_decoder.h"

#include <atomic>
#include <map>
#include <mutex>

// Posted from libairplay threads to the UI thread.
#define WM_AM_RELAYOUT (WM_APP + 1)
#define WM_AM_CLIENT (WM_APP + 2)

struct AppOptions {
    std::string serviceName;              // default: this PC's name
    unsigned short adWidth = 1920;        // display size advertised to the client
    unsigned short adHeight = 1080;
    unsigned short adFps = 60;
    bool allowH265 = false;
    float startScale = 1.0f;

    // Preview helpers: show a device frame with no client attached, so a skin
    // can be checked in both orientations.
    std::string previewModel;              // e.g. "iPad13,4" or "iPhone16,1"
    int previewW = 0, previewH = 0;        // fake stream size, sets orientation

    // Self-check: display a synthetic 810x1080 picture inside the 816x1088
    // surface D3D11VA would allocate for it, with the alignment padding zeroed.
    // Zeroed NV12 decodes as bright green, so any green edge means the renderer
    // is sampling past the picture.
    bool uvTest = false;
};

class App : public AirPlaySink {
  public:
    bool Init(HWND hwnd, const AppOptions &opts);
    void Shutdown();

    // Returns true if it drew this iteration.
    bool Tick();
    bool Animating() const;
    HANDLE FrameEvent() const { return frameEvent_; }

    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp, bool &handled);

    // ---- AirPlaySink ----
    void OnClientRequest(const std::string &deviceId, const std::string &model,
                         const std::string &name, bool &admit) override;
    void OnConnectionOpened() override;
    void OnConnectionClosed() override;
    bool OnVideoCodec(bool h265) override;
    void OnVideoSize(float srcW, float srcH, float w, float h) override;
    void OnVideoData(const uint8_t *data, int len) override;
    void OnVideoFlush() override;
    void OnVideoPause(bool paused) override;
    void OnAudioFormat(unsigned char ct) override;
    void OnAudioData(const uint8_t *data, int len) override;
    void OnAudioFlush() override;
    void OnVolume(float db) override;

  private:
    // Geometry / orientation
    void RebuildDevice();
    void OnGeometryChanged();            // a new stream size was observed
    void MaybeApplyGeometry(double now); // act on it once it has held steady
    void ApplyGeometry(float sw, float sh);
    // Begin a pivot towards the target angle. contentWillChange says whether a
    // stream in the new orientation is on its way (a real device turn) or the
    // picture is unchanged and only the chassis moves (the L flip).
    void StartRotation(bool contentWillChange);
    void FinishRotation();
    void AdoptFrame(VideoFrameRef &&f);
    void RecomputeVideoRect();
    float TargetAngle() const;
    float WorkAreaLimit() const;
    void ResizeWindow();
    void SetWindowSizeKeepCenter(int w, int h);
    bool HitsDevice(int clientX, int clientY) const;

    bool MakeUvTestFrame();
    void UpdateIdleText();
    void ShowContextMenu(int x, int y);
    float BaseLongEdge() const;
    const Skin *SkinFor(const DeviceProfile &p);

    HWND hwnd_ = nullptr;
    Gpu gpu_;
    Renderer renderer_;
    VideoDecoder decoder_;
    AudioPlayer audio_;
    AirPlayServer server_;

    VideoFrameRef current_;
    // Frames decoded while the chassis is turning are parked here instead of
    // being shown: the panel is dark through the pivot, and pushing landscape
    // content into a still-portrait panel is what used to look like the screen
    // rotating before the device did.
    VideoFrameRef pending_;
    Com<ID3D11Texture2D> uvTestTex_;
    HANDLE frameEvent_ = nullptr;

    // Guarded because libairplay threads write them.
    std::mutex stateMutex_;
    DeviceProfile profile_;
    float streamW_ = 0, streamH_ = 0;
    bool connected_ = false;
    bool paused_ = false;
    std::string clientName_;

    // UI-thread state
    std::map<std::string, Skin> skins_;
    DeviceLayout dev_;
    float devHx_ = 0, devHy_ = 0;        // drawn half-extent, device space
    float fitHalf_ = 0;                  // half-side of the square window's device area
    float appliedW_ = 0, appliedH_ = 0;  // stream dims the layout was built for
    bool landscapeFlip_ = false;
    float userScale_ = 1.0f;

    // The stream's shape is a noisy signal: around a turn iOS emits trailing
    // frames at the old size, and this iPad also drifts between 4:3 and 16:9 on
    // its own. A proposal must hold steady before the UI acts on it.
    float wantW_ = 0, wantH_ = 0;
    double wantSince_ = 0.0;
    double orientSettled_ = -1.0e9;
    bool streamShapeKnown_ = false;
    int droppedWhileConfirming_ = 0;

    // Rotation animation
    float angleDeg_ = 0.0f;
    float angleFrom_ = 0.0f, angleTo_ = 0.0f;
    float scaleNow_ = 1.0f;
    double rotStart_ = -1.0;
    double rotSeconds_ = 0.42;
    bool rotating_ = false;
    RectF videoTo_, videoNow_;
    bool awaitingFrame_ = false;
    double awaitStart_ = 0.0;

    // Panel brightness is driven declaratively: anything that means "what we
    // have is not what the device is showing" pulls the target to 0, and it
    // eases back to 1 on its own once that clears.
    float fadeValue_ = 1.0f, fadeTarget_ = 1.0f;
    double flipDimUntil_ = -1.0e9;
    double lastTick_ = 0.0;

    static constexpr double kRotBaseSeconds = 0.30;   // plus 0.0016 s per degree
    static constexpr double kFadeOutSeconds = 0.11;
    static constexpr double kFadeInSeconds = 0.17;
    static constexpr double kAwaitTimeout = 1.5;
    static constexpr double kConfirmSeconds = 0.18;   // shape must hold this long
    static constexpr double kOrientHold = 0.50;       // quiet period after a turn
    static constexpr double kFlipDimHold = 0.40;      // rides out veto/re-arm chatter

    bool needsRedraw_ = true;
    // Set by libairplay threads, consumed on the UI thread in Tick().
    std::atomic<bool> clearVideoPending_{false};
};
