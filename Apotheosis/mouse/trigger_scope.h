#ifndef MOUSE_TRIGGER_SCOPE_H
#define MOUSE_TRIGGER_SCOPE_H

#include <algorithm>
#include <cstdint>
#include "trigger_fsm.h"

namespace boss
{

inline int physicalScopeRemainingDelayMs(int delay_ms, int64_t activated_ms,
                                         int64_t now_ms)
{
    if (activated_ms <= 0) return 0;
    return static_cast<int>(std::clamp<int64_t>(
        static_cast<int64_t>(std::max(0, delay_ms)) - (now_ms - activated_ms),
        0, 2000));
}

class ScopeController
{
public:
    struct Action
    {
        bool press_right = false;
        bool release_right = false;
    };

    Action tick(bool in_zone, bool allowed, int mode, int delay_ms, int64_t now_ms)
    {
        Action action;
        if (tap_release_pending_ && now_ms >= tap_release_at_ms_)
        {
            action.release_right = true;
            tap_release_pending_ = false;
        }

        if (engaged_ && mode != mode_)
        {
            if (mode_ >= 2 || tap_release_pending_)
                action.release_right = true;
            tap_release_pending_ = false;
            engaged_ = false;
        }
        mode_ = mode;

        const bool want = in_zone && allowed && mode > 0;
        if (!want)
        {
            if (engaged_ && mode >= 2)
            {
                engaged_ = false;
                action.release_right = true;
            }
            return action;
        }

        if (!engaged_)
        {
            engaged_ = true;
            opened_ms_ = now_ms;
            action.press_right = true;
            if (mode == 1)
            {
                tap_release_pending_ = true;
                tap_release_at_ms_ = now_ms + kTapHoldMs;
            }
        }
        return action;
    }

    bool ready(bool allowed, int mode, int delay_ms, int64_t now_ms) const
    {
        if (!allowed || mode <= 0)
            return true;
        if (!engaged_)
            return false;
        if (mode == 1 && tap_release_pending_)
            return false;
        if (delay_ms <= 0)
            return true;
        return (now_ms - opened_ms_) >= delay_ms;
    }

    int remainingDelayMs(bool allowed, int mode, int delay_ms, int64_t now_ms) const
    {
        if (!allowed || mode <= 0 || !engaged_) return 0;
        const int64_t remaining = static_cast<int64_t>(std::max(0, delay_ms)) -
                                  (now_ms - opened_ms_);
        const int64_t tapRemaining = mode == 1 && tap_release_pending_
            ? tap_release_at_ms_ - now_ms : 0;
        return static_cast<int>(std::clamp<int64_t>(
            std::max(remaining, tapRemaining), 0, 2000));
    }

    Action flushUp()
    {
        Action action;
        if (tap_release_pending_)
        {
            action.release_right = true;
            tap_release_pending_ = false;
        }
        return action;
    }

    void retryTapRelease(int64_t now_ms)
    {
        if (mode_ == 1 && engaged_)
        {
            tap_release_pending_ = true;
            tap_release_at_ms_ = now_ms;
        }
    }

    Action forceRelease()
    {
        Action action = flushUp();
        if (engaged_)
        {
            engaged_ = false;
            if (mode_ >= 2)
                action.release_right = true;
        }
        return action;
    }

    bool engaged() const { return engaged_; }
    int  mode() const { return mode_; }

private:
    static constexpr int64_t kTapHoldMs = 20;
    bool    engaged_ = false;
    bool    tap_release_pending_ = false;
    int     mode_ = 0;
    int64_t opened_ms_ = 0;
    int64_t tap_release_at_ms_ = 0;
};

}

#endif // MOUSE_TRIGGER_SCOPE_H
