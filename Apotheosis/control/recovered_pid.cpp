#include "recovered_pid.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace control {
namespace {

struct AxisTune
{
    float derivAlpha = 0.0f;     // low-pass coefficient for the D term (A)
    float deadzone = 0.0f;       // per-axis deadzone radius
    float hardDeadzone = 0.0f;   // free-wiggle zone: no output at all inside (0 = off)
    float integralCeiling = 0.0f;// decoupled integral authority limit (B)
    float satLimit = 0.0f;       // PID output clip = smoothMaxPixel (B)
};

// A: the derivative is low-pass filtered so kd is usable phase lead, not
//    amplified per-frame/dt jitter -> kp can go higher.
// B: conditional-integration anti-windup. When the output is already saturated
//    and integrating would push it further out, accumulation is SKIPPED (never
//    reversed) -- so it never injects an integral that opposes the proportional
//    term, and ki<=0 keeps the integral exactly zero (pure P). The ceiling is
//    decoupled from ki, so raising ki speeds correction without raising the
//    windup ceiling.
// D: inside the deadzone the integral bleeds toward zero so it settles on the
//    point instead of holding a value that nudges across it.
float axisPid(float error, float kp, float ki, float kd, float dt,
              bool preserveIntegralOnReverse, const AxisTune& tune,
              float& integral, float& previous, float& dFilter, float& rawD)
{
    if (!preserveIntegralOnReverse && error * integral < 0.0f)
        integral = 0.0f;

    // Derivative first (the saturation test below needs the full output).
    rawD = kd * (error - previous) / dt;
    float dTerm = rawD;
    if (tune.derivAlpha > 0.0f) { dFilter += tune.derivAlpha * (rawD - dFilter); dTerm = dFilter; }
    else dFilter = rawD;
    previous = error;

    // The free-wiggle zone also lets the integral bleed, so nothing is wound up
    // to kick the crosshair the moment it leaves the zone.
    const bool inDeadzone = (tune.deadzone > 0.0f && std::abs(error) < tune.deadzone) ||
                            (tune.hardDeadzone > 0.0f && std::abs(error) <= tune.hardDeadzone);
    if (ki <= 0.0f)
    {
        integral = 0.0f; // pure-P tune: never any integral action
    }
    else if (inDeadzone)
    {
        integral *= std::exp(-dt / 0.05f); // bleed windup so the output settles
    }
    else
    {
        const float candidate = integral + ki * error * dt;
        const float u = kp * error + candidate + dTerm;
        const bool saturated = tune.satLimit > 0.0f && std::abs(u) >= tune.satLimit;
        const bool pushingOut = u * error > 0.0f; // output drives further in error's dir
        if (!(saturated && pushingOut)) integral = candidate; // else: just stop, don't reverse
        if (tune.integralCeiling > 0.0f)
            integral = std::clamp(integral, -tune.integralCeiling, tune.integralCeiling);
        else
            integral = std::clamp(integral, -ki, ki);
    }

    return kp * error + integral + dTerm;
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
    clean.hardDeadzoneX = finiteRange(clean.hardDeadzoneX, 0.0f, 200.0f);
    clean.hardDeadzoneY = finiteRange(clean.hardDeadzoneY, 0.0f, 200.0f);
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
    if (seedResetX_) { previousX_ = ex; dFilterX_ = ffFilterX_ = 0.0f; seedResetX_ = false; }
    if (seedResetY_) { previousY_ = ey; dFilterY_ = ffFilterY_ = 0.0f; seedResetY_ = false; }
    // Always-on stabilizers, baked to fixed good values (no new knobs):
    //  A  ~20 Hz low-pass on the derivative -> usable kd, higher kp.
    //  B  integral ceiling decoupled from ki (a small fraction of the output
    //     budget) + conditional-integration anti-windup inside axisPid.
    const float maxPixel = std::max(0.0f, config_.smoothMaxPixel);
    constexpr float kDerivativeCutoffHz = 20.0f;
    constexpr float kIntegralCeilingFraction = 0.1f;
    const float derivAlpha = dt / (dt + 1.0f / (2.0f * 3.14159265f * kDerivativeCutoffHz));
    auto tuneFor = [&](float deadzone, float hardDeadzone) {
        AxisTune tune;
        tune.derivAlpha = derivAlpha;
        tune.deadzone = deadzone;
        tune.hardDeadzone = hardDeadzone;
        tune.integralCeiling = kIntegralCeilingFraction * maxPixel;
        tune.satLimit = maxPixel;
        return tune;
    };
    float rawDx = 0.0f, rawDy = 0.0f;
    float px = 0.0f, py = 0.0f;
    if (config_.maskX) {
        integralX_ = carryX_ = 0.0f;
        previousX_ = ex;
        dFilterX_ = ffFilterX_ = 0.0f;
    } else px = axisPid(ex, config_.kpX, config_.kiX, config_.kdX, dt,
                       config_.preserveIntegralOnReverse,
                       tuneFor(config_.deadzoneX, config_.hardDeadzoneX),
                       integralX_, previousX_, dFilterX_, rawDx);
    if (config_.maskY) {
        integralY_ = carryY_ = 0.0f;
        previousY_ = ey;
        dFilterY_ = ffFilterY_ = 0.0f;
    } else py = axisPid(ey, config_.kpY, config_.kiY, config_.kdY, dt,
                       config_.preserveIntegralOnReverse,
                       tuneFor(config_.deadzoneY, config_.hardDeadzoneY),
                       integralY_, previousY_, dFilterY_, rawDy);
    result.pid = { px, py };
    result.integral = { integralX_, integralY_ };
    result.derivativeRaw = { rawDx, rawDy };

    const float dx = config_.deadzoneX, dy = config_.deadzoneY;
    if (!config_.skipDeadzone)
    {
        // D: true-zero inner band. Inside the innermost fraction the output is
        // exactly zero so the aim settles on the point; it then ramps to full at
        // the deadzone edge. The old 0.1 floor left a persistent nudge that, with
        // integer quantization, showed up as a static tremor around the target.
        constexpr float kInner = 0.2f;
        auto band = [](float radius) {
            return radius <= kInner ? 0.0f : (radius - kInner) / (1.0f - kInner);
        };
        if (dx > 0.0f && dy > 0.0f)
        {
            const float radius = std::hypot(std::abs(ex) / dx, std::abs(ey) / dy);
            if (radius < 1.0f)
            {
                const float scale = band(radius);
                px *= scale;
                py *= scale;
            }
        }
        else if (dx > 0.0f && std::abs(ex) < dx)
            px *= band(std::abs(ex) / dx);
        else if (dy > 0.0f && std::abs(ey) < dy)
            py *= band(std::abs(ey) / dy);
    }
    // Free-wiggle zone (the extra X/Y zone): judged per axis AFTER the existing
    // deadzone above, which is left exactly as it was. Inside it that axis
    // outputs nothing (PID, feedforward and the fractional carry are all cleared).
    // It is deliberately not bypassed by the auto-fire skipDeadzone rule.
    const bool holdX = !config_.maskX && config_.hardDeadzoneX > 0.0f &&
                       std::abs(ex) <= config_.hardDeadzoneX;
    const bool holdY = !config_.maskY && config_.hardDeadzoneY > 0.0f &&
                       std::abs(ey) <= config_.hardDeadzoneY;
    if (holdX) px = 0.0f;
    if (holdY) py = 0.0f;
    result.afterDeadzone = { px, py };

    // C: low-pass the velocity feedforward so raising FF stops injecting the
    // tracker's per-frame velocity noise straight into the output (f-shake).
    constexpr float kFeedforwardCutoffHz = 15.0f;
    const float ffAlpha = dt / (dt + 1.0f / (2.0f * 3.14159265f * kFeedforwardCutoffHz));
    const float rawFx = !config_.maskX && std::isfinite(trackedFeedforward.x)
        ? std::max(0.0f, config_.feedforwardX) * static_cast<float>(trackedFeedforward.x) : 0.0f;
    const float rawFy = !config_.maskY && std::isfinite(trackedFeedforward.y)
        ? std::max(0.0f, config_.feedforwardY) * static_cast<float>(trackedFeedforward.y) : 0.0f;
    ffFilterX_ += ffAlpha * (rawFx - ffFilterX_);
    ffFilterY_ += ffAlpha * (rawFy - ffFilterY_);
    // Inside the free-wiggle zone the feedforward is cleared too (and its filter
    // restarts from zero) so it ramps in smoothly on leaving instead of kicking.
    if (holdX) ffFilterX_ = carryX_ = 0.0f;
    if (holdY) ffFilterY_ = carryY_ = 0.0f;
    const float fx = ffFilterX_;
    const float fy = ffFilterY_;
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
    if (x) { integralX_ = previousX_ = carryX_ = dFilterX_ = ffFilterX_ = 0.0f; seedResetX_ = true; }
    if (y) { integralY_ = previousY_ = carryY_ = dFilterY_ = ffFilterY_ = 0.0f; seedResetY_ = true; }
}

void RecoveredPid::reset()
{
    resetIntegral();
    previousX_ = previousY_ = 0.0f;
    carryX_ = carryY_ = 0.0f;
    dFilterX_ = dFilterY_ = 0.0f;
    ffFilterX_ = ffFilterY_ = 0.0f;
    configured_ = false;
    skipOutputOnce_ = false;
    seedResetX_ = seedResetY_ = false;
}

} // namespace control
