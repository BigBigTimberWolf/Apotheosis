#ifndef MOUSE_TRIGGER_SCOPE_H
#define MOUSE_TRIGGER_SCOPE_H

#include <cstdint>

namespace boss
{

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
        if (tap_release_pending_)
        {
            action.release_right = true;
            tap_release_pending_ = false;
        }

        if (engaged_ && mode != mode_)
        {
            if (mode_ >= 2)
                action.release_right = true;
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
                tap_release_pending_ = true;
        }
        return action;
    }

    bool ready(bool allowed, int mode, int delay_ms, int64_t now_ms) const
    {
        if (!allowed || mode <= 0)
            return true;
        if (!engaged_)
            return false;
        if (delay_ms <= 0)
            return true;
        return (now_ms - opened_ms_) >= delay_ms;
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
    bool    engaged_ = false;
    bool    tap_release_pending_ = false;
    int     mode_ = 0;
    int64_t opened_ms_ = 0;
};

}

#endif // MOUSE_TRIGGER_SCOPE_H
