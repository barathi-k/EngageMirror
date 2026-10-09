#include "audio_player.h"

#ifdef _WIN32
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <process.h>
#endif

#include <algorithm>
#include <cmath>

#ifdef _WIN32
// Declared locally so the build does not depend on which GUIDs a given MinGW
// libuuid happens to export.
static const CLSID kCLSID_MMDeviceEnumerator = {
    0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIID_IMMDeviceEnumerator = {
    0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID kIID_IAudioClient = {
    0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID kIID_IAudioRenderClient = {
    0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT is declared but not exported by MinGW's libs.
static const GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
#endif

// AirPlay codec configuration (matches what the client negotiates in RTSP).
// ALAC magic cookie: 44100/16/2, 352 samples per frame.
static const uint8_t kAlacCookie[36] = {
    0x00, 0x00, 0x00, 0x24, 0x61, 0x6c, 0x61, 0x63, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x60, 0x00, 0x10, 0x28, 0x0a, 0x0e, 0x02, 0x00, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xac, 0x44};
// AudioSpecificConfig for AAC-LC 44100/2 and ER AAC-ELD 44100/2.
static const uint8_t kAacLcAsc[2] = {0x12, 0x10};
static const uint8_t kAacEldAsc[4] = {0xf8, 0xe8, 0x50, 0x00};

bool AudioPlayer::Init() {
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!packet_ || !frame_) return false;

    ring_.assign((size_t)kRate * kChannels, 0.0f); // 1 second

    if (!OpenDevice()) {
        LOGW("audio: no output device, continuing muted");
        return true; // video mirroring should still work
    }

#ifdef _WIN32
    running_.store(true);
    thread_ = (HANDLE)_beginthreadex(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!thread_) {
        running_.store(false);
        return true;
    }
    SetThreadPriority(thread_, THREAD_PRIORITY_TIME_CRITICAL);
#endif
    return true;
}

#ifdef _WIN32
bool AudioPlayer::OpenDevice() {
    HRESULT hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  kIID_IMMDeviceEnumerator, (void **)&enumerator_);
    if (FAILED(hr)) return false;
    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr)) return false;
    hr = device_->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void **)&client_);
    if (FAILED(hr)) return false;

    // Ask WASAPI for 44.1 kHz stereo float directly and let it resample to the
    // endpoint rate; that keeps our own pipeline free of rate conversion.
    WAVEFORMATEXTENSIBLE wfx{};
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = kChannels;
    wfx.Format.nSamplesPerSec = kRate;
    wfx.Format.wBitsPerSample = 32;
    wfx.Format.nBlockAlign = kChannels * 4;
    wfx.Format.nAvgBytesPerSec = kRate * wfx.Format.nBlockAlign;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wfx.SubFormat = kSubtypeIeeeFloat;

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    const REFERENCE_TIME duration = 40 * 10000; // 40 ms

    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0,
                             (WAVEFORMATEX *)&wfx, nullptr);
    if (FAILED(hr)) {
        LOGW("audio: Initialize(44100/float) failed 0x%08lX, trying mix format",
             (unsigned long)hr);
        WAVEFORMATEX *mix = nullptr;
        if (FAILED(client_->GetMixFormat(&mix)) || !mix) return false;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration, 0, mix,
                                 nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) return false;
    }

    if (FAILED(client_->GetBufferSize(&bufferFrames_))) return false;

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_ || FAILED(client_->SetEventHandle(event_))) return false;
    if (FAILED(client_->GetService(kIID_IAudioRenderClient, (void **)&render_))) return false;

    // Started by SetActive() when a client starts streaming audio.
    LOGI("audio: WASAPI ready (%u frame buffer)", bufferFrames_);
    return true;
}

void AudioPlayer::SetActive(bool active) {
    if (!client_) return;
    if (active) {
        client_->Start();
    } else {
        client_->Stop();
    }
}

void AudioPlayer::CloseDevice() {
    if (client_) client_->Stop();
    if (render_) { render_->Release(); render_ = nullptr; }
    if (client_) { client_->Release(); client_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
    if (event_) { CloseHandle(event_); event_ = nullptr; }
}

#else // macOS: the default-output AudioUnit pulls from the ring buffer
bool AudioPlayer::OpenDevice() {
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &unit_) != noErr) return false;

    // 44.1 kHz stereo float in; the unit converts to whatever the device runs.
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = kRate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = kChannels;
    fmt.mBitsPerChannel = 32;
    fmt.mBytesPerFrame = kChannels * 4;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerPacket = fmt.mBytesPerFrame;

    AURenderCallbackStruct cb{RenderCb, this};
    if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                             0, &fmt, sizeof(fmt)) != noErr ||
        AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0, &cb, sizeof(cb)) != noErr ||
        AudioUnitInitialize(unit_) != noErr) {
        CloseDevice();
        return false;
    }
    LOGI("audio: Core Audio output ready");
    return true;
}

void AudioPlayer::CloseDevice() {
    if (!unit_) return;
    AudioOutputUnitStop(unit_);
    AudioUnitUninitialize(unit_);
    AudioComponentInstanceDispose(unit_);
    unit_ = nullptr;
}

void AudioPlayer::SetActive(bool active) {
    if (!unit_) return;
    if (active) {
        AudioOutputUnitStart(unit_);
    } else {
        AudioOutputUnitStop(unit_);
    }
}

OSStatus AudioPlayer::RenderCb(void *self, AudioUnitRenderActionFlags *flags,
                               const AudioTimeStamp *, UInt32, UInt32 frames,
                               AudioBufferList *data) {
    auto *dst = (float *)data->mBuffers[0].mData;
    const int got = ((AudioPlayer *)self)->PopSamples(dst, (int)frames);
    if (got < (int)frames) {
        memset(dst + (size_t)got * kChannels, 0,
               (size_t)(frames - got) * kChannels * sizeof(float));
    }
    if (got == 0) *flags |= kAudioUnitRenderAction_OutputIsSilence;
    return noErr;
}
#endif

void AudioPlayer::Shutdown() {
#ifdef _WIN32
    running_.store(false);
    if (event_) SetEvent(event_);
    if (thread_) {
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
#endif
    CloseDevice();
    CloseCodec();
    if (frame_) av_frame_free(&frame_);
    if (packet_) av_packet_free(&packet_);
}

#ifdef _WIN32
unsigned __stdcall AudioPlayer::ThreadProc(void *self) {
    ((AudioPlayer *)self)->RenderLoop();
    return 0;
}

void AudioPlayer::RenderLoop() {
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    // A stopped client never signals, so this sleeps until SetActive(true) or
    // Shutdown() - no polling while nothing is connected.
    while (running_.load()) {
        if (WaitForSingleObject(event_, INFINITE) != WAIT_OBJECT_0) continue;
        if (!running_.load()) break;

        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) continue;
        UINT32 avail = bufferFrames_ - padding;
        if (avail == 0) continue;

        BYTE *buf = nullptr;
        if (FAILED(render_->GetBuffer(avail, &buf))) continue;

        const int got = PopSamples((float *)buf, (int)avail);
        DWORD flags = 0;
        if (got == 0) {
            flags = AUDCLNT_BUFFERFLAGS_SILENT;
        } else if (got < (int)avail) {
            memset((float *)buf + (size_t)got * kChannels, 0,
                   (size_t)(avail - got) * kChannels * sizeof(float));
        }
        render_->ReleaseBuffer(avail, flags);
    }

    if (task) AvRevertMmThreadCharacteristics(task);
}
#endif

void AudioPlayer::PushSamples(const float *src, int frames) {
    if (frames <= 0) return;
    std::lock_guard<std::mutex> lk(ringMutex_);
    const size_t cap = ring_.size();
    size_t need = (size_t)frames * kChannels;

    // Never let latency grow without bound: drop the oldest audio instead.
    if (need > cap) {
        src += (need - cap);
        need = cap;
    }
    if (filled_ + need > cap) {
        const size_t drop = filled_ + need - cap;
        readPos_ = (readPos_ + drop) % cap;
        filled_ -= drop;
    }
    for (size_t i = 0; i < need; i++) {
        ring_[writePos_] = src[i];
        writePos_ = (writePos_ + 1) % cap;
    }
    filled_ += need;
}

int AudioPlayer::PopSamples(float *dst, int frames) {
    std::lock_guard<std::mutex> lk(ringMutex_);
    const size_t cap = ring_.size();
    size_t want = (size_t)frames * kChannels;
    if (want > filled_) want = filled_;
    if (want == 0) return 0;

    const float g = muted_.load() ? 0.0f : gain_.load();
    for (size_t i = 0; i < want; i++) {
        dst[i] = ring_[readPos_] * g;
        readPos_ = (readPos_ + 1) % cap;
    }
    filled_ -= want;
    return (int)(want / kChannels);
}

void AudioPlayer::SetVolumeDb(float db) {
    float g = (db <= -144.0f) ? 0.0f : powf(10.0f, db / 20.0f);
    gain_.store(Clampf(g, 0.0f, 1.0f));
}

bool AudioPlayer::OpenCodec(unsigned char ct) {
    CloseCodec();

    AVCodecID id = AV_CODEC_ID_NONE;
    const uint8_t *extra = nullptr;
    int extraSize = 0;

    switch (ct) {
    case 2:
        id = AV_CODEC_ID_ALAC;
        extra = kAlacCookie;
        extraSize = sizeof(kAlacCookie);
        break;
    case 4:
        id = AV_CODEC_ID_AAC;
        extra = kAacLcAsc;
        extraSize = sizeof(kAacLcAsc);
        break;
    case 8:
        id = AV_CODEC_ID_AAC;
        extra = kAacEldAsc;
        extraSize = sizeof(kAacEldAsc);
        break;
    case 1:
        ct_ = 1; // raw S16LE, no decoder needed
        LOGI("audio: LPCM 44100/16/2");
        return true;
    default:
        LOGW("audio: unsupported compression type %u", (unsigned)ct);
        return false;
    }

    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) {
        LOGE("audio: decoder for ct=%u missing from FFmpeg build", (unsigned)ct);
        return false;
    }
    codec_ = avcodec_alloc_context3(codec);
    if (!codec_) return false;

    codec_->sample_rate = kRate;
    av_channel_layout_default(&codec_->ch_layout, kChannels);
    codec_->extradata = (uint8_t *)av_mallocz(extraSize + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!codec_->extradata) {
        avcodec_free_context(&codec_);
        return false;
    }
    memcpy(codec_->extradata, extra, extraSize);
    codec_->extradata_size = extraSize;

    if (avcodec_open2(codec_, codec, nullptr) < 0) {
        LOGE("audio: avcodec_open2 failed for ct=%u", (unsigned)ct);
        avcodec_free_context(&codec_);
        return false;
    }

    ct_ = ct;
    LOGI("audio: opened %s decoder (ct=%u)",
         ct == 2 ? "ALAC" : (ct == 8 ? "AAC-ELD" : "AAC-LC"), (unsigned)ct);
    return true;
}

void AudioPlayer::CloseCodec() {
    if (codec_) avcodec_free_context(&codec_);
    if (swr_) {
        swr_free(&swr_);
        swr_ = nullptr;
    }
    ct_ = 0;
}

void AudioPlayer::SetFormat(unsigned char ct) {
    if (ct == ct_ && (codec_ || ct == 1)) return;
    OpenCodec(ct);
    Flush();
}

void AudioPlayer::Flush() {
    if (codec_) avcodec_flush_buffers(codec_);
    std::lock_guard<std::mutex> lk(ringMutex_);
    readPos_ = writePos_ = filled_ = 0;
}

void AudioPlayer::Submit(const uint8_t *data, int len) {
    if (!data || len <= 0) return;

    // Uncompressed path.
    if (ct_ == 1) {
        const int frames = len / (kChannels * 2);
        convBuf_.resize((size_t)frames * kChannels);
        const int16_t *pcm = (const int16_t *)data;
        for (int i = 0; i < frames * kChannels; i++) convBuf_[i] = pcm[i] / 32768.0f;
        PushSamples(convBuf_.data(), frames);
        return;
    }

    if (!codec_) return;

    packet_->data = (uint8_t *)data;
    packet_->size = len;
    if (avcodec_send_packet(codec_, packet_) < 0) return;

    while (avcodec_receive_frame(codec_, frame_) == 0) {
        AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
        if (!swr_) {
            if (swr_alloc_set_opts2(&swr_, &outLayout, AV_SAMPLE_FMT_FLT, kRate,
                                    &frame_->ch_layout, (AVSampleFormat)frame_->format,
                                    frame_->sample_rate, 0, nullptr) < 0 ||
                swr_init(swr_) < 0) {
                av_frame_unref(frame_);
                return;
            }
        }

        const int maxOut = (int)swr_get_out_samples(swr_, frame_->nb_samples) + 64;
        convBuf_.resize((size_t)maxOut * kChannels);
        uint8_t *out[1] = {(uint8_t *)convBuf_.data()};
        const int got = swr_convert(swr_, out, maxOut,
                                    (const uint8_t **)frame_->extended_data,
                                    frame_->nb_samples);
        if (got > 0) PushSamples(convBuf_.data(), got);
        av_frame_unref(frame_);
    }
}
