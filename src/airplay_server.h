// AirMirror - thin wrapper over UxPlay's libairplay.
//
// Service discovery: on Windows, UxPlay's built-in mDNS responder (lib/mdnsd),
// so the machine does NOT need Apple's Bonjour service installed. On macOS,
// the system's own mDNSResponder through UxPlay's dns_sd backend.
#pragma once

#include "common.h"

#include <string>
#include <vector>

struct raop_s;
struct dnssd_s;

// Callbacks are invoked on libairplay's own network threads.
class AirPlaySink {
  public:
    virtual ~AirPlaySink() = default;

    virtual void OnClientRequest(const std::string &deviceId, const std::string &model,
                                 const std::string &name, bool &admit) = 0;
    virtual void OnConnectionOpened() = 0;
    virtual void OnConnectionClosed() = 0;

    virtual bool OnVideoCodec(bool h265) = 0;
    virtual void OnVideoSize(float srcW, float srcH, float w, float h) = 0;
    virtual void OnVideoData(const uint8_t *data, int len) = 0;
    virtual void OnVideoFlush() = 0;
    virtual void OnVideoPause(bool paused) = 0;

    virtual void OnAudioFormat(unsigned char ct) = 0;
    virtual void OnAudioData(const uint8_t *data, int len) = 0;
    virtual void OnAudioFlush() = 0;
    virtual void OnVolume(float db) = 0;
};

struct AirPlayConfig {
    std::string serviceName = "AirMirror";
    unsigned short width = 1920;
    unsigned short height = 1080;
    unsigned short refreshRate = 60;
    unsigned short maxFps = 60;
    bool allowH265 = false;
};

class AirPlayServer {
  public:
    bool Start(const AirPlayConfig &cfg, AirPlaySink *sink);
    void Stop();

    AirPlaySink *sink() const { return sink_; }
    const std::string &ServiceName() const { return name_; }
    unsigned short Port() const { return port_; }
    bool Running() const { return raop_ != nullptr; }

  private:
    bool FindMac(std::string &macText, std::vector<char> &macBytes);

    raop_s *raop_ = nullptr;
    dnssd_s *dnssd_ = nullptr;
    AirPlaySink *sink_ = nullptr;
    std::string name_;
    unsigned short port_ = 0;
    bool wsaStarted_ = false;
};
