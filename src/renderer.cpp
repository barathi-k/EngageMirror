#include "renderer.h"

#include <algorithm>
#include <cmath>

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Shaders (the Metal port of these lives in renderer_mac.mm)
// ---------------------------------------------------------------------------
static const char *kShaderSrc = R"HLSL(
cbuffer Frame : register(b0)
{
    float2 viewport;
    float2 deviceCenter;
    float2 bodyHalf;
    float2 scrCenter;
    float2 scrHalf;
    float2 islandCenter;
    float2 islandHalf;
    float2 homeCenter;
    float2 earHalf;
    float2 earCenter;
    float2 skinCenter;
    float2 skinHalf;
    float2 videoCenter;
    float2 videoHalf;
    float  bodyRadius;
    float  scrRadius;
    float  islandRadius;
    float  islandMode;
    float  homeR;
    float  hasVideo;
    float  shadowAlpha;
    float  videoIsRGB;
    float  videoFullRange;
    float2 videoTexel;
    float  superSample;
    float  useSkin;
    float  cosA;
    float  sinA;
    float  videoAlpha;
    float  shadowRadius;
    float  shadowDrop;
    float  invScale;
    float  pad0;
    float2 videoUvOrigin;
    float2 videoUvScale;
    float2 videoUvClamp;
    float2 pad1;
    float4 bodyColor;
    float4 railColor;
};

cbuffer Quad : register(b1)
{
    float4 quadRect;   // x, y, w, h in pixels
    float4 quadTint;
};

Texture2D<float>  texY    : register(t0);
Texture2D<float2> texUV   : register(t1);
Texture2D<float4> texRGBA : register(t2);
Texture2D<float4> texSkin : register(t3);
SamplerState samp         : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSFull(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

VSOut VSQuad(uint id : SV_VertexID)
{
    VSOut o;
    float2 c  = float2((id == 1 || id == 3) ? 1.0 : 0.0, (id >= 2) ? 1.0 : 0.0);
    float2 px = quadRect.xy + c * quadRect.zw;
    o.uv  = c;
    o.pos = float4(px / viewport * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Window space -> device space (undo the chassis rotation).
float2 ToDevice(float2 v) { return float2(v.x * cosA + v.y * sinA, -v.x * sinA + v.y * cosA); }

// Cheap stable hash, used to dither the shadow's tail.
float Hash12(float2 p)
{
    float3 q = frac(float3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}

float sdRoundRect(float2 p, float2 b, float r)
{
    r = min(r, min(b.x, b.y));
    float2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float3 SampleVideoOnce(float2 uv)
{
    // Never outside the picture: everything else in the surface is padding.
    uv = clamp(uv, videoUvOrigin, videoUvClamp);

    if (videoIsRGB > 0.5)
        return texRGBA.Sample(samp, uv).rgb;

    float  y   = texY.Sample(samp, uv);
    float2 uvc = texUV.Sample(samp, uv);
    float  u   = uvc.x - 0.5;
    float  v   = uvc.y - 0.5;

    if (videoFullRange < 0.5)
    {
        y = (y - 0.0627451) * 1.1643836;   // 16..235 -> 0..1
        u *= 1.1383929;                    // 16..240 -> -0.5..0.5
        v *= 1.1383929;
    }

    // BT.709
    float3 c;
    c.r = y + 1.5748 * v;
    c.g = y - 0.1873 * u - 0.4681 * v;
    c.b = y + 1.8556 * u;
    return saturate(c);
}

float3 SampleVideo(float2 uv)
{
    if (superSample < 0.5)
        return SampleVideoOnce(uv);

    // Rotated-grid 2x2 supersample: the panel is usually minified a lot
    // (a 2048-wide stream shown at ~800px), and plain bilinear shimmers.
    float2 d = videoTexel * superSample * 0.25;
    float3 s  = SampleVideoOnce(uv + float2(-d.x,  d.y));
    s        += SampleVideoOnce(uv + float2( d.x,  d.y));
    s        += SampleVideoOnce(uv + float2(-d.x, -d.y));
    s        += SampleVideoOnce(uv + float2( d.x, -d.y));
    return s * 0.25;
}

float4 PSFrame(VSOut i) : SV_Target
{
    float2 p = i.uv * viewport;
    float2 w = p - deviceCenter;         // window space, centred on the device
    float2 d = ToDevice(w) * invScale;   // device space (always portrait, unit scale)

    float3 col = float3(0.0, 0.0, 0.0);
    float  a   = 0.0;

    // Soft contact shadow. The drop is a window-space offset, so the light
    // stays overhead while the device turns; ToDevice is linear, so undoing the
    // scale turns that into a plain device-space offset.
    if (shadowAlpha > 0.001 && shadowRadius > 0.5)
    {
        float2 dsh = d - ToDevice(float2(0.0, shadowDrop));
        float  ds  = sdRoundRect(dsh, bodyHalf + 2.0, bodyRadius + 2.0);
        // Falls to exactly zero (with zero slope) at shadowRadius, so the
        // window edge never cuts the shadow off mid-gradient.
        float  x   = saturate(1.0 - ds / shadowRadius);
        a = shadowAlpha * x * x;
        // The tail only changes by one 8-bit level every pixel or so, which on a
        // flat background reads as a thin contour line where the shadow starts.
        // A sub-level dither turns that edge into noise below the eye's floor.
        if (a > 0.0) a = max(0.0, a + (Hash12(p) - 0.5) * (1.0 / 255.0));
    }

    // Panel, in device space so it turns with the chassis.
    float dsc  = sdRoundRect(d - scrCenter, scrHalf, scrRadius);
    float panA = saturate(0.5 - dsc);

    // Video: upright in window space, clipped to the panel.
    float3 vidC = float3(0.0, 0.0, 0.0);
    float  vidA = 0.0;
    if (hasVideo > 0.5 && videoAlpha > 0.002 && videoHalf.x > 0.5 && videoHalf.y > 0.5)
    {
        float dv = sdRoundRect(p - videoCenter, videoHalf, 0.0);
        vidA = saturate(0.5 - dv) * panA * videoAlpha;
        float2 uv = saturate((p - (videoCenter - videoHalf)) / (2.0 * videoHalf));
        vidC = SampleVideo(videoUvOrigin + uv * videoUvScale);
    }

    // ---- PNG skin -------------------------------------------------------
    if (useSkin > 0.5)
    {
        col = col * (1.0 - panA) + float3(0.012, 0.012, 0.015) * panA;
        a   = a   * (1.0 - panA) + panA;

        col = col * (1.0 - vidA) + vidC * vidA;
        a   = a   * (1.0 - vidA) + vidA;

        // The skin is authored portrait, and device space *is* portrait, so
        // the lookup needs no rotation of its own.
        float2 su = (d - (skinCenter - skinHalf)) / (2.0 * skinHalf);
        float4 sk = float4(0.0, 0.0, 0.0, 0.0);
        if (su.x >= 0.0 && su.x <= 1.0 && su.y >= 0.0 && su.y <= 1.0)
            sk = texSkin.Sample(samp, su);   // premultiplied

        col = sk.rgb + col * (1.0 - sk.a);
        a   = sk.a   + a   * (1.0 - sk.a);
        return float4(col, a);
    }

    // ---- procedural chassis ---------------------------------------------
    float db    = sdRoundRect(d, bodyHalf, bodyRadius);
    float bodyA = saturate(0.5 - db);

    float3 bodyC = bodyColor.rgb;
    float  rail  = saturate(1.0 - abs(db + 1.3) / 1.5);
    float2 n     = normalize(d + float2(1e-5, 1e-5));
    float  sheen = 0.55 + 0.45 * abs(n.x * 0.85 + n.y * 0.35);
    bodyC = lerp(bodyC, railColor.rgb * sheen, rail);

    col = col * (1.0 - bodyA) + bodyC * bodyA;
    a   = a   * (1.0 - bodyA) + bodyA;

    col = col * (1.0 - panA) + float3(0.016, 0.016, 0.020) * panA;
    a   = a   * (1.0 - panA) + panA;

    col = col * (1.0 - vidA) + vidC * vidA;
    a   = a   * (1.0 - vidA) + vidA;

    // Earpiece slot (home-button hardware).
    if (earHalf.x > 0.5)
    {
        float de = sdRoundRect(d - earCenter, earHalf, earHalf.y);
        float ea = saturate(0.5 - de);
        col = col * (1.0 - ea) + float3(0.03, 0.03, 0.035) * ea;
        a   = max(a, ea);
    }

    // Home button.
    if (homeR > 0.5)
    {
        float dh   = length(d - homeCenter) - homeR;
        float fill = saturate(0.5 - dh);
        float ring = saturate(1.4 - abs(dh));
        col = col * (1.0 - fill) + bodyColor.rgb * 0.82 * fill;
        a   = max(a, fill);
        col = col * (1.0 - ring) + railColor.rgb * 0.85 * ring;
        a   = max(a, ring);
    }

    // Notch or Dynamic Island, painted over the video.
    if (islandMode > 0.5)
    {
        float di = sdRoundRect(d - islandCenter, islandHalf, islandRadius);
        if (islandMode < 1.5)
            di = max(di, dsc);        // a notch is clipped to the panel edge

        float  ia = saturate(0.5 - di);
        float3 ic = float3(0.012, 0.012, 0.016);

        float2 lens  = d - (islandCenter + float2(islandHalf.x * 0.60, 0.0));
        float  lensD = length(lens) - min(islandHalf.y, islandHalf.x) * 0.40;
        ic = lerp(ic, float3(0.09, 0.10, 0.13), saturate(0.5 - lensD) * 0.75);

        col = col * (1.0 - ia) + ic * ia;
        a   = max(a, ia);
    }

    return float4(col, a);
}

float4 PSText(VSOut i) : SV_Target
{
    return texRGBA.Sample(samp, i.uv) * quadTint;
}
)HLSL";
#endif // _WIN32

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
// Shadow geometry, derived from the body so it scales with the window.
static void SetShadow(DeviceLayout &L) {
    L.shadowRadius = Clampf(std::min(L.bodyHx, L.bodyHy) * 0.16f, 10.0f, 64.0f);
    L.shadowDrop = std::max(L.bodyHx, L.bodyHy) * 0.045f;
}

float ShadowMargin(const DeviceLayout &L) { return L.shadowRadius + L.shadowDrop + 4.0f; }

DeviceLayout ComputeDeviceLayout(const DeviceProfile &p, const Skin *skin,
                                 float streamW, float streamH, float longPx) {
    DeviceLayout L;

    if (skin && skin->valid) {
        L.useSkin = true;

        // Scale so the cutout's long edge matches the requested size, which
        // keeps a skinned iPad the same visual size as a procedural iPhone.
        const float cutLong = (float)std::max(skin->ScreenW(), skin->ScreenH());
        const float k = (cutLong > 0.0f) ? longPx / cutLong : 1.0f;

        const float bodyCx = (skin->bodyX0 + skin->bodyX1 + 1) * 0.5f;
        const float bodyCy = (skin->bodyY0 + skin->bodyY1 + 1) * 0.5f;

        L.bodyHx = (skin->bodyX1 - skin->bodyX0 + 1) * 0.5f * k;
        L.bodyHy = (skin->bodyY1 - skin->bodyY0 + 1) * 0.5f * k;
        L.bodyRadius = skin->cornerRadius * k;

        L.scrCx = ((skin->scrX0 + skin->scrX1 + 1) * 0.5f - bodyCx) * k;
        L.scrCy = ((skin->scrY0 + skin->scrY1 + 1) * 0.5f - bodyCy) * k;
        L.scrHx = skin->ScreenW() * 0.5f * k;
        L.scrHy = skin->ScreenH() * 0.5f * k;
        L.scrRadius = 0.0f; // the skin's own artwork rounds the corners

        L.skinCx = (skin->imgW * 0.5f - bodyCx) * k;
        L.skinCy = (skin->imgH * 0.5f - bodyCy) * k;
        L.skinHx = skin->imgW * 0.5f * k;
        L.skinHy = skin->imgH * 0.5f * k;
        SetShadow(L);
        return L;
    }

    // Panel aspect comes from the stream; it is the same either way up.
    float sol = 0.5f;
    if (streamW > 0.0f && streamH > 0.0f) {
        sol = std::min(streamW, streamH) / std::max(streamW, streamH);
    } else if (p.defaultAspect > 0.0f) {
        sol = std::min(p.defaultAspect, 1.0f / p.defaultAspect);
    }

    const float dh = longPx;   // panel long edge (device-space height)
    const float dw = longPx * sol;
    const float s = dw;        // every ratio in DeviceProfile is relative to this

    const float bl = p.bezelSide * s;
    const float br = p.bezelSide * s;
    const float bt = p.bezelTop * s;
    const float bb = p.bezelBottom * s;

    L.bodyHx = (dw + bl + br) * 0.5f;
    L.bodyHy = (dh + bt + bb) * 0.5f;
    L.bodyRadius = (p.screenRadius + p.bezelSide + p.bodyRadiusPad) * s;

    L.scrCx = 0.0f;
    L.scrCy = (bt - bb) * 0.5f; // uneven bezels push the panel off-centre
    L.scrHx = dw * 0.5f;
    L.scrHy = dh * 0.5f;
    L.scrRadius = p.screenRadius * s;

    L.islandMode = (p.island == Island::DynamicIsland) ? 2
                   : (p.island == Island::Notch)       ? 1
                                                       : 0;
    if (L.islandMode) {
        const float iw = p.islandWidth * s;
        const float ih = p.islandHeight * s;
        L.islandCx = 0.0f;
        L.islandHx = iw * 0.5f;
        if (L.islandMode == 2) { // pill, inset from the top edge
            L.islandHy = ih * 0.5f;
            L.islandCy = L.scrCy - dh * 0.5f + p.islandInset * s + L.islandHy;
            L.islandRadius = L.islandHy;
        } else { // notch, hanging off the top edge and clipped to the panel
            L.islandHy = ih;
            L.islandCy = L.scrCy - dh * 0.5f;
            L.islandRadius = std::min(0.055f * s, ih * 0.85f);
        }
    }

    if (p.homeButton && bb > 0.0f) {
        L.homeCx = 0.0f;
        L.homeCy = L.scrCy + dh * 0.5f + bb * 0.5f;
        L.homeR = p.homeButtonRadius * s;
    }
    if (p.earpieceWidth > 0.0f && bt > 0.0f) {
        L.earCx = 0.0f;
        L.earCy = L.scrCy - (dh * 0.5f + bt * 0.45f);
        L.earHx = p.earpieceWidth * s * 0.5f;
        L.earHy = p.earpieceHeight * s * 0.5f;
    }
    SetShadow(L);
    return L;
}

void DeviceExtent(const DeviceLayout &L, float &hx, float &hy) {
    hx = L.bodyHx;
    hy = L.bodyHy;
    if (L.useSkin) {
        hx = std::max(hx, std::fabs(L.skinCx) + L.skinHx);
        hy = std::max(hy, std::fabs(L.skinCy) + L.skinHy);
    }
}

void RotatedExtent(float hx, float hy, float angleDeg, float &ax, float &ay) {
    const float r = angleDeg * 3.14159265358979f / 180.0f;
    const float c = std::fabs(std::cos(r));
    const float s = std::fabs(std::sin(r));
    ax = hx * c + hy * s;
    ay = hx * s + hy * c;
}

float SweepScale(float hx, float hy, float fitHalf, float angleDeg) {
    if (fitHalf <= 1.0f) return 1.0f;
    float ax, ay;
    RotatedExtent(hx, hy, angleDeg, ax, ay);
    const float need = std::max(ax, ay);
    return (need <= fitHalf) ? 1.0f : fitHalf / need;
}

RectF PanelRectAt(const DeviceLayout &L, float angleDeg) {
    const float r = angleDeg * 3.14159265358979f / 180.0f;
    const float c = std::cos(r);
    const float s = std::sin(r);

    RectF out;
    out.cx = L.scrCx * c - L.scrCy * s;
    out.cy = L.scrCx * s + L.scrCy * c;
    // Only ever called at quadrant angles, so the extent simply swaps.
    if (std::fabs(c) > 0.5f) {
        out.hx = L.scrHx;
        out.hy = L.scrHy;
    } else {
        out.hx = L.scrHy;
        out.hy = L.scrHx;
    }
    return out;
}

RectF FitVideo(const RectF &panel, float streamW, float streamH) {
    RectF v = panel;
    if (streamW <= 0.0f || streamH <= 0.0f) return v;
    const float k = std::min(panel.hx / streamW, panel.hy / streamH);
    v.hx = streamW * k;
    v.hy = streamH * k;
    return v;
}

// ---------------------------------------------------------------------------
// Renderer - shared state handling
// ---------------------------------------------------------------------------
void Renderer::SetVideoNV12(const TexView &y, const TexView &uv, int w, int h, int texW,
                            int texH, int cropX, int cropY, bool fullRange) {
    videoY_ = y;
    videoUV_ = uv;
    videoRGBA_.reset();
    videoW_ = w;
    videoH_ = h;
    videoTexW_ = (texW > 0) ? texW : w;
    videoTexH_ = (texH > 0) ? texH : h;
    videoCropX_ = cropX;
    videoCropY_ = cropY;
    videoIsRGB_ = false;
    videoFullRange_ = fullRange;
    hasVideo_ = (w > 0 && h > 0);
}

void Renderer::SetVideoBGRA(const TexView &rgba, int w, int h) {
    videoRGBA_ = rgba;
    videoY_.reset();
    videoUV_.reset();
    videoW_ = w;
    videoH_ = h;
    videoTexW_ = w;
    videoTexH_ = h;
    videoCropX_ = 0;
    videoCropY_ = 0;
    videoIsRGB_ = true;
    hasVideo_ = (w > 0 && h > 0);
}

void Renderer::ClearVideo() {
    videoY_.reset();
    videoUV_.reset();
    videoRGBA_.reset();
    hasVideo_ = false;
    videoW_ = videoH_ = 0;
}

void Renderer::SetStatusText(const std::string &title, const std::string &subtitle) {
    if (title != titleStr_) {
        titleStr_ = title;
        titleTex_.reset();
        MakeTextTexture(*gpu_, title, 22, true, titleTex_, titleW_, titleH_);
    }
    if (subtitle != subStr_) {
        subStr_ = subtitle;
        subTex_.reset();
        MakeTextTexture(*gpu_, subtitle, 15, false, subTex_, subW_, subH_);
    }
}

void Renderer::FillFrameCB(FrameCB &cb, float vpW, float vpH) const {
    const bool skinned = device_.useSkin && skin_ && skin_->valid;
    const float rad = frame_.angleDeg * 3.14159265358979f / 180.0f;

    cb = FrameCB{};
    cb.viewport[0] = vpW;
    cb.viewport[1] = vpH;
    cb.deviceCenter[0] = frame_.centerX;
    cb.deviceCenter[1] = frame_.centerY;
    cb.bodyHalf[0] = device_.bodyHx;
    cb.bodyHalf[1] = device_.bodyHy;
    cb.scrCenter[0] = device_.scrCx;
    cb.scrCenter[1] = device_.scrCy;
    cb.scrHalf[0] = device_.scrHx;
    cb.scrHalf[1] = device_.scrHy;
    cb.islandCenter[0] = device_.islandCx;
    cb.islandCenter[1] = device_.islandCy;
    cb.islandHalf[0] = device_.islandHx;
    cb.islandHalf[1] = device_.islandHy;
    cb.homeCenter[0] = device_.homeCx;
    cb.homeCenter[1] = device_.homeCy;
    cb.earHalf[0] = device_.earHx;
    cb.earHalf[1] = device_.earHy;
    cb.earCenter[0] = device_.earCx;
    cb.earCenter[1] = device_.earCy;
    cb.skinCenter[0] = device_.skinCx;
    cb.skinCenter[1] = device_.skinCy;
    cb.skinHalf[0] = device_.skinHx;
    cb.skinHalf[1] = device_.skinHy;
    cb.videoCenter[0] = frame_.centerX + frame_.video.cx;
    cb.videoCenter[1] = frame_.centerY + frame_.video.cy;
    cb.videoHalf[0] = frame_.video.hx;
    cb.videoHalf[1] = frame_.video.hy;
    cb.bodyRadius = device_.bodyRadius;
    cb.scrRadius = device_.scrRadius;
    cb.islandRadius = device_.islandRadius;
    cb.islandMode = (float)device_.islandMode;
    cb.homeR = device_.homeR;
    cb.hasVideo = hasVideo_ ? 1.0f : 0.0f;
    cb.shadowAlpha = 0.42f;
    cb.videoIsRGB = videoIsRGB_ ? 1.0f : 0.0f;
    cb.videoFullRange = videoFullRange_ ? 1.0f : 0.0f;
    cb.useSkin = skinned ? 1.0f : 0.0f;
    cb.cosA = std::cos(rad);
    cb.sinA = std::sin(rad);
    cb.videoAlpha = Clampf(frame_.videoAlpha, 0.0f, 1.0f);
    cb.shadowRadius = device_.shadowRadius;
    cb.shadowDrop = device_.shadowDrop;
    cb.invScale = 1.0f / std::max(frame_.scale, 0.05f);

    // The picture is a sub-rectangle of the surface; the rest is padding.
    cb.videoUvOrigin[0] = cb.videoUvOrigin[1] = 0.0f;
    cb.videoUvScale[0] = cb.videoUvScale[1] = 1.0f;
    cb.videoUvClamp[0] = cb.videoUvClamp[1] = 1.0f;
    if (videoTexW_ > 0 && videoTexH_ > 0 && videoW_ > 0 && videoH_ > 0) {
        cb.videoUvOrigin[0] = (float)videoCropX_ / (float)videoTexW_;
        cb.videoUvOrigin[1] = (float)videoCropY_ / (float)videoTexH_;
        cb.videoUvScale[0] = (float)videoW_ / (float)videoTexW_;
        cb.videoUvScale[1] = (float)videoH_ / (float)videoTexH_;
        // Stop a full texel short so bilinear never reaches past the picture.
        cb.videoUvClamp[0] =
            (float)(videoCropX_ + videoW_ - 1) / (float)videoTexW_;
        cb.videoUvClamp[1] =
            (float)(videoCropY_ + videoH_ - 1) / (float)videoTexH_;
    }

    if (videoW_ > 0 && videoH_ > 0 && frame_.video.hx > 0.5f && frame_.video.hy > 0.5f) {
        cb.videoTexel[0] = 1.0f / (float)videoTexW_;
        cb.videoTexel[1] = 1.0f / (float)videoTexH_;
        const float minify = std::max((float)videoW_ / (frame_.video.hx * 2.0f),
                                      (float)videoH_ / (frame_.video.hy * 2.0f));
        cb.superSample = (minify > 1.25f) ? std::min(minify, 4.0f) : 0.0f;
    }

    auto argb = [](uint32_t c, float *out) {
        out[2] = ((c >> 0) & 0xFF) / 255.0f;  // B
        out[1] = ((c >> 8) & 0xFF) / 255.0f;  // G
        out[0] = ((c >> 16) & 0xFF) / 255.0f; // R
        out[3] = ((c >> 24) & 0xFF) / 255.0f;
    };
    argb(profile_.bodyColor, cb.bodyColor);
    argb(profile_.railColor, cb.railColor);
}

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Renderer - Direct3D 11
// ---------------------------------------------------------------------------
bool Renderer::Init(Gpu *gpu) {
    gpu_ = gpu;

    if (!gpu_->CompileVS(kShaderSrc, "VSFull", vsFull_)) return false;
    if (!gpu_->CompileVS(kShaderSrc, "VSQuad", vsQuad_)) return false;
    if (!gpu_->CompilePS(kShaderSrc, "PSFrame", psFrame_)) return false;
    if (!gpu_->CompilePS(kShaderSrc, "PSText", psText_)) return false;

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    bd.ByteWidth = sizeof(FrameCB);
    if (FAILED(gpu_->Device()->CreateBuffer(&bd, nullptr, cbFrame_.put()))) return false;
    bd.ByteWidth = sizeof(QuadCB);
    if (FAILED(gpu_->Device()->CreateBuffer(&bd, nullptr, cbQuad_.put()))) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(gpu_->Device()->CreateSamplerState(&sd, sampler_.put()))) return false;

    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; // colours are premultiplied
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(gpu_->Device()->CreateBlendState(&bl, blendPremul_.put()))) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(gpu_->Device()->CreateRasterizerState(&rd, raster_.put()))) return false;

    return true;
}

void Renderer::Shutdown() {
    videoY_.reset();
    videoUV_.reset();
    videoRGBA_.reset();
    titleTex_.reset();
    subTex_.reset();
    vsFull_.reset();
    vsQuad_.reset();
    psFrame_.reset();
    psText_.reset();
    cbFrame_.reset();
    cbQuad_.reset();
    sampler_.reset();
    blendPremul_.reset();
    raster_.reset();
}

void Renderer::DrawText(const TexView &srv, int w, int h, float cx, float cy,
                        float alpha) {
    if (!srv || alpha <= 0.01f) return;
    auto *ctx = gpu_->Context();

    QuadCB q{};
    q.rect[0] = cx - w * 0.5f;
    q.rect[1] = cy - h * 0.5f;
    q.rect[2] = (float)w;
    q.rect[3] = (float)h;
    q.tint[0] = q.tint[1] = q.tint[2] = q.tint[3] = alpha;

    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(cbQuad_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, &q, sizeof(q));
    ctx->Unmap(cbQuad_.get(), 0);

    ID3D11ShaderResourceView *views[3] = {nullptr, nullptr, srv.get()};
    ctx->PSSetShaderResources(0, 3, views);
    ctx->VSSetShader(vsQuad_.get(), nullptr, 0);
    ctx->PSSetShader(psText_.get(), nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->Draw(4, 0);
}

void Renderer::Render() {
    if (!gpu_ || !gpu_->BackBufferRTV()) return;

    GpuLock lk(*gpu_);
    auto *ctx = gpu_->Context();

    ID3D11RenderTargetView *rtv = gpu_->BackBufferRTV();
    const float clear[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(rtv, clear);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp{};
    vp.Width = (float)gpu_->Width();
    vp.Height = (float)gpu_->Height();
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(raster_.get());

    const float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(blendPremul_.get(), blendFactor, 0xFFFFFFFF);

    const bool skinned = device_.useSkin && skin_ && skin_->valid;
    FrameCB cb;
    FillFrameCB(cb, vp.Width, vp.Height);

    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(cbFrame_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &cb, sizeof(cb));
        ctx->Unmap(cbFrame_.get(), 0);
    }

    ID3D11Buffer *cbs[2] = {cbFrame_.get(), cbQuad_.get()};
    ctx->VSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetConstantBuffers(0, 2, cbs);

    ID3D11SamplerState *samplers[1] = {sampler_.get()};
    ctx->PSSetSamplers(0, 1, samplers);

    // ---- device frame + video --------------------------------------------
    ID3D11ShaderResourceView *views[4] = {videoY_.get(), videoUV_.get(), videoRGBA_.get(),
                                          skinned ? skin_->srv.get() : nullptr};
    ctx->PSSetShaderResources(0, 4, views);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vsFull_.get(), nullptr, 0);
    ctx->PSSetShader(psFrame_.get(), nullptr, 0);
    ctx->Draw(3, 0);

    // ---- idle overlay, upright at the panel centre ------------------------
    if (!hasVideo_) {
        const RectF panel = PanelRectAt(device_, frame_.angleDeg);
        const float cx = frame_.centerX + panel.cx * frame_.scale;
        const float cy = frame_.centerY + panel.cy * frame_.scale;
        if (titleTex_) DrawText(titleTex_, titleW_, titleH_, cx, cy - 16.0f, 0.92f);
        if (subTex_) DrawText(subTex_, subW_, subH_, cx, cy + 16.0f, 0.55f);
    }

    // Leave nothing bound: the decoder may recycle these textures next frame.
    ID3D11ShaderResourceView *nullViews[4] = {nullptr, nullptr, nullptr, nullptr};
    ctx->PSSetShaderResources(0, 4, nullViews);
}
#endif // _WIN32
