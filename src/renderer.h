// AirMirror - draws the mirrored screen inside a device frame.
//
// Geometry is expressed in the device's own portrait frame ("device space")
// and the whole chassis is rotated by an angle at draw time, so turning the
// iPad is a continuous pivot rather than a swap between two fixed layouts.
// The video is always drawn upright in window space and clipped to the
// (rotating) panel, because content must stay readable however the device is
// held - exactly what a real device does when its UI counter-rotates.
#pragma once

#include "common.h"
#include "device_db.h"
#include "gpu.h"
#include "skin.h"

// A rectangle as centre + half-extent. Offsets are relative to the device
// centre unless stated otherwise.
struct RectF {
    float cx = 0, cy = 0, hx = 0, hy = 0;
};

// Chassis geometry in device space: portrait, centred on the body's centre.
// Independent of orientation - rotation is applied when drawing.
struct DeviceLayout {
    float bodyHx = 0, bodyHy = 0, bodyRadius = 0;

    // Panel: the glass area. With a PNG skin this is the transparent cutout.
    float scrCx = 0, scrCy = 0, scrHx = 0, scrHy = 0, scrRadius = 0;

    int islandMode = 0; // 0 none, 1 notch, 2 dynamic island
    float islandCx = 0, islandCy = 0, islandHx = 0, islandHy = 0, islandRadius = 0;
    float homeCx = 0, homeCy = 0, homeR = 0;
    float earCx = 0, earCy = 0, earHx = 0, earHy = 0;

    bool useSkin = false;
    float skinCx = 0, skinCy = 0, skinHx = 0, skinHy = 0;

    // Contact shadow. The falloff reaches exactly zero at shadowRadius past the
    // body edge, so as long as the window leaves that much room the shadow
    // never gets clipped into a hard line.
    float shadowRadius = 0, shadowDrop = 0;
};

// Per-frame state that does change with orientation and animation.
struct FrameParams {
    float centerX = 0, centerY = 0; // device centre, in window pixels
    float angleDeg = 0;             // 0 = portrait, -90 = top edge to the left
    float scale = 1.0f;             // uniform shrink, used mid-pivot (see SweepScale)
    RectF video;                    // upright, window pixels
    float videoAlpha = 1.0f;
};

// Builds the chassis for a device whose panel long edge should be longPx on
// screen. streamW/streamH only supply the panel's aspect ratio (the same in
// either orientation), and are ignored when a skin fixes the cutout.
DeviceLayout ComputeDeviceLayout(const DeviceProfile &p, const Skin *skin,
                                 float streamW, float streamH, float longPx);

// Half-extent of everything that gets drawn (skin image or chassis), used for
// window sizing and hit-testing.
void DeviceExtent(const DeviceLayout &L, float &hx, float &hy);

// Clearance the shadow needs outside the device extent.
float ShadowMargin(const DeviceLayout &L);

// Axis-aligned half-extent once rotated by angleDeg.
void RotatedExtent(float hx, float hy, float angleDeg, float &ax, float &ay);

// Largest uniform scale at which an hx*hy device turned to angleDeg still fits
// a square of half-side fitHalf. Exactly 1 at the quadrant angles and dipping
// around 45 degrees, which is what lets the window stay one fixed size for the
// whole pivot instead of being resized (and glitching) part-way through.
float SweepScale(float hx, float hy, float fitHalf, float angleDeg);

// Panel rectangle, as an offset from the device centre, at a quadrant angle.
RectF PanelRectAt(const DeviceLayout &L, float angleDeg);

// Largest streamW:streamH rectangle that fits inside panel, centred on it.
RectF FitVideo(const RectF &panel, float streamW, float streamH);

class Renderer {
  public:
    bool Init(Gpu *gpu);
    void Shutdown();

    void SetDevice(const DeviceLayout &l) { device_ = l; }
    void SetFrame(const FrameParams &f) { frame_ = f; }
    void SetProfile(const DeviceProfile &p) { profile_ = p; }
    void SetSkin(const Skin *s) { skin_ = s; }

    // Zero-copy path: two views onto one NV12 surface. The picture is the
    // w x h region at (cropX, cropY) within a texW x texH surface; everything
    // else in there is decoder padding and must never be sampled.
    void SetVideoNV12(const TexView &y, const TexView &uv, int w, int h, int texW,
                      int texH, int cropX, int cropY, bool fullRange);
    // Software fallback path.
    void SetVideoBGRA(const TexView &rgba, int w, int h);
    void ClearVideo();

    // UTF-8.
    void SetStatusText(const std::string &title, const std::string &subtitle);

    void Render();

  private:
    struct FrameCB {
        float viewport[2];
        float deviceCenter[2];
        float bodyHalf[2];
        float scrCenter[2];
        float scrHalf[2];
        float islandCenter[2];
        float islandHalf[2];
        float homeCenter[2];
        float earHalf[2];
        float earCenter[2];
        float skinCenter[2];
        float skinHalf[2];
        float videoCenter[2];
        float videoHalf[2];
        float bodyRadius;
        float scrRadius;
        float islandRadius;
        float islandMode;
        float homeR;
        float hasVideo;
        float shadowAlpha;
        float videoIsRGB;
        float videoFullRange;
        float videoTexel[2];
        float superSample;
        float useSkin;
        float cosA;
        float sinA;
        float videoAlpha;
        float shadowRadius;
        float shadowDrop;
        float invScale;
        float pad0;
        float videoUvOrigin[2];
        float videoUvScale[2];
        float videoUvClamp[2];
        float pad1[2];
        float bodyColor[4];
        float railColor[4];
    };
    static_assert(sizeof(FrameCB) % 16 == 0, "cbuffer must be 16-byte aligned");

    struct QuadCB {
        float rect[4];
        float tint[4];
    };

    // Everything the frame shader needs, for a viewport of vpW x vpH pixels.
    void FillFrameCB(FrameCB &cb, float vpW, float vpH) const;
    void DrawText(const TexView &srv, int w, int h, float cx, float cy, float alpha);

    Gpu *gpu_ = nullptr;
    DeviceLayout device_;
    FrameParams frame_;
    DeviceProfile profile_;
    const Skin *skin_ = nullptr;

#ifdef _WIN32
    Com<ID3D11VertexShader> vsFull_, vsQuad_;
    Com<ID3D11PixelShader> psFrame_, psText_;
    Com<ID3D11Buffer> cbFrame_, cbQuad_;
    Com<ID3D11SamplerState> sampler_;
    Com<ID3D11BlendState> blendPremul_;
    Com<ID3D11RasterizerState> raster_;
#else
    id<MTLRenderPipelineState> pipeFrame_ = nil, pipeText_ = nil;
    id<MTLSamplerState> sampler_ = nil;
    id<MTLTexture> dummy_ = nil;                 // bound to unused texture slots
    id<MTLRenderCommandEncoder> enc_ = nil;      // only valid inside Render()
    FrameCB lastCB_{};                           // DrawText needs the viewport
#endif

    TexView videoY_, videoUV_, videoRGBA_;
    int videoW_ = 0, videoH_ = 0;
    int videoTexW_ = 0, videoTexH_ = 0;
    int videoCropX_ = 0, videoCropY_ = 0;
    bool videoIsRGB_ = false;
    bool videoFullRange_ = false;
    bool hasVideo_ = false;

    TexView titleTex_, subTex_;
    int titleW_ = 0, titleH_ = 0, subW_ = 0, subH_ = 0;
    std::string titleStr_, subStr_;
};
