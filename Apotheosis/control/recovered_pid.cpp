#include "recovered_pid.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace control {
namespace {

// The integral path is exactly the frozen second-port behavior. The only change
// kept from the stabilizer work is the low-pass on the derivative term: it turns
// kd into usable phase lead (instead of amplified per-frame/dt jitter) so kp can
// go higher, and it never injects anything into the integral. derivAlpha is the
// filter coefficient for this tick; 0 reproduces the raw frozen derivative.
float axisPid(float error, float kp, float ki, float kd, float dt,
              bool preserveIntegralOnReverse, float derivAlpha,
              float& integral, float& previous, float& dFilter, float& rawD)
{
    if (!preserveIntegralOnReverse && error * integral < 0.0f)
        integral = 0.0f;
    integral += ki * error * dt;
    if (ki > 0.0f)
        integral = std::clamp(integral, -ki, ki);
    rawD = kd * (error - previous) / dt;
    float dTerm = rawD;
    if (derivAlpha > 0.0f)
    {
        dFilter += derivAlpha * (rawD - dFilter);
        dTerm = dFilter;
    }
    else
        dFilter = rawD; // keep state coherent if the filter is toggled off
    const float output = kp * error + integral + dTerm;
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
    // The one always-on change to the frozen formula: a fixed ~20 Hz low-pass on
    // the derivative term so kd becomes usable phase lead (kp can go higher)
    // rather than amplified per-frame/dt jitter. P and I stay exactly frozen.
    constexpr float kDerivativeCutoffHz = 20.0f;
    const float derivAlpha = dt / (dt + 1.0f / (2.0f * 3.14159265f * kDerivativeCutoffHz));
    float rawDx = 0.0f, rawDy = 0.0f;
    float px = 0.0f, py = 0.0f;
    if (config_.maskX) {
        integralX_ = carryX_ = 0.0f;
        previousX_ = ex;
        dFilterX_ = 0.0f;
    } else px = axisPid(ex, config_.kpX, config_.kiX, config_.kdX, dt,
                       config_.preserveIntegralOnReverse, derivAlpha,
                       integralX_, previousX_, dFilterX_, rawDx);
    if (config_.maskY) {
        integralY_ = carryY_ = 0.0f;
        previousY_ = ey;
        dFilterY_ = 0.0f;
    } else py = axisPid(ey, config_.kpY, config_.kiY, config_.kdY, dt,
                       config_.preserveIntegralOnReverse, derivAlpha,
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
