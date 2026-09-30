#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace runtime {

// The timer belongs to the overlap of the aim hotkey and a real fire press.
// Trigger presses use the same gate after the driver accepted the press.
class AimpointRecoilGate
{
public:
    double update(bool aimEnabled, bool fireHeld, int64_t nowMs,
                  double speedPxPerSec, double maximumPx)
    {
        if (!aimEnabled || !fireHeld || nowMs <= 0 ||
            !std::isfinite(speedPxPerSec) || !std::isfinite(maximumPx))
        {
            reset();
            return 0.0;
        }
        if (!active_ || nowMs < startedMs_)
        {
            active_ = true;
            startedMs_ = nowMs;
        }
        const double heldSec = static_cast<double>(nowMs - startedMs_) / 1000.0;
        return std::min(std::max(0.0, maximumPx),
                        std::max(0.0, speedPxPerSec) * heldSec);
    }

    void reset()
    {
        active_ = false;
        startedMs_ = 0;
    }

private:
    bool active_ = false;
    int64_t startedMs_ = 0;
};

} // namespace runtime
