#pragma once

#include "types.h"

namespace control {

// Direct-output branch recovered from 0xAED20 and 0x15917D..0x15A530.
// Based on the user-requested second-port baseline. Later authorized changes
// include D/I/deadzone stabilization and (2026-10-03) removal of the extra FF
// filter, calibrated self-motion and shared saturation reporting. Further
// formula changes still require explicit authorization.
// Values are in image pixels until the final segment division and rounding.
struct RecoveredPidConfig
{
    // Used by the outer following compensator, never by the PID formula.
    float followX = 0.0f, followY = 0.0f;
    float kpX = 0.4f, kiX = 0.02f, kdX = 0.12f;
    float kpY = 0.4f, kiY = 0.02f, kdY = 0.12f;
    float deadzoneX = 5.0f, deadzoneY = 5.0f;
    // Extra per-axis free-wiggle zone (separate from the radius above, which is
    // left exactly as it was). While |error| on an axis is within this value the
    // axis outputs nothing at all, so the crosshair can move freely inside it.
    // X and Y are judged independently; 0 = off.
    float hardDeadzoneX = 0.0f, hardDeadzoneY = 0.0f;
    // FF input is selected track velocity with successful-move feedback.
    float feedforwardX = 0.0f, feedforwardY = 0.0f;
    // Measured image pixels per successful mouse count, independently per bank.
    float motionPixelsPerCountX = 0.91f, motionPixelsPerCountY = 0.91f;
    // Send-to-image lag in the capture timestamp domain; -1 keeps the legacy window.
    float motionDelayMs = -1.0f;
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
    Vec2 feedforward{};
    Vec2 saturation{}; // Sign of the actual pre-clip output; zero if not limited.
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
    // Low-pass state for the always-on filtered derivative.
    float dFilterX_ = 0.0f, dFilterY_ = 0.0f;
    bool configured_ = false;
    bool skipOutputOnce_ = false;
    bool seedResetX_ = false, seedResetY_ = false;
};

} // namespace control
