#pragma once

#include <algorithm>
#include <cstdint>

namespace mouse_async {
// A keyboard sequence belongs to a shot whose left-down was sent. A failed
// left-up report must not cancel the post-shot sequence.
struct Switch31ShotGate {
    bool armed = false;
    int delayMs = 0;
    int minHoldMs = 20;
    int64_t pressedAtMs = 0;

    void onPress(bool sent, bool enabled, int delay, int hold, int64_t now) {
        armed = sent && enabled;
        delayMs = std::clamp(delay, 0, 2000);
        minHoldMs = std::max(20, hold);
        pressedAtMs = now;
    }

    int onRelease(int64_t now) {
        const bool complete = armed && now - pressedAtMs >= minHoldMs;
        const int delay = complete ? delayMs : -1;
        *this = {};
        return delay;
    }

    int remainingHoldMs(int64_t now) const {
        if (!armed) return 0;
        return static_cast<int>(std::clamp<int64_t>(
            pressedAtMs + minHoldMs - now, 0, minHoldMs));
    }

    void cancel() { *this = {}; }
};
}
