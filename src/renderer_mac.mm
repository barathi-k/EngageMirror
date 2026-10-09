// EngageMirror - Metal renderer (macOS). A line-for-line port of the HLSL in
// renderer.cpp; keep the two in step. Layout maths and FrameCB filling are
// shared and live in renderer.cpp.
#include "renderer.h"

#include <algorithm>
#include <cmath>

// Compiled at runtime (like the D3D build does with D3DCompile), which keeps
// the build free of the separately-downloaded Metal toolchain.
//
// FrameCB is declared with packed vector types so the struct is laid out
// exactly like the C++ one: plain float2 would be 8-byte aligned and shift
// everything after videoTexel.
static NSString *const kShaderSrc = @R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Frame {
    packed_float2 viewport;
    packed_float2 deviceCenter;
    packed_float2 bodyHalf;
    packed_float2 scrCenter;
    packed_float2 scrHalf;
    packed_float2 islandCenter;
    packed_float2 islandHalf;
    packed_float2 homeCenter;
    packed_float2 earHalf;
    packed_float2 earCenter;
    packed_float2 skinCenter;
    packed_float2 skinHalf;
    packed_float2 videoCenter;
    packed_float2 videoHalf;
    float bodyRadius;
    float scrRadius;
    float islandRadius;
    float islandMode;
    float homeR;
    float hasVideo;
    float shadowAlpha;
    float videoIsRGB;
    float videoFullRange;
    packed_float2 videoTexel;
    float superSample;
    float useSkin;
    float cosA;
    float sinA;
    float videoAlpha;
    float shadowRadius;
    float shadowDrop;
    float invScale;
    float pad0;
    packed_float2 videoUvOrigin;
    packed_float2 videoUvScale;
    packed_float2 videoUvClamp;
    packed_float2 pad1;
    packed_float4 bodyColor;
    packed_float4 railColor;
};

struct Quad {
    float4 quadRect;   // x, y, w, h in pixels
    float4 quadTint;
};

struct VSOut { float4 pos [[position]]; float2 uv; };

struct Tex {
    texture2d<float> y, uv, rgba, skin;
    sampler samp;
};

vertex VSOut VSFull(uint id [[vertex_id]])
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

vertex VSOut VSQuad(uint id [[vertex_id]], constant Frame &F [[buffer(0)]],
                    constant Quad &Q [[buffer(1)]])
{
    VSOut o;
    float2 c  = float2((id == 1 || id == 3) ? 1.0 : 0.0, (id >= 2) ? 1.0 : 0.0);
    float2 px = Q.quadRect.xy + c * Q.quadRect.zw;
    o.uv  = c;
    o.pos = float4(px / float2(F.viewport) * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Window space -> device space (undo the chassis rotation).
float2 ToDevice(constant Frame &F, float2 v)
{
    return float2(v.x * F.cosA + v.y * F.sinA, -v.x * F.sinA + v.y * F.cosA);
}

// Cheap stable hash, used to dither the shadow's tail.
float Hash12(float2 p)
{
    float3 q = fract(float3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

float sdRoundRect(float2 p, float2 b, float r)
{
    r = min(r, min(b.x, b.y));
    float2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float3 SampleVideoOnce(constant Frame &F, thread const Tex &T, float2 uv)
{
    // Never outside the picture: everything else in the surface is padding.
    uv = clamp(uv, float2(F.videoUvOrigin), float2(F.videoUvClamp));

    if (F.videoIsRGB > 0.5)
        return T.rgba.sample(T.samp, uv).rgb;

    float  y   = T.y.sample(T.samp, uv).r;
    float2 uvc = T.uv.sample(T.samp, uv).rg;
    float  u   = uvc.x - 0.5;
    float  v   = uvc.y - 0.5;

    if (F.videoFullRange < 0.5)
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

float3 SampleVideo(constant Frame &F, thread const Tex &T, float2 uv)
{
    if (F.superSample < 0.5)
        return SampleVideoOnce(F, T, uv);

    // Rotated-grid 2x2 supersample: the panel is usually minified a lot
    // (a 2048-wide stream shown at ~800px), and plain bilinear shimmers.
    float2 d = float2(F.videoTexel) * F.superSample * 0.25;
    float3 s  = SampleVideoOnce(F, T, uv + float2(-d.x,  d.y));
    s        += SampleVideoOnce(F, T, uv + float2( d.x,  d.y));
    s        += SampleVideoOnce(F, T, uv + float2(-d.x, -d.y));
    s        += SampleVideoOnce(F, T, uv + float2( d.x, -d.y));
    return s * 0.25;
}

fragment float4 PSFrame(VSOut i [[stage_in]], constant Frame &F [[buffer(0)]],
                        texture2d<float> texY [[texture(0)]],
                        texture2d<float> texUV [[texture(1)]],
                        texture2d<float> texRGBA [[texture(2)]],
                        texture2d<float> texSkin [[texture(3)]],
                        sampler samp [[sampler(0)]])
{
    Tex T = { texY, texUV, texRGBA, texSkin, samp };
    float2 p = i.uv * float2(F.viewport);
    float2 w = p - float2(F.deviceCenter);  // window space, centred on the device
    float2 d = ToDevice(F, w) * F.invScale; // device space (always portrait, unit scale)

    float2 bodyHalf  = F.bodyHalf;
    float2 scrCenter = F.scrCenter;

    float3 col = float3(0.0);
    float  a   = 0.0;

    // Soft contact shadow; see the HLSL for the reasoning.
    if (F.shadowAlpha > 0.001 && F.shadowRadius > 0.5)
    {
        float2 dsh = d - ToDevice(F, float2(0.0, F.shadowDrop));
        float  ds  = sdRoundRect(dsh, bodyHalf + 2.0, F.bodyRadius + 2.0);
        float  x   = saturate(1.0 - ds / F.shadowRadius);
        a = F.shadowAlpha * x * x;
        if (a > 0.0) a = max(0.0, a + (Hash12(p) - 0.5) * (1.0 / 255.0));
    }

    // Panel, in device space so it turns with the chassis.
    float dsc  = sdRoundRect(d - scrCenter, float2(F.scrHalf), F.scrRadius);
    float panA = saturate(0.5 - dsc);

    // Video: upright in window space, clipped to the panel.
    float3 vidC = float3(0.0);
    float  vidA = 0.0;
    float2 vc = F.videoCenter, vh = F.videoHalf;
    if (F.hasVideo > 0.5 && F.videoAlpha > 0.002 && vh.x > 0.5 && vh.y > 0.5)
    {
        float dv = sdRoundRect(p - vc, vh, 0.0);
        vidA = saturate(0.5 - dv) * panA * F.videoAlpha;
        float2 uv = saturate((p - (vc - vh)) / (2.0 * vh));
        vidC = SampleVideo(F, T, float2(F.videoUvOrigin) + uv * float2(F.videoUvScale));
    }

    // ---- PNG skin -------------------------------------------------------
    if (F.useSkin > 0.5)
    {
        col = col * (1.0 - panA) + float3(0.012, 0.012, 0.015) * panA;
        a   = a   * (1.0 - panA) + panA;

        col = col * (1.0 - vidA) + vidC * vidA;
        a   = a   * (1.0 - vidA) + vidA;

        float2 sc = F.skinCenter, sh = F.skinHalf;
        float2 su = (d - (sc - sh)) / (2.0 * sh);
        float4 sk = float4(0.0);
        if (su.x >= 0.0 && su.x <= 1.0 && su.y >= 0.0 && su.y <= 1.0)
            sk = texSkin.sample(samp, su);   // premultiplied

        col = sk.rgb + col * (1.0 - sk.a);
        a   = sk.a   + a   * (1.0 - sk.a);
        return float4(col, a);
    }

    // ---- procedural chassis ---------------------------------------------
    float4 bodyColor = F.bodyColor, railColor = F.railColor;
    float db    = sdRoundRect(d, bodyHalf, F.bodyRadius);
    float bodyA = saturate(0.5 - db);

    float3 bodyC = bodyColor.rgb;
    float  rail  = saturate(1.0 - abs(db + 1.3) / 1.5);
    float2 n     = normalize(d + float2(1e-5, 1e-5));
    float  sheen = 0.55 + 0.45 * abs(n.x * 0.85 + n.y * 0.35);
    bodyC = mix(bodyC, railColor.rgb * sheen, rail);

    col = col * (1.0 - bodyA) + bodyC * bodyA;
    a   = a   * (1.0 - bodyA) + bodyA;

    col = col * (1.0 - panA) + float3(0.016, 0.016, 0.020) * panA;
    a   = a   * (1.0 - panA) + panA;

    col = col * (1.0 - vidA) + vidC * vidA;
    a   = a   * (1.0 - vidA) + vidA;

    // Earpiece slot (home-button hardware).
    float2 earHalf = F.earHalf;
    if (earHalf.x > 0.5)
    {
        float de = sdRoundRect(d - float2(F.earCenter), earHalf, earHalf.y);
        float ea = saturate(0.5 - de);
        col = col * (1.0 - ea) + float3(0.03, 0.03, 0.035) * ea;
        a   = max(a, ea);
    }

    // Home button.
    if (F.homeR > 0.5)
    {
        float dh   = length(d - float2(F.homeCenter)) - F.homeR;
        float fill = saturate(0.5 - dh);
        float ring = saturate(1.4 - abs(dh));
        col = col * (1.0 - fill) + bodyColor.rgb * 0.82 * fill;
        a   = max(a, fill);
        col = col * (1.0 - ring) + railColor.rgb * 0.85 * ring;
        a   = max(a, ring);
    }

    // Notch or Dynamic Island, painted over the video.
    if (F.islandMode > 0.5)
    {
        float2 ic0 = F.islandCenter, ih = F.islandHalf;
        float di = sdRoundRect(d - ic0, ih, F.islandRadius);
        if (F.islandMode < 1.5)
            di = max(di, dsc);        // a notch is clipped to the panel edge

        float  ia = saturate(0.5 - di);
        float3 ic = float3(0.012, 0.012, 0.016);

        float2 lens  = d - (ic0 + float2(ih.x * 0.60, 0.0));
        float  lensD = length(lens) - min(ih.y, ih.x) * 0.40;
        ic = mix(ic, float3(0.09, 0.10, 0.13), saturate(0.5 - lensD) * 0.75);

        col = col * (1.0 - ia) + ic * ia;
        a   = max(a, ia);
    }

    return float4(col, a);
}

fragment float4 PSText(VSOut i [[stage_in]], constant Quad &Q [[buffer(1)]],
                       texture2d<float> texRGBA [[texture(2)]], sampler samp [[sampler(0)]])
{
    return texRGBA.sample(samp, i.uv) * Q.quadTint;
}
)MSL";

static_assert(sizeof(float) == 4, "FrameCB layout assumes 4-byte floats");

namespace {
id<MTLRenderPipelineState> MakePipeline(id<MTLDevice> dev, id<MTLLibrary> lib,
                                        NSString *vs, NSString *fs) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:vs];
    d.fragmentFunction = [lib newFunctionWithName:fs];
    MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
    c.pixelFormat = MTLPixelFormatBGRA8Unorm;
    c.blendingEnabled = YES;
    // Colours are premultiplied.
    c.sourceRGBBlendFactor = MTLBlendFactorOne;
    c.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    c.sourceAlphaBlendFactor = MTLBlendFactorOne;
    c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    NSError *err = nil;
    id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) LOGE("pipeline %s/%s: %s", vs.UTF8String, fs.UTF8String,
                 err.localizedDescription.UTF8String);
    return p;
}
} // namespace

bool Renderer::Init(Gpu *gpu) {
    gpu_ = gpu;
    id<MTLDevice> dev = gpu_->Device();

    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:kShaderSrc options:nil error:&err];
    if (!lib) {
        LOGE("shader compile failed: %s", err.localizedDescription.UTF8String);
        return false;
    }
    pipeFrame_ = MakePipeline(dev, lib, @"VSFull", @"PSFrame");
    pipeText_ = MakePipeline(dev, lib, @"VSQuad", @"PSText");
    if (!pipeFrame_ || !pipeText_) return false;

    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.mipFilter = MTLSamplerMipFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_ = [dev newSamplerStateWithDescriptor:sd];

    // Metal has no "null texture" binding; unused slots get this instead.
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:1
                                                          height:1
                                                       mipmapped:NO];
    dummy_ = [dev newTextureWithDescriptor:td];
    const uint32_t zero = 0;
    [dummy_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&zero
              bytesPerRow:4];
    return true;
}

void Renderer::Shutdown() {
    videoY_.reset();
    videoUV_.reset();
    videoRGBA_.reset();
    titleTex_.reset();
    subTex_.reset();
    pipeFrame_ = nil;
    pipeText_ = nil;
    sampler_ = nil;
    dummy_ = nil;
}

void Renderer::DrawText(const TexView &srv, int w, int h, float cx, float cy, float alpha) {
    if (!srv || !enc_ || alpha <= 0.01f) return;

    QuadCB q{};
    q.rect[0] = std::round(cx - w * 0.5f); // whole pixels keep the glyphs crisp
    q.rect[1] = std::round(cy - h * 0.5f);
    q.rect[2] = (float)w;
    q.rect[3] = (float)h;
    q.tint[0] = q.tint[1] = q.tint[2] = q.tint[3] = alpha;

    [enc_ setRenderPipelineState:pipeText_];
    [enc_ setVertexBytes:&lastCB_ length:sizeof(lastCB_) atIndex:0];
    [enc_ setVertexBytes:&q length:sizeof(q) atIndex:1];
    [enc_ setFragmentBytes:&q length:sizeof(q) atIndex:1];
    [enc_ setFragmentTexture:srv.tex atIndex:2];
    [enc_ drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

void Renderer::Render() {
    if (!gpu_ || !pipeFrame_) return;

    @autoreleasepool {
        id<CAMetalDrawable> drawable = [gpu_->Layer() nextDrawable];
        if (!drawable) return;
        const float vpW = (float)drawable.texture.width;
        const float vpH = (float)drawable.texture.height;

        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLCommandBuffer> cmd = [gpu_->Queue() commandBuffer];
        enc_ = [cmd renderCommandEncoderWithDescriptor:pass];

        FillFrameCB(lastCB_, vpW, vpH);
        const bool skinned = device_.useSkin && skin_ && skin_->valid;

        [enc_ setRenderPipelineState:pipeFrame_];
        [enc_ setFragmentBytes:&lastCB_ length:sizeof(lastCB_) atIndex:0];
        [enc_ setFragmentTexture:(videoY_ ? videoY_.tex : dummy_) atIndex:0];
        [enc_ setFragmentTexture:(videoUV_ ? videoUV_.tex : dummy_) atIndex:1];
        [enc_ setFragmentTexture:(videoRGBA_ ? videoRGBA_.tex : dummy_) atIndex:2];
        [enc_ setFragmentTexture:(skinned ? skin_->srv.tex : dummy_) atIndex:3];
        [enc_ setFragmentSamplerState:sampler_ atIndex:0];
        [enc_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

        // ---- idle overlay, upright at the panel centre ------------------------
        if (!hasVideo_) {
            const RectF panel = PanelRectAt(device_, frame_.angleDeg);
            const float cx = frame_.centerX + panel.cx * frame_.scale;
            const float cy = frame_.centerY + panel.cy * frame_.scale;
            const float gap = 16.0f * gpu_->Scale();
            if (titleTex_) DrawText(titleTex_, titleW_, titleH_, cx, cy - gap, 0.92f);
            if (subTex_) DrawText(subTex_, subW_, subH_, cx, cy + gap, 0.55f);
        }

        [enc_ endEncoding];
        enc_ = nil;

        // The decoder recycles its surfaces once their last reference goes;
        // hold this frame's until the GPU is done sampling them.
        id keepY = videoY_.keep, keepUV = videoUV_.keep, keepRGBA = videoRGBA_.tex;
        [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
          (void)keepY;
          (void)keepUV;
          (void)keepRGBA;
        }];
        [cmd presentDrawable:drawable];
        [cmd commit];
    }
}
