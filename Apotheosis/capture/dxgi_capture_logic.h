#pragma once

// Platform-independent pieces of the DXGI desktop-capture source. Nothing here
// includes Windows, DXGI or OpenCV headers, so the rules that decide what is
// captured and how a frame is turned into the detector's input can be tested on
// any machine. The thin D3D11/DXGI layer (dxgi_capture.cpp) only supplies pixels.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dxgi_capture {

// One monitor as DXGI reports it.
struct OutputInfo
{
    std::string deviceName; // e.g. "\\\\.\\DISPLAY1": stable across reboots, unlike an index
    int width = 0;
    int height = 0;
    bool primary = false;
    bool rotated = false;   // desktop rotation other than 0°: not supported
};

// Which monitor to capture. An empty or unknown name means the primary monitor so
// a changed display setup never leaves the user with no capture at all; the
// returned flag tells the caller it had to fall back (to be reported, not hidden).
struct OutputChoice
{
    int index = -1;
    bool fellBack = false;
};

inline OutputChoice chooseOutput(const std::vector<OutputInfo>& outputs, const std::string& wanted)
{
    OutputChoice choice;
    if (outputs.empty()) return choice;
    if (!wanted.empty())
        for (size_t i = 0; i < outputs.size(); ++i)
            if (outputs[i].deviceName == wanted) { choice.index = static_cast<int>(i); return choice; }
    choice.fellBack = !wanted.empty();
    choice.index = 0;
    for (size_t i = 0; i < outputs.size(); ++i)
        if (outputs[i].primary) { choice.index = static_cast<int>(i); break; }
    return choice;
}

// The square cut out of the middle of the monitor (where the crosshair is), as the
// other capture sources do. If the monitor is smaller than the wanted side the whole
// shorter side is used and the result is scaled up to `side` afterwards.
struct CropGeometry
{
    int x = 0, y = 0;  // top-left of the cut in the monitor image
    int size = 0;      // side of the cut
    int side = 0;      // side of the delivered frame
    bool scale = false; // true when `size` != `side`
};

inline CropGeometry centerCrop(int monitorWidth, int monitorHeight, int wantedSide)
{
    CropGeometry g;
    g.side = std::clamp(wantedSide, 32, 2048);
    const int limit = std::min(monitorWidth, monitorHeight);
    if (limit <= 0) { g.side = 0; return g; }
    g.size = std::min(limit, g.side);
    g.x = (monitorWidth - g.size) / 2;
    g.y = (monitorHeight - g.size) / 2;
    g.scale = g.size != g.side;
    return g;
}

// QueryPerformanceCounter ticks -> nanoseconds on the steady clock's timeline (on
// Windows std::chrono::steady_clock is the same counter). Split into whole seconds
// and a remainder so a long uptime cannot overflow the multiplication.
inline int64_t ticksToNs(int64_t ticks, int64_t frequency)
{
    if (frequency <= 0 || ticks < 0) return 0;
    const int64_t whole = ticks / frequency;
    const int64_t part = ticks % frequency;
    return whole * 1000000000LL + part * 1000000000LL / frequency;
}

// Age of a frame in microseconds, or -1 when the present time is unknown or in the
// future (clock mismatch), so a bad timestamp is reported as unknown rather than
// as a tiny or negative latency.
inline int ageUs(int64_t nowNs, int64_t presentedNs)
{
    if (presentedNs <= 0 || presentedNs > nowNs) return -1;
    const int64_t us = (nowNs - presentedNs) / 1000;
    return us > 60 * 1000 * 1000 ? -1 : static_cast<int>(us);
}

// BGRA (what the desktop gives) -> tightly packed BGR for the detector. `src` points
// at the top-left pixel of the region; strides are in bytes. Returns false on
// geometry that cannot be converted safely.
inline bool bgraToBgr(const uint8_t* src, size_t srcStride, int width, int height,
                      uint8_t* dst, size_t dstStride)
{
    if (!src || !dst || width <= 0 || height <= 0) return false;
    if (srcStride < static_cast<size_t>(width) * 4 || dstStride < static_cast<size_t>(width) * 3) return false;
    for (int row = 0; row < height; ++row) {
        const uint8_t* in = src + static_cast<size_t>(row) * srcStride;
        uint8_t* out = dst + static_cast<size_t>(row) * dstStride;
        for (int col = 0; col < width; ++col) {
            out[0] = in[0];
            out[1] = in[1];
            out[2] = in[2];
            in += 4;
            out += 3;
        }
    }
    return true;
}

// Frames per second over roughly one-second windows.
class FpsMeter
{
public:
    // Call once per delivered frame; returns the latest estimate.
    int tick(std::chrono::steady_clock::time_point now)
    {
        if (!started_) { started_ = true; start_ = now; }
        ++count_;
        const double elapsed = std::chrono::duration<double>(now - start_).count();
        if (elapsed >= 1.0) {
            fps_ = static_cast<int>(count_ / elapsed + 0.5);
            count_ = 0;
            start_ = now;
        }
        return fps_;
    }
    // Nothing arrived for a while: report 0 instead of the last good number.
    void idle(std::chrono::steady_clock::time_point now)
    {
        if (started_ && now - start_ > std::chrono::seconds(2)) { fps_ = 0; count_ = 0; start_ = now; }
    }
    int fps() const { return fps_; }
private:
    bool started_ = false;
    std::chrono::steady_clock::time_point start_{};
    int count_ = 0;
    int fps_ = 0;
};

// What to do after the desktop duplication fails.
enum class Failure
{
    Timeout,        // no new frame within the wait: the screen is simply unchanged
    AccessLost,     // mode change, lock screen, UAC, device removed: recreate and carry on
    Unavailable,    // too many duplications in the system / not supported right now: keep trying
    Fatal,          // cannot work at all (e.g. no usable output): give up so the user is told
};

struct RecoveryDecision
{
    bool retry = false;
    int delayMs = 0;
    bool giveUp = false;
};

// Recoverable failures are retried with a growing delay (capped), because a locked
// screen or a mode switch can last a while; a fatal one, or recoverable ones that
// never succeed in a long time, end the source so the capture loop reports it.
inline RecoveryDecision decideRecovery(Failure failure, int consecutiveFailures)
{
    RecoveryDecision d;
    if (failure == Failure::Timeout) return d; // not a failure
    if (failure == Failure::Fatal) { d.giveUp = true; return d; }
    constexpr int kGiveUpAfter = 600; // about a minute at the capped delay
    if (consecutiveFailures >= kGiveUpAfter) { d.giveUp = true; return d; }
    d.retry = true;
    d.delayMs = std::min(100 + consecutiveFailures * 50, 1000);
    return d;
}

} // namespace dxgi_capture
