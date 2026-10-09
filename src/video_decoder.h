// AirMirror - H.264/H.265 decoding.
//
// Preferred path is FFmpeg + D3D11VA: the decoder writes NV12 straight into a
// D3D11 texture array that we sample directly in the frame shader, so a
// mirrored frame never touches system memory. If the driver will not give us
// decoder textures that are also shader resources we fall back to software
// decode + BGRA upload.
#pragma once

#include "common.h"
#include "gpu.h"

#include <map>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswscale/swscale.h>
}

// A decoded frame plus the views needed to sample it. Owns an AVFrame
// reference so the decoder's surface pool cannot recycle it while on screen.
class VideoFrameRef {
  public:
    VideoFrameRef() = default;
    ~VideoFrameRef() { Reset(); }
    VideoFrameRef(const VideoFrameRef &) = delete;
    VideoFrameRef &operator=(const VideoFrameRef &) = delete;
    VideoFrameRef(VideoFrameRef &&o) noexcept { *this = std::move(o); }
    VideoFrameRef &operator=(VideoFrameRef &&o) noexcept;

    void Reset();
    bool Valid() const { return width > 0 && height > 0; }

    AVFrame *frame = nullptr;
    Com<ID3D11ShaderResourceView> y, uv, rgba;
    int width = 0, height = 0;
    // Where the picture actually sits inside the surface. D3D11VA rounds decode
    // surfaces up to a multiple of 16, so an 810x1080 stream lives in an
    // 816x1088 texture; sampling uv 0..1 drags in the padding, which decodes as
    // bright green. FFmpeg cannot apply left/top cropping to a hardware surface
    // either, so the picture does not necessarily start at (0,0).
    int texW = 0, texH = 0;
    int cropX = 0, cropY = 0;
    bool isRGB = false;
    bool fullRange = false;
    uint64_t seq = 0;
};

class VideoDecoder {
  public:
    bool Init(Gpu *gpu);
    void Shutdown();

    // Called when the client announces (or changes) the codec.
    bool SetCodec(bool h265);

    // Feed one Annex-B access unit, as delivered by libairplay.
    void Decode(const uint8_t *data, int len);

    void Flush();

    // Render thread: swap in the newest frame if there is one. Returns true
    // when `out` was replaced.
    bool PullNewest(VideoFrameRef &out);

    bool UsingHardware() const { return usingHardware_; }

  private:
    static AVPixelFormat GetFormatCb(AVCodecContext *c, const AVPixelFormat *fmts);
    bool SetupHwFrames(AVCodecContext *c);
    void CloseCodec();
    void DrainFrames();
    void OnDecodedFrame(AVFrame *f);
    bool MakeNV12Views(AVFrame *f, VideoFrameRef &out);
    bool MakeBGRAView(AVFrame *f, VideoFrameRef &out);

    Gpu *gpu_ = nullptr;
    AVBufferRef *hwDevice_ = nullptr;
    AVCodecContext *codec_ = nullptr;
    AVPacket *packet_ = nullptr;
    AVFrame *scratch_ = nullptr;
    SwsContext *sws_ = nullptr;
    std::vector<uint8_t> bgra_;
    Com<ID3D11Texture2D> bgraTex_;
    Com<ID3D11ShaderResourceView> bgraSrv_;
    int bgraW_ = 0, bgraH_ = 0;

    struct SrvPair {
        Com<ID3D11ShaderResourceView> y, uv;
    };
    std::map<std::pair<ID3D11Texture2D *, int>, SrvPair> srvCache_;

    // Guards the AVCodecContext. Decode() runs on the mirror RTP thread while
    // Flush()/SetCodec() can arrive from the RTSP thread.
    std::mutex codecMutex_;
    // Guards the handoff of the newest frame to the render thread.
    std::mutex mutex_;
    VideoFrameRef newest_;
    uint64_t seqCounter_ = 0;

    int frameLogW_ = 0, frameLogPicW_ = 0; // rate-limits the frame geometry log

    bool isH265_ = false;
    bool usingHardware_ = false;
    bool hwSetupFailed_ = false;
};
