#include "recovered_pid.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace control {
namespace {

// Optional per-axis stabilizers. Defaults here reproduce the frozen baseline:
// derivAlpha==0 keeps the raw derivative, integralLimit<=0 keeps the ±ki clamp,
// and antiWindupGain<=0 / freezeInDeadzone==false disable the new integral paths.
struct AxisTune
{
    float deadzone = 0.0f;
    float derivAlpha = 0.0f;   // low-pass coefficient for the D term
    float integralLimit = 0.0f;
    bool  freezeIntegralInDeadzone = false;
    float antiWindupGain = 0.0f;
    float satLimit = 0.0f;     // smoothMaxPixel, the PID output clip
};

void clampIntegral(float& integral, float ki, float integralLimit)
{
    if (integralLimit > 0.0f)
        integral = std::clamp(integral, -integralLimit, integralLimit);
    else if (ki > 0.0f)
        integral = std::clamp(integral, -ki, ki);
}

float axisPid(float error, float kp, float ki, float kd, float dt,
              bool preserveIntegralOnReverse, const AxisTune& tune,
              float& integral, float& previous, float& dFilter, float& rawD)
{
    if (!preserveIntegralOnReverse && error * integral < 0.0f)
        integral = 0.0f;
    const bool inDeadzone = tune.deadzone > 0.0f && std::abs(error) < tune.deadzone;
    const bool freeze = tune.freezeIntegralInDeadzone && inDeadzone;
    if (!freeze)
        integral += ki * error * dt;
    clampIntegral(integral, ki, tune.integralLimit);
    rawD = kd * (error - previous) / dt;
    // Low-pass the derivative so measurement/dt jitter stops being amplified by
    // ~1/dt. derivAlpha==0 leaves the frozen raw derivative untouched.
    float dTerm = rawD;
    if (tune.derivAlpha > 0.0f)
    {
        dFilter += tune.derivAlpha * (rawD - dFilter);
        dTerm = dFilter;
    }
    else
        dFilter = rawD; // keep state coherent if the filter is toggled on later
    float output = kp * error + integral + dTerm;
    // Back-calculation anti-windup: bleed the integral toward the value that
    // keeps the PID output inside the ±satLimit clip. Skipped while frozen in
    // the deadzone so the two integral rules never fight.
    if (tune.antiWindupGain > 0.0f && tune.satLimit > 0.0f && !freeze)
    {
        const float clipped = std::clamp(output, -tune.satLimit, tune.satLimit);
        if (clipped != output)
        {
            integral += tune.antiWindupGain * (clipped - output) * dt;
            clampIntegral(integral, ki, tune.integralLimit);
            output = kp * error + integral + dTerm;
        }
    }
    previous = error;
    return output;
}

int quantize(float value, float& carry)
{
    const float total = value + carry;
    if (!std::isfinite(total) ||
        std::abs(total) > static_cast<float>(std::numeric_limits<int>::max() - 1))
    {
        carry = 0.0f;
        return 0;
    }
    const int sent = static_cast<int>(std::round(total));
    carry = total - static_cast<float>(sent);
    return sent;
}

} // namespace

void RecoveredPid::setConfig(const RecoveredPidConfig& config)
{
    RecoveredPidConfig clean = config;
    auto finiteRange = [](float value, float fallback, float maximum) {
        return std::isfinite(value) ? std::clamp(value, 0.0f, maximum) : fallback;
    };
    clean.kpX = finiteRange(clean.kpX, 0.4f, 10.0f);
    clean.kiX = finiteRange(clean.kiX, 0.02f, 10.0f);
    clean.kdX = finiteRange(clean.kdX, 0.12f, 10.0f);
    clean.kpY = finiteRange(clean.kpY, 0.4f, 10.0f);
    clean.kiY = finiteRange(clean.kiY, 0.02f, 10.0f);
    clean.kdY = finiteRange(clean.kdY, 0.12f, 10.0f);
    clean.deadzoneX = finiteRange(clean.deadzoneX, 5.0f, 200.0f);
    clean.deadzoneY = finiteRange(clean.deadzoneY, 5.0f, 200.0f);
    clean.feedforwardX = finiteRange(clean.feedforwardX, 0.0f, 10.0f);
    clean.feedforwardY = finiteRange(clean.feedforwardY, 0.0f, 10.0f);
    clean.smoothMaxPixel = finiteRange(clean.smoothMaxPixel, 50.0f, 1000.0f);
    clean.segment = std::isfinite(clean.segment)
        ? std::clamp(clean.segment, 1.0f, 10.0f) : 3.0f;
    clean.followX = finiteRange(clean.followX, 0.0f, 50.0f);
    clean.followY = finiteRange(clean.followY, 0.0f, 50.0f);
    // Frozen second-port configuration behavior: only KpX or the segment
    // setting skips one send; changing either Ki clears that axis's integral.
    if (configured_ && (clean.kpX != config_.kpX ||
                        clean.segmentEnabled != config_.segmentEnabled ||
                        clean.segment != config_.segment))
        skipOutputOnce_ = true;
    if (clean.kiX != config_.kiX) integralX_ = 0.0f;
    if (clean.kiY != config_.kiY) integralY_ = 0.0f;
    if (clean.maskX) integralX_ = carryX_ = 0.0f;
    if (clean.maskY) integralY_ = carryY_ = 0.0f;
    config_ = clean;
    configured_ = true;
}

RecoveredPidStep RecoveredPid::update(Vec2 error, Vec2 trackedFeedforward,
                                     double dtSec)
{
    RecoveredPidStep result;
    if (!std::isfinite(error.x) || !std::isfinite(error.y) || !std::isfinite(dtSec))
        return result;

    const float dt = std::clamp(static_cast<float>(dtSec), 0.001f, 0.1f);
    const float ex = static_cast<float>(error.x);
    const float ey = static_cast<float>(error.y);
    // A macro axis reset has no historical sample. Seed it from the next
    // observation instead of differentiating against an invented zero.
    if (seedResetX_) { previousX_ = ex; dFilterX_ = 0.0f; seedResetX_ = false; }
    if (seedResetY_) { previousY_ = ey; dFilterY_ = 0.0f; seedResetY_ = false; }
    // Always-on stabilizers, baked to good fixed values so there is nothing new
    // to tune. These intentionally diverge from the raw frozen formula:
    //  - filtered derivative: a fixed ~20 Hz low-pass turns kd into usable phase
    //    lead instead of amplifying per-frame/dt jitter, so kp can go higher.
    //  - integral ceiling decoupled from ki (a fraction of the output budget) so
    //    raising ki no longer raises the windup ceiling -> no "i shakes" runaway.
    //  - back-calculation anti-windup (Tt ~= 50 ms) bleeds the integral only when
    //    the output saturates; dormant otherwise.
    //  - integral frozen inside the deadzone to stop at-rest hunting.
    constexpr float kDerivativeCutoffHz = 20.0f;
    constexpr float kIntegralLimitFraction = 0.3f;
    constexpr float kAntiWindupTt = 0.05f; // seconds
    const float derivAlpha = dt / (dt + 1.0f / (2.0f * 3.14159265f * kDerivativeCutoffHz));
    const float satLimit = std::max(0.0f, config_.smoothMaxPixel);
    auto tuneFor = [&](float deadzone) {
        AxisTune tune;
        tune.deadzone = deadzone;
        tune.derivAlpha = derivAlpha;
        tune.integralLimit = kIntegralLimitFraction * satLimit;
        tune.freezeIntegralInDeadzone = true;
        tune.antiWindupGain = 1.0f / kAntiWindupTt;
        tune.satLimit = satLimit;
        return tune;
    };
    float rawDx = 0.0f, rawDy = 0.0f;
    float px = 0.0f, py = 0.0f;
    if (config_.maskX) {
        integralX_ = carryX_ = 0.0f;
        previousX_ = ex;
        dFilterX_ = 0.0f;
    } else px = axisPid(ex, config_.kpX, config_.kiX, config_.kdX, dt,
                       config_.preserveIntegralOnReverse, tuneFor(config_.deadzoneX),
                       integralX_, previousX_, dFilterX_, rawDx);
    if (config_.maskY) {
        integralY_ = carryY_ = 0.0f;
        previousY_ = ey;
        dFilterY_ = 0.0f;
    } else py = axisPid(ey, config_.kpY, config_.kiY, config_.kdY, dt,
                       config_.preserveIntegralOnReverse, tuneFor(config_.deadzoneY),
                       integralY_, previousY_, dFilterY_, rawDy);
    result.pid = { px, py };
    result.integral = { integralX_, integralY_ };
    result.derivativeRaw = { rawDx, rawDy };

    const float dx = config_.deadzoneX, dy = config_.deadzoneY;
    if (!config_.skipDeadzone)
    {
        if (dx > 0.0f && dy > 0.0f)
        {
            const float radius = std::hypot(std::abs(ex) / dx, std::abs(ey) / dy);
            if (radius < 1.0f)
            {
                const float scale = std::max(0.1f, radius);
                px *= scale;
                py *= scale;
            }
        }
        else if (dx > 0.0f && std::abs(ex) < dx)
            px *= std::max(0.1f, std::abs(ex) / dx);
        else if (dy > 0.0f && std::abs(ey) < dy)
            py *= std::max(0.1f, std::abs(ey) / dy);
    }
    result.afterDeadzone = { px, py };

    const float fx = !config_.maskX && std::isfinite(trackedFeedforward.x)
        ? std::max(0.0f, config_.feedforwardX) * static_cast<float>(trackedFeedforward.x) : 0.0f;
    const float fy = !config_.maskY && std::isfinite(trackedFeedforward.y)
        ? std::max(0.0f, config_.feedforwardY) * static_cast<float>(trackedFeedforward.y) : 0.0f;
    const float maxPixel = std::max(0.0f, config_.smoothMaxPixel);
    const float segment = config_.segmentEnabled && std::isfinite(config_.segment)
        ? std::clamp(config_.segment, 1.0f, 10.0f) : 3.0f;
    const float x = std::clamp(px + fx, -maxPixel, maxPixel) / segment;
    const float y = std::clamp(py + fy, -maxPixel, maxPixel) / segment;
    if (skipOutputOnce_)
    {
        skipOutputOnce_ = false;
        result.carry = { carryX_, carryY_ };
        return result;
    }
    result.beforeRounding = { x, y };
    result.counts = { quantize(x, carryX_), quantize(y, carryY_) };
    result.carry = { carryX_, carryY_ };
    return result;
}

void RecoveredPid::resetIntegral()
{
    integralX_ = integralY_ = 0.0f;
}

void RecoveredPid::resetAxes(bool x, bool y)
{
    if (x) { integralX_ = previousX_ = carryX_ = dFilterX_ = 0.0f; seedResetX_ = true; }
    if (y) { integralY_ = previousY_ = carryY_ = dFilterY_ = 0.0f; seedResetY_ = true; }
}

void RecoveredPid::reset()
{
    resetIntegral();
    previousX_ = previousY_ = 0.0f;
    carryX_ = carryY_ = 0.0f;
    dFilterX_ = dFilterY_ = 0.0f;
    configured_ = false;
    skipOutputOnce_ = false;
    seedResetX_ = seedResetY_ = false;
}

} // namespace control
