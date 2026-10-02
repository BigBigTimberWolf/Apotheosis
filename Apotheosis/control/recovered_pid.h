#pragma once

#include "types.h"

namespace control {

// Direct-output branch recovered from 0xAED20 and 0x15917D..0x15A530.
// User-requested frozen baseline: this second-port PIDF (P/I/D, FF, deadzone,
// clipping, segmentation, carry and configuration-state rules) must not be
// changed by later controller work unless the user explicitly authorizes it.
// Values are in image pixels until the final segment division and rounding.
struct RecoveredPidConfig
{
    // Used by the outer following compensator, never by the PID formula.
    float followX = 0.0f, followY = 0.0f;
    float kpX = 0.4f, kiX = 0.02f, kdX = 0.12f;
    float kpY = 0.4f, kiY = 0.02f, kdY = 0.12f;
    float deadzoneX = 5.0f, deadzoneY = 5.0f;
    // FF input is selected track velocity with successful-move feedback.
    float feedforwardX = 0.0f, feedforwardY = 0.0f;
    float smoothMaxPixel = 50.0f;
    bool segmentEnabled = false;
    float segment = 3.0f;
    bool skipDeadzone = false;
    bool preserveIntegralOnReverse = false;
    bool maskX = false, maskY = false;
};

struct RecoveredPidStep
{
    Counts counts{};
    Vec2 pid{};
    Vec2 afterDeadzone{};
    Vec2 beforeRounding{};
    Vec2 carry{};
    Vec2 integral{};
    Vec2 derivativeRaw{};
};

class RecoveredPid
{
public:
    void setConfig(const RecoveredPidConfig& config);
    const RecoveredPidConfig& config() const { return config_; }
    RecoveredPidStep update(Vec2 error, Vec2 trackedFeedforward, double dtSec);
    void reset();
    void resetIntegral();
    void resetAxes(bool x, bool y);
    void seedDerivativeAfterPause() { seedResetX_ = seedResetY_ = true; }

private:
    RecoveredPidConfig config_{};
    float integralX_ = 0.0f, integralY_ = 0.0f;
    float previousX_ = 0.0f, previousY_ = 0.0f;
    float carryX_ = 0.0f, carryY_ = 0.0f;
    bool configured_ = false;
    bool skipOutputOnce_ = false;
    bool seedResetX_ = false, seedResetY_ = false;
};

} // namespace control
