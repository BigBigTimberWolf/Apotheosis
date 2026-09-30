#ifndef MOUSE_AUTO_STOP_H
#define MOUSE_AUTO_STOP_H

#include <algorithm>
#include <cstdint>

namespace boss
{

// 自动急停: 开枪前开始屏蔽，在开枪后指定时间下发 maskoff。
// MAKCUNEW 固件有超时；KMBoxNet 按键屏蔽需要显式解除。
class AutoStopController
{
public:
    enum class Phase { Idle, Preparing, AfterFire, Continuous };

    void reset()
    {
        phase_ = Phase::Idle;
        cancel_at_ms_ = 0;
    }

    bool active() const { return phase_ != Phase::Idle; }
    bool preparing() const { return phase_ == Phase::Preparing; }
    bool continuous() const { return phase_ == Phase::Continuous; }
    Phase phase() const { return phase_; }

    void markPrepared()
    {
        phase_ = Phase::Preparing;
        cancel_at_ms_ = 0;
    }

    void markFired(int64_t now_ms, int after_ms)
    {
        phase_ = Phase::AfterFire;
        cancel_at_ms_ = now_ms + std::max(0, after_ms);
    }

    void markContinuous(int64_t now_ms)
    {
        phase_ = Phase::Continuous;
        // Firmware cannot extend an active mask and hard-stops after 2 s.
        // Refresh before that limit by explicitly releasing and restarting it.
        cancel_at_ms_ = now_ms + 1500;
    }

    bool continuousNeedsRefresh(int64_t now_ms) const
    {
        return continuous() && now_ms >= cancel_at_ms_;
    }

    bool shouldCancel(int64_t now_ms) const
    {
        return phase_ == Phase::AfterFire && now_ms >= cancel_at_ms_;
    }

private:
    Phase phase_ = Phase::Idle;
    int64_t cancel_at_ms_ = 0;
};

}

#endif // MOUSE_AUTO_STOP_H
