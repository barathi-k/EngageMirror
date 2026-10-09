// EngageMirror - audio output.
//
// AirPlay hands us one of four payload types; screen mirroring always uses
// AAC-ELD, which is the reason this app decodes audio with FFmpeg rather than
// Media Foundation (Windows has no AAC-ELD decoder). Output goes to WASAPI in
// shared mode (Windows) or the default-output AudioUnit (macOS), both with
// automatic rate conversion, so we can always render at the stream's native
// 44.1 kHz.
#pragma once

#include "common.h"

#include <atomic>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#ifdef _WIN32
struct IAudioClient;
struct IAudioRenderClient;
struct IMMDevice;
struct IMMDeviceEnumerator;
#else
#include <AudioToolbox/AudioToolbox.h>
#endif

class AudioPlayer {
  public:
    bool Init();
    void Shutdown();

    // AirPlay compression type: 1=LPCM, 2=ALAC, 4=AAC-LC, 8=AAC-ELD.
    void SetFormat(unsigned char ct);
    // Runs the output device only while a client is streaming audio: an idle
    // receiver should not keep the audio hardware (and the machine) awake.
    void SetActive(bool active);
    void Submit(const uint8_t *data, int len);
    void Flush();

    // AirPlay reports volume in dB (0 = loudest, -144 = muted).
    void SetVolumeDb(float db);
    void SetMuted(bool muted) { muted_.store(muted); }
    bool Muted() const { return muted_.load(); }

  private:
    bool OpenDevice();
    void CloseDevice();
    bool OpenCodec(unsigned char ct);
    void CloseCodec();
    void PushSamples(const float *interleaved, int frames);
    int PopSamples(float *dst, int frames);

#ifdef _WIN32
    static unsigned __stdcall ThreadProc(void *self);
    void RenderLoop();

    // WASAPI
    IMMDeviceEnumerator *enumerator_ = nullptr;
    IMMDevice *device_ = nullptr;
    IAudioClient *client_ = nullptr;
    IAudioRenderClient *render_ = nullptr;
    HANDLE event_ = nullptr;
    HANDLE thread_ = nullptr;
    uint32_t bufferFrames_ = 0;
    std::atomic<bool> running_{false};
#else
    // Pulled by Core Audio's render thread.
    static OSStatus RenderCb(void *self, AudioUnitRenderActionFlags *flags,
                             const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
                             AudioBufferList *data);
    AudioComponentInstance unit_ = nullptr;
#endif

    // Decoder
    AVCodecContext *codec_ = nullptr;
    AVPacket *packet_ = nullptr;
    AVFrame *frame_ = nullptr;
    SwrContext *swr_ = nullptr;
    unsigned char ct_ = 0;
    std::vector<float> convBuf_;

    // Ring buffer of interleaved stereo float at 44.1 kHz
    static constexpr int kRate = 44100;
    static constexpr int kChannels = 2;
    std::mutex ringMutex_;
    std::vector<float> ring_;
    size_t readPos_ = 0;
    size_t writePos_ = 0;
    size_t filled_ = 0;

    std::atomic<float> gain_{1.0f};
    std::atomic<bool> muted_{false};
};
