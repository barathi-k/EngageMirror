#include "video_decoder.h"

#include <algorithm>
#include <utility>

extern "C" {
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
}

namespace {

// FFmpeg is chatty when a stream hiccups; fold repeats so one bad packet
// cannot bury the log.
void FFmpegLogBridge(void *avcl, int level, const char *fmt, va_list vl) {
    if (level > AV_LOG_WARNING) return;

    char line[512];
    int prefix = 1;
    if (av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &prefix) < 0) return;

    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    if (n == 0) return;

    static char last[512] = {};
    static unsigned repeats = 0;
    if (strcmp(last, line) == 0) {
        if (++repeats % 250 != 0) return;
        LOGW("ffmpeg: %s (x%u)", line, repeats);
        return;
    }
    snprintf(last, sizeof(last), "%s", line);
    repeats = 0;
    if (level <= AV_LOG_ERROR) {
        LOGW("ffmpeg: %s", line);
    } else {
        LOGD("ffmpeg: %s", line);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// VideoFrameRef
// ---------------------------------------------------------------------------
VideoFrameRef &VideoFrameRef::operator=(VideoFrameRef &&o) noexcept {
    if (this != &o) {
        Reset();
        frame = o.frame;
        y = std::move(o.y);
        uv = std::move(o.uv);
        rgba = std::move(o.rgba);
        width = o.width;
        height = o.height;
        texW = o.texW;
        texH = o.texH;
        cropX = o.cropX;
        cropY = o.cropY;
        isRGB = o.isRGB;
        fullRange = o.fullRange;
        seq = o.seq;
        o.frame = nullptr;
        o.width = o.height = 0;
        o.texW = o.texH = 0;
        o.cropX = o.cropY = 0;
        o.seq = 0;
    }
    return *this;
}

void VideoFrameRef::Reset() {
    if (frame) av_frame_free(&frame);
    y.reset();
    uv.reset();
    rgba.reset();
    width = height = 0;
    texW = texH = 0;
    cropX = cropY = 0;
    isRGB = false;
    fullRange = false;
    seq = 0;
}

// The move assignment and Reset() above list every member by hand, because the
// AVFrame reference cannot be copied. Adding a field and forgetting them is
// silent: it arrives at the renderer as zero. This trips if the layout changes
// so that both get revisited - check them, then update the number.
static_assert(sizeof(VideoFrameRef) == 72,
              "VideoFrameRef gained or lost a member: update operator=() and Reset()");

// ---------------------------------------------------------------------------
// VideoDecoder
// ---------------------------------------------------------------------------
bool VideoDecoder::Init(Gpu *gpu) {
    gpu_ = gpu;
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(FFmpegLogBridge);

    packet_ = av_packet_alloc();
    scratch_ = av_frame_alloc();
    if (!packet_ || !scratch_) return false;

    hwDevice_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hwDevice_) {
        LOGW("D3D11VA hwdevice allocation failed; software decoding only");
        return true;
    }
    auto *dctx = (AVHWDeviceContext *)hwDevice_->data;
    auto *d3d = (AVD3D11VADeviceContext *)dctx->hwctx;
    d3d->device = gpu_->Device();
    d3d->device->AddRef(); // FFmpeg releases this on teardown

    int err = av_hwdevice_ctx_init(hwDevice_);
    if (err < 0) {
        char buf[128];
        av_strerror(err, buf, sizeof(buf));
        LOGW("av_hwdevice_ctx_init failed (%s); software decoding only", buf);
        av_buffer_unref(&hwDevice_);
    }
    return true;
}

void VideoDecoder::Shutdown() {
    {
        std::lock_guard<std::mutex> lk(codecMutex_);
        CloseCodec();
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        newest_.Reset();
    }
    srvCache_.clear();
    bgraSrv_.reset();
    bgraTex_.reset();
    if (sws_) {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    if (scratch_) av_frame_free(&scratch_);
    if (packet_) av_packet_free(&packet_);
    if (hwDevice_) av_buffer_unref(&hwDevice_);
    gpu_ = nullptr;
}

void VideoDecoder::CloseCodec() {
    if (codec_) {
        avcodec_free_context(&codec_);
    }
    srvCache_.clear();
}

AVPixelFormat VideoDecoder::GetFormatCb(AVCodecContext *c, const AVPixelFormat *fmts) {
    auto *self = (VideoDecoder *)c->opaque;

    if (self && self->hwDevice_ && !self->hwSetupFailed_) {
        for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == AV_PIX_FMT_D3D11) {
                if (self->SetupHwFrames(c)) {
                    self->usingHardware_ = true;
                    LOGI("video: D3D11VA hardware decoding (%dx%d)", c->width, c->height);
                    return AV_PIX_FMT_D3D11;
                }
                self->hwSetupFailed_ = true;
                LOGW("video: D3D11VA unavailable, falling back to software decode");
                break;
            }
        }
    }

    for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            if (self) self->usingHardware_ = false;
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

bool VideoDecoder::SetupHwFrames(AVCodecContext *c) {
    av_buffer_unref(&c->hw_frames_ctx);

    int err = avcodec_get_hw_frames_parameters(c, c->hw_device_ctx, AV_PIX_FMT_D3D11,
                                               &c->hw_frames_ctx);
    if (err < 0) return false;

    auto *fctx = (AVHWFramesContext *)c->hw_frames_ctx->data;
    auto *hw = (AVD3D11VAFramesContext *)fctx->hwctx;

    // The decode surfaces must double as shader resources for the zero-copy
    // path, and we keep one frame checked out for display.
    hw->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    fctx->initial_pool_size += 4;

    err = av_hwframe_ctx_init(c->hw_frames_ctx);
    if (err < 0) {
        av_buffer_unref(&c->hw_frames_ctx);
        return false;
    }
    if (fctx->width != c->width || fctx->height != c->height) {
        // Padded up to a multiple of 16. The renderer scales its uv so the
        // padding is never sampled - it decodes as bright green.
        LOGI("video: decode surface %dx%d for a %dx%d picture", fctx->width, fctx->height,
             c->width, c->height);
    }
    srvCache_.clear();
    return true;
}

bool VideoDecoder::SetCodec(bool h265) {
    std::lock_guard<std::mutex> lk(codecMutex_);
    if (codec_ && isH265_ == h265) return true;

    CloseCodec();
    isH265_ = h265;
    usingHardware_ = false;

    const AVCodec *codec = avcodec_find_decoder(h265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    if (!codec) {
        LOGE("no %s decoder available in this FFmpeg build", h265 ? "HEVC" : "H.264");
        return false;
    }

    codec_ = avcodec_alloc_context3(codec);
    if (!codec_) return false;

    codec_->opaque = this;
    codec_->get_format = GetFormatCb;
    codec_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // NB: do not set AV_CODEC_FLAG2_CHUNKS. libairplay delivers whole,
    // AU-aligned access units; telling the decoder that packets may be
    // truncated makes it report "Broken frame packetizing" and reject every
    // slice after the first.
    codec_->flags2 |= AV_CODEC_FLAG2_FAST;
    // Slice threading adds no output delay, unlike frame threading, which
    // matters a lot for a mirroring display.
    codec_->thread_type = FF_THREAD_SLICE;
    codec_->thread_count = 4;
    if (hwDevice_) codec_->hw_device_ctx = av_buffer_ref(hwDevice_);

    int err = avcodec_open2(codec_, codec, nullptr);
    if (err < 0) {
        char buf[128];
        av_strerror(err, buf, sizeof(buf));
        LOGE("avcodec_open2 failed: %s", buf);
        avcodec_free_context(&codec_);
        return false;
    }
    LOGI("video: opened %s decoder", h265 ? "HEVC" : "H.264");
    return true;
}

void VideoDecoder::DrainFrames() {
    for (;;) {
        int err = avcodec_receive_frame(codec_, scratch_);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) return;
        if (err < 0) {
            char buf[128];
            av_strerror(err, buf, sizeof(buf));
            LOGD("receive_frame: %s", buf);
            return;
        }
        OnDecodedFrame(scratch_);
        av_frame_unref(scratch_);
    }
}

void VideoDecoder::Decode(const uint8_t *data, int len) {
    if (!data || len <= 0) return;
    std::lock_guard<std::mutex> lk(codecMutex_);
    if (!codec_) return;

    packet_->data = (uint8_t *)data;
    packet_->size = len;
    packet_->pts = AV_NOPTS_VALUE;
    packet_->dts = AV_NOPTS_VALUE;

    // EAGAIN means the decoder still has output queued. Dropping the packet
    // here would punch a hole in the reference chain and every later frame
    // would fail, so drain and retry instead.
    for (int attempt = 0; attempt < 8; attempt++) {
        int err = avcodec_send_packet(codec_, packet_);
        if (err == AVERROR(EAGAIN)) {
            DrainFrames();
            continue;
        }
        if (err < 0) {
            char buf[128];
            av_strerror(err, buf, sizeof(buf));
            LOGD("send_packet: %s", buf);
        }
        break;
    }

    DrainFrames();
}

void VideoDecoder::OnDecodedFrame(AVFrame *f) {
    VideoFrameRef ref;
    bool ok = false;

    if (f->format == AV_PIX_FMT_D3D11) {
        ok = MakeNV12Views(f, ref);
    } else {
        ok = MakeBGRAView(f, ref);
    }
    if (!ok) return;

    ref.seq = ++seqCounter_;
    std::lock_guard<std::mutex> lk(mutex_);
    newest_ = std::move(ref);
}

bool VideoDecoder::MakeNV12Views(AVFrame *f, VideoFrameRef &out) {
    auto *tex = (ID3D11Texture2D *)f->data[0];
    const int index = (int)(intptr_t)f->data[1];
    if (!tex) return false;

    auto key = std::make_pair(tex, index);
    auto it = srvCache_.find(key);
    if (it == srvCache_.end()) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray.MostDetailedMip = 0;
        d.Texture2DArray.MipLevels = 1;
        d.Texture2DArray.FirstArraySlice = (UINT)index;
        d.Texture2DArray.ArraySize = 1;

        SrvPair pair;
        d.Format = DXGI_FORMAT_R8_UNORM;
        if (FAILED(gpu_->Device()->CreateShaderResourceView(tex, &d, pair.y.put()))) {
            LOGW("luma SRV creation failed on decoder surface");
            return false;
        }
        d.Format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(gpu_->Device()->CreateShaderResourceView(tex, &d, pair.uv.put()))) {
            LOGW("chroma SRV creation failed on decoder surface");
            return false;
        }
        it = srvCache_.emplace(key, std::move(pair)).first;
    }

    out.frame = av_frame_alloc();
    if (!out.frame) return false;
    if (av_frame_ref(out.frame, f) < 0) {
        av_frame_free(&out.frame);
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);

    // What the renderer's uv scaling actually depends on, straight from the
    // frame rather than from the codec context, logged whenever it changes.
    if ((int)td.Width != frameLogW_ || f->width != frameLogPicW_) {
        frameLogW_ = (int)td.Width;
        frameLogPicW_ = f->width;
        LOGI("video: frame %dx%d in a %ux%u surface, crop l%zu r%zu t%zu b%zu, %d slices",
             f->width, f->height, td.Width, td.Height, f->crop_left, f->crop_right,
             f->crop_top, f->crop_bottom, (int)td.ArraySize);
    }

    out.y = it->second.y;
    out.uv = it->second.uv;
    out.width = f->width;
    out.height = f->height;
    out.texW = (int)td.Width;
    out.texH = (int)td.Height;
    out.cropX = (int)f->crop_left;
    out.cropY = (int)f->crop_top;
    out.isRGB = false;
    out.fullRange = (f->color_range == AVCOL_RANGE_JPEG);
    return true;
}

bool VideoDecoder::MakeBGRAView(AVFrame *f, VideoFrameRef &out) {
    const int w = f->width, h = f->height;
    if (w <= 0 || h <= 0) return false;

    sws_ = sws_getCachedContext(sws_, w, h, (AVPixelFormat)f->format, w, h,
                                AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) return false;

    bgra_.resize((size_t)w * h * 4);
    uint8_t *dst[4] = {bgra_.data(), nullptr, nullptr, nullptr};
    int stride[4] = {w * 4, 0, 0, 0};
    sws_scale(sws_, f->data, f->linesize, 0, h, dst, stride);

    if (!bgraTex_ || bgraW_ != w || bgraH_ != h) {
        bgraSrv_.reset();
        bgraTex_.reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)w;
        td.Height = (UINT)h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(gpu_->Device()->CreateTexture2D(&td, nullptr, bgraTex_.put()))) return false;
        if (FAILED(gpu_->Device()->CreateShaderResourceView(bgraTex_.get(), nullptr,
                                                            bgraSrv_.put())))
            return false;
        bgraW_ = w;
        bgraH_ = h;
    }

    {
        GpuLock lk(*gpu_);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(gpu_->Context()->Map(bgraTex_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            return false;
        for (int row = 0; row < h; row++) {
            memcpy((uint8_t *)m.pData + (size_t)row * m.RowPitch,
                   bgra_.data() + (size_t)row * w * 4, (size_t)w * 4);
        }
        gpu_->Context()->Unmap(bgraTex_.get(), 0);
    }

    out.rgba = bgraSrv_;
    out.width = w;
    out.height = h;
    out.texW = w; // software path allocates exactly the picture size
    out.texH = h;
    out.isRGB = true;
    out.fullRange = true;
    return true;
}

bool VideoDecoder::PullNewest(VideoFrameRef &out) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!newest_.Valid() || newest_.seq == out.seq) return false;
    out = std::move(newest_);
    return true;
}

void VideoDecoder::Flush() {
    {
        std::lock_guard<std::mutex> lk(codecMutex_);
        if (codec_) avcodec_flush_buffers(codec_);
    }
    std::lock_guard<std::mutex> lk(mutex_);
    newest_.Reset();
}
