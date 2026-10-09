#include "airplay_server.h"

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <map>
#endif

#include <cstdio>
#include <cstring>

#include "raop.h"
#include "dnssd.h"
#include "stream.h"

// ---------------------------------------------------------------------------
// libairplay callbacks. `cls` is always the AirPlayServer instance.
// ---------------------------------------------------------------------------
namespace {

AirPlaySink *SinkOf(void *cls) {
    auto *srv = (AirPlayServer *)cls;
    return srv ? srv->sink() : nullptr;
}

extern "C" void cb_conn_init(void *cls) {
    if (auto *s = SinkOf(cls)) s->OnConnectionOpened();
}

extern "C" void cb_conn_destroy(void *cls) {
    if (auto *s = SinkOf(cls)) s->OnConnectionClosed();
}

extern "C" void cb_conn_reset(void *cls, int reason) {
    (void)reason;
    if (auto *s = SinkOf(cls)) s->OnConnectionClosed();
}

extern "C" void cb_conn_feedback(void *cls) { (void)cls; }

extern "C" void cb_video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *data) {
    (void)ntp;
    if (auto *s = SinkOf(cls)) s->OnVideoData(data->data, data->data_len);
}

extern "C" void cb_audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data) {
    (void)ntp;
    if (auto *s = SinkOf(cls)) s->OnAudioData(data->data, data->data_len);
}

extern "C" void cb_video_flush(void *cls) {
    if (auto *s = SinkOf(cls)) s->OnVideoFlush();
}

extern "C" void cb_audio_flush(void *cls) {
    if (auto *s = SinkOf(cls)) s->OnAudioFlush();
}

extern "C" void cb_video_pause(void *cls) {
    if (auto *s = SinkOf(cls)) s->OnVideoPause(true);
}

extern "C" void cb_video_resume(void *cls) {
    if (auto *s = SinkOf(cls)) s->OnVideoPause(false);
}

extern "C" void cb_video_reset(void *cls, reset_type_t type) {
    (void)type;
    if (auto *s = SinkOf(cls)) s->OnVideoFlush();
}

extern "C" int cb_video_set_codec(void *cls, video_codec_t codec) {
    auto *s = SinkOf(cls);
    if (!s) return 0;
    return s->OnVideoCodec(codec == VIDEO_CODEC_H265) ? 0 : -1;
}

// Fired on every codec change, which iOS sends whenever the device rotates.
extern "C" void cb_video_report_size(void *cls, float *srcW, float *srcH, float *w,
                                     float *h) {
    if (auto *s = SinkOf(cls)) s->OnVideoSize(*srcW, *srcH, *w, *h);
}

extern "C" void cb_audio_get_format(void *cls, unsigned char *ct, unsigned short *spf,
                                    bool *usingScreen, bool *isMedia,
                                    uint64_t *audioFormat) {
    (void)spf;
    (void)usingScreen;
    (void)isMedia;
    (void)audioFormat;
    if (auto *s = SinkOf(cls)) s->OnAudioFormat(*ct);
}

extern "C" double cb_audio_set_client_volume(void *cls) {
    (void)cls;
    return 0.0; // request full volume from the client
}

extern "C" void cb_audio_set_volume(void *cls, float volume) {
    if (auto *s = SinkOf(cls)) s->OnVolume(volume);
}

extern "C" void cb_report_client_request(void *cls, char *deviceid, char *model,
                                         char *name, bool *admit) {
    *admit = true;
    if (auto *s = SinkOf(cls)) {
        bool ok = true;
        s->OnClientRequest(deviceid ? deviceid : "", model ? model : "",
                           name ? name : "", ok);
        *admit = ok;
    }
}

extern "C" void cb_log(void *cls, int level, const char *msg) {
    (void)cls;
    if (level <= 3) { // LOGGER_ERR and worse
        LOGE("libairplay: %s", msg);
    } else if (level == 4) { // LOGGER_WARNING
        LOGW("libairplay: %s", msg);
    } else if (level <= 6) { // LOGGER_NOTICE / LOGGER_INFO
        LOGI("libairplay: %s", msg);
    } else {
        LOGD("libairplay: %s", msg);
    }
}

} // namespace

// ---------------------------------------------------------------------------
#ifdef __APPLE__
bool AirPlayServer::FindMac(std::string &macText, std::vector<char> &macBytes) {
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return false;

    // An interface qualifies when it is up, has a 6-byte hardware address and
    // a routable IPv4 address. Prefer the lowest-numbered enN, which is the
    // built-in Ethernet or Wi-Fi; utun/awdl/bridge interfaces never have both.
    std::map<std::string, std::vector<uint8_t>> macs;
    std::map<std::string, bool> hasIpv4;
    for (ifaddrs *a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || !(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        if (a->ifa_addr->sa_family == AF_LINK) {
            auto *sdl = (sockaddr_dl *)a->ifa_addr;
            if (sdl->sdl_alen == 6) {
                auto *p = (const uint8_t *)LLADDR(sdl);
                macs[a->ifa_name].assign(p, p + 6);
            }
        } else if (a->ifa_addr->sa_family == AF_INET) {
            const uint32_t host = ntohl(((sockaddr_in *)a->ifa_addr)->sin_addr.s_addr);
            if ((host & 0xFFFF0000u) != 0xA9FE0000u) hasIpv4[a->ifa_name] = true; // not 169.254
        }
    }
    freeifaddrs(list);

    std::string chosen;
    for (const auto &kv : macs) {
        if (!hasIpv4[kv.first]) continue;
        const bool en = kv.first.rfind("en", 0) == 0;
        const bool chosenEn = chosen.rfind("en", 0) == 0;
        if (chosen.empty() || (en && !chosenEn) ||
            (en == chosenEn && kv.first.size() <= chosen.size() && kv.first < chosen)) {
            chosen = kv.first;
        }
    }
    if (chosen.empty()) return false;

    const std::vector<uint8_t> &m = macs[chosen];
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4],
             m[5]);
    macText = buf;
    macBytes.assign(m.begin(), m.end());
    LOGI("network interface: %s (%s)", chosen.c_str(), macText.c_str());
    return true;
}
#else
bool AirPlayServer::FindMac(std::string &macText, std::vector<char> &macBytes) {
    ULONG buflen = 16 * 1024;
    std::vector<uint8_t> storage(buflen);
    auto *addresses = (IP_ADAPTER_ADDRESSES *)storage.data();

    // INCLUDE_GATEWAYS or FirstGatewayAddress comes back NULL for everything.
    const ULONG kFlags = GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, kFlags, nullptr, addresses, &buflen);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        storage.resize(buflen);
        addresses = (IP_ADAPTER_ADDRESSES *)storage.data();
        rc = GetAdaptersAddresses(AF_UNSPEC, kFlags, nullptr, addresses, &buflen);
    }
    if (rc != NO_ERROR) return false;

    // Two passes: a real LAN adapter first, then anything plausible. WSL,
    // Hyper-V and Docker all present themselves as Ethernet and up, and taking
    // the first match hands out a virtual switch's MAC as the AirPlay device id.
    // A host-only switch has no default gateway; that is what tells them apart.
    const IP_ADAPTER_ADDRESSES *chosen = nullptr;
    ULONG bestMetric = 0xFFFFFFFFu;
    for (int pass = 0; pass < 2 && !chosen; pass++) {
        for (auto *a = addresses; a; a = a->Next) {
            // Ethernet (6) or 802.11 (71), and the interface must be up.
            if (a->PhysicalAddressLength != 6) continue;
            if (a->IfType != 6 && a->IfType != 71) continue;
            if (a->OperStatus != IfOperStatusUp) continue;
            if (pass == 0 && !a->FirstGatewayAddress) continue;
            if (pass == 0 && a->Ipv4Metric >= bestMetric) continue;
            if (pass == 0) {
                bestMetric = a->Ipv4Metric;
                chosen = a;
            } else {
                chosen = a;
                break;
            }
        }
    }
    if (!chosen) return false;

    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", chosen->PhysicalAddress[0],
             chosen->PhysicalAddress[1], chosen->PhysicalAddress[2],
             chosen->PhysicalAddress[3], chosen->PhysicalAddress[4],
             chosen->PhysicalAddress[5]);
    macText = buf;
    macBytes.assign((const char *)chosen->PhysicalAddress,
                    (const char *)chosen->PhysicalAddress + 6);
    LOGI("network adapter: %ls (%s)", chosen->FriendlyName ? chosen->FriendlyName : L"?",
         macText.c_str());
    return true;
}
#endif

bool AirPlayServer::Start(const AirPlayConfig &cfg, AirPlaySink *sink) {
    sink_ = sink;
    name_ = cfg.serviceName;

    // libairplay initialises Winsock inside raop_init(), but dnssd_init() runs
    // first and calls gethostname(), which fails without it - leaving the mDNS
    // responder advertising a fallback host name. Start Winsock up front.
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        wsaStarted_ = true;
    }
#endif

    std::string macText;
    std::vector<char> macBytes;
    if (!FindMac(macText, macBytes)) {
        LOGE("no active Ethernet or Wi-Fi adapter found");
        return false;
    }
    LOGI("using adapter MAC %s", macText.c_str());

    int err = 0;
    dnssd_ = dnssd_init(name_.c_str(), (int)name_.size(), macBytes.data(),
                        (int)macBytes.size(), 0, &err);
    if (!dnssd_ || err) {
        LOGE("dnssd_init failed with error %d", err);
        return false;
    }
    // Everything else keeps libairplay's default feature set; bit 42 opts into
    // the multi-codec (H.265) path, which we only want when asked for.
    dnssd_set_airplay_features(dnssd_, 42, cfg.allowH265 ? 1 : 0);

    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.cls = this;
    cbs.conn_init = cb_conn_init;
    cbs.conn_destroy = cb_conn_destroy;
    cbs.conn_reset = cb_conn_reset;
    cbs.conn_feedback = cb_conn_feedback;
    cbs.audio_process = cb_audio_process;
    cbs.video_process = cb_video_process;
    cbs.audio_flush = cb_audio_flush;
    cbs.video_flush = cb_video_flush;
    cbs.video_pause = cb_video_pause;
    cbs.video_resume = cb_video_resume;
    cbs.video_reset = cb_video_reset;
    cbs.video_set_codec = cb_video_set_codec;
    cbs.video_report_size = cb_video_report_size;
    cbs.audio_get_format = cb_audio_get_format;
    cbs.audio_set_volume = cb_audio_set_volume;
    cbs.audio_set_client_volume = cb_audio_set_client_volume;
    cbs.report_client_request = cb_report_client_request;

    raop_ = raop_init(&cbs);
    if (!raop_) {
        LOGE("raop_init failed");
        return false;
    }

    raop_set_log_callback(raop_, cb_log, nullptr);
    raop_set_log_level(raop_, 6 /* LOGGER_INFO */);
    ntp_global_init();

    // nohold = 1: a new client takes over instead of being refused.
    if (raop_init2(raop_, 1, macText.c_str(), "")) {
        LOGE("raop_init2 failed");
        raop_destroy(raop_);
        raop_ = nullptr;
        return false;
    }

    // Advertised display capabilities. The client picks a stream size that
    // fits inside this while preserving its own aspect ratio.
    raop_set_plist(raop_, "width", cfg.width);
    raop_set_plist(raop_, "height", cfg.height);
    raop_set_plist(raop_, "refreshRate", cfg.refreshRate);
    raop_set_plist(raop_, "maxFPS", cfg.maxFps);

    unsigned short tcp[3] = {0, 0, 0};
    unsigned short udp[3] = {0, 0, 0};
    raop_set_tcp_ports(raop_, tcp);
    raop_set_udp_ports(raop_, udp);

    // httpd_start returns 1 when it starts, 0 when already running, <0 on error.
    port_ = raop_get_port(raop_);
    if (raop_start_httpd(raop_, &port_) < 0) {
        LOGE("raop_start_httpd failed");
        raop_destroy(raop_);
        raop_ = nullptr;
        return false;
    }
    raop_set_port(raop_, port_);
    raop_set_dnssd(raop_, dnssd_);

    if (dnssd_register_raop(dnssd_, port_)) {
        LOGE("failed to advertise _raop._tcp");
        return false;
    }
    if (dnssd_register_airplay(dnssd_, port_)) {
        LOGE("failed to advertise _airplay._tcp");
        return false;
    }

#ifdef _WIN32
    LOGI("AirPlay receiver \"%s\" listening on port %u (built-in mDNS, no Bonjour)",
         name_.c_str(), (unsigned)port_);
#else
    LOGI("AirPlay receiver \"%s\" listening on port %u", name_.c_str(), (unsigned)port_);
#endif
    return true;
}

void AirPlayServer::Stop() {
    if (dnssd_) {
        dnssd_unregister_raop(dnssd_);
        dnssd_unregister_airplay(dnssd_);
    }
    if (raop_) {
        raop_destroy(raop_);
        raop_ = nullptr;
    }
    if (dnssd_) {
        dnssd_destroy(dnssd_);
        dnssd_ = nullptr;
    }
#ifdef _WIN32
    if (wsaStarted_) {
        WSACleanup();
        wsaStarted_ = false;
    }
#endif
    sink_ = nullptr;
}
