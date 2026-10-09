// AirMirror - shared utilities.
#pragma once

#include <windows.h>
#include <unknwn.h> // IUnknown: WIN32_LEAN_AND_MEAN keeps it out of windows.h

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

// ---------------------------------------------------------------------------
// Logging (goes to the debugger and, when present, an attached console)
// ---------------------------------------------------------------------------
void LogLine(const char *level, const char *fmt, ...);

#define LOGI(...) LogLine("info", __VA_ARGS__)
#define LOGW(...) LogLine("warn", __VA_ARGS__)
#define LOGE(...) LogLine("err ", __VA_ARGS__)
#ifdef NDEBUG
#define LOGD(...) ((void)0)
#else
#define LOGD(...) LogLine("dbg ", __VA_ARGS__)
#endif

// ---------------------------------------------------------------------------
// Minimal COM smart pointer (avoids depending on WRL/ATL under MinGW)
// ---------------------------------------------------------------------------
template <class T>
class Com {
  public:
    Com() = default;
    Com(const Com &o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Com(Com &&o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~Com() { reset(); }

    Com &operator=(const Com &o) {
        if (this != &o) { reset(); p_ = o.p_; if (p_) p_->AddRef(); }
        return *this;
    }
    Com &operator=(Com &&o) noexcept {
        if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }

    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T *get() const { return p_; }
    T *operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    // For APIs that write into a T** out-parameter.
    T **put() { reset(); return &p_; }
    void **putVoid() { reset(); return reinterpret_cast<void **>(&p_); }

    // Takes ownership of an already-AddRef'd pointer.
    void attach(T *p) { reset(); p_ = p; }

  private:
    T *p_ = nullptr;
};

// ---------------------------------------------------------------------------
// ID3D10Multithread, declared inline so we do not have to include d3d10.h
// just to turn on device-level thread protection.
// ---------------------------------------------------------------------------
struct IMultithreadLite : public IUnknown {
    virtual void STDMETHODCALLTYPE Enter() = 0;
    virtual void STDMETHODCALLTYPE Leave() = 0;
    virtual BOOL STDMETHODCALLTYPE SetMultithreadProtected(BOOL enable) = 0;
    virtual BOOL STDMETHODCALLTYPE GetMultithreadProtected() = 0;
};
extern const GUID IID_MultithreadLite;

// ---------------------------------------------------------------------------
// Misc helpers
// ---------------------------------------------------------------------------
inline float Clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
inline float Lerpf(float a, float b, float t) { return a + (b - a) * t; }

std::wstring Widen(const std::string &s);
std::string Narrow(const std::wstring &s);

// Monotonic seconds since process start.
double NowSeconds();
