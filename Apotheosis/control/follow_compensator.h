#pragma once

#include "types.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace control {

struct FollowMotion
{
    Vec2 position{}; // Unrounded observed box center, on this captured image.
    Vec2 feedback{}; // Existing capture-aligned, weighted successful mouse events.
    bool valid = false;
};

// Integral-like outer correction learns from ORIGINAL point error. Crossing
// that point never clears the lead. A separate empirical motion estimate gates
// reversal/stop handling and reuse of a previously settled correction.
class FollowCompensator
{
public:
    // Append states so existing recorded state numbers keep their meaning.
    enum State { Learning, Checking, Reversed, Stopped, Preset, Remembered, Uncertain, Burst };

    Vec2 update(Vec2 error, Vec2 frameSize, Vec2 strength,
                int64_t observationUs, double controlDt, FollowMotion motion = {})
    {
        if (!finite(error) || !finite(frameSize) || frameSize.x <= 0.0 || frameSize.y <= 0.0 ||
            !std::isfinite(controlDt) || controlDt <= 0.0)
        {
            reset();
            return {};
        }
        strength = {clean(strength.x), clean(strength.y)};
        if (strength.x == 0.0) x_ = {};
        if (strength.y == 0.0) y_ = {};
        const bool stamped = observationUs > 0;
        double imageDt = controlDt;
        const bool newImage = !ready_ || !stamped || observationUs > previousUs_;
        if (ready_ && stamped && previousUs_ > 0 && newImage)
            imageDt = double(observationUs - previousUs_) * 1e-6;
        if (!ready_ || stamped != (previousUs_ > 0) || imageDt > 0.2)
        {
            reset();
            ready_ = true;
            previousUs_ = observationUs;
            seed(x_, error.x, strength.x);
            seed(y_, error.y, strength.y);
            previousPosition_ = motion.position;
            positionReady_ = motion.valid && finite(motion.position);
            return {};
        }
        if (newImage)
        {
            // Do not integrate a stale gap as a long period of known error.
            const double dt = std::clamp(imageDt, 0.000001, 0.05);
            const bool valid = motion.valid && finite(motion.position) && finite(motion.feedback);
            // Same empirical 0.91 event scale as the original FF estimator,
            // but actual capture dt and NO FF low-speed clipping. This is not
            // a calibrated world-velocity measurement.
            const Vec2 velocity = valid && positionReady_
                ? (motion.position - previousPosition_ + motion.feedback * 0.91) / imageDt
                : Vec2{};
            learn(x_, error.x, frameSize.x, strength.x, dt, velocity.x, valid && positionReady_);
            learn(y_, error.y, frameSize.y, strength.y, dt, velocity.y, valid && positionReady_);
            previousPosition_ = motion.position;
            positionReady_ = valid;
            previousUs_ = observationUs;
        }
        // Smooth buildup at the control rate, including duplicate images.
        // Only a confirmed motion event clears the old applied value.
        auto apply = [&](Axis& axis) {
            const double tau = axis.presetSmoothLeft > 0.0 ? 0.015
                : 0.06 - 0.035 * response(axis.mean, noiseBand(axis));
            const double alpha = -std::expm1(-std::min(controlDt, 0.05) / tau);
            axis.applied += alpha * (axis.offset - axis.applied);
            axis.presetSmoothLeft = std::max(0.0, axis.presetSmoothLeft - controlDt);
        };
        apply(x_);
        apply(y_);
        return {x_.applied, y_.applied};
    }

    Vec2 motionEstimate() const { return {x_.motionMean, y_.motionMean}; }
    Vec2 presetAmount() const { return {x_.presetAmount, y_.presetAmount}; }
    int stateX() const { return x_.state; }
    int stateY() const { return y_.state; }

    void setSaturation(Vec2 direction)
    {
        x_.saturated = direction.x;
        y_.saturated = direction.y;
    }

    void reset()
    {
        x_ = {}; y_ = {};
        ready_ = false;
        previousUs_ = 0;
        previousPosition_ = {};
        positionReady_ = false;
    }

private:
    struct Axis
    {
        double mean = 0.0, variation = 0.0, sameDirectionSec = 0.0;
        double offset = 0.0, applied = 0.0, saturated = 0.0;
        double burstPendingLeft = 0.0, burstTimeLeft = 0.0, burstBudget = 0.0;
        double burstReferenceLead = 0.0, burstCooldown = 0.0;
        double accelerationHold = 0.0;
        int accelerationSamples = 0;
        int direction = 0;
        bool ready = false;
        bool directionReady = false;
        double motionMean = 0.0, motionNoise = 0.0, speedReference = 0.0;
        double motionHold = 0.0, movingTime = 0.0, quietTime = 0.0;
        int motionDirection = 0, pendingDirection = 0, motionSamples = 0;
        bool motionReady = false;
        double memoryLead = 0.0, memorySpeed = 0.0, memoryAge = 1.0;
        double settledTime = 0.0, lastApplied = 0.0, lastStrength = 0.0;
        double presetAmount = 0.0, presetSmoothLeft = 0.0, eventLeft = 0.0;
        int state = Learning;
    };
    static constexpr double burstDuration = 0.24;
    static constexpr double burstDecay = 0.08;
    static constexpr double burstExtraGain = 3.0;

    static void cancelBurst(Axis& axis)
    {
        axis.burstPendingLeft = axis.burstTimeLeft = axis.burstBudget = 0.0;
        axis.burstReferenceLead = 0.0;
        axis.accelerationHold = 0.0;
        axis.accelerationSamples = 0;
    }

    static void armBurst(Axis& axis, double previousLead = 0.0)
    {
        cancelBurst(axis);
        // Wait briefly for lag to point along the confirmed motion. In a turn,
        // motion may change before the original point crosses the crosshair.
        axis.burstPendingLeft = 0.25;
        axis.burstReferenceLead = previousLead;
        axis.burstCooldown = 0.5;
    }
    static bool finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
    static double clean(double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 50.0) : 0.0; }
    static int sign(double v) { return v > 0.0 ? 1 : v < 0.0 ? -1 : 0; }
    static double noiseBand(const Axis& axis)
    {
        return std::clamp(1.5 * axis.variation, 1.0, 4.0);
    }
    static double response(double error, double band)
    {
        return std::clamp((std::abs(error) - band) / (4.0 * band), 0.0, 1.0);
    }
    static void seed(Axis& axis, double error, double strength)
    {
        if (strength == 0.0) return;
        axis.mean = error;
        axis.direction = sign(error);
        axis.lastStrength = strength;
        axis.ready = true;
    }

    static void clearCorrection(Axis& axis, double error)
    {
        axis.offset = axis.applied = 0.0;
        axis.mean = error;
        axis.variation = axis.sameDirectionSec = 0.0;
        axis.direction = sign(error);
        axis.directionReady = true;
        cancelBurst(axis);
        axis.presetAmount = axis.presetSmoothLeft = 0.0;
    }

    static void observeMotion(Axis& a, double error, double extent, double dt,
                              double velocity, bool valid)
    {
        a.eventLeft = std::max(0.0, a.eventLeft - dt);
        a.burstCooldown = std::max(0.0, a.burstCooldown - dt);
        a.memoryAge += dt;
        if (a.eventLeft == 0.0 || a.state == Burst) {
            a.state = a.memoryAge < 0.5 ? Remembered : Learning;
            a.presetAmount = 0.0;
        }
        if (!valid || !std::isfinite(velocity)) {
            // Missing motion evidence is NOT a stationary target.
            a.motionReady = false;
            a.motionHold = a.quietTime = a.settledTime = a.movingTime = 0.0;
            a.memoryAge = 1.0;
            a.pendingDirection = a.motionSamples = 0;
            cancelBurst(a);
            a.state = Uncertain;
            return;
        }
        const double alpha = -std::expm1(-dt / 0.04);
        if (!a.motionReady) {
            a.motionMean = velocity;
            a.motionNoise = 0.0;
            a.motionReady = true;
        } else {
            a.motionMean += alpha * (velocity - a.motionMean);
            a.motionNoise += -std::expm1(-dt / 0.12) *
                (std::abs(velocity - a.motionMean) - a.motionNoise);
        }
        const double speed = std::abs(a.motionMean);
        const double uncertainty = std::max(0.5,
            0.5 * a.motionNoise * std::sqrt(std::min(dt / 0.12, 1.0)));
        const int observed = speed > uncertainty ? sign(a.motionMean) : 0;
        if (observed != 0 && observed != a.motionDirection &&
            sign(velocity) == observed && std::abs(velocity) > uncertainty) {
            if (a.pendingDirection != observed) {
                a.pendingDirection = observed;
                a.motionHold = 0.0;
                a.motionSamples = 0;
            }
            a.motionHold += dt;
            ++a.motionSamples;
            if (a.eventLeft == 0.0) a.state = Checking;
            // About 0.6px of estimated travel, with bounded confirmation time.
            // Slow targets get more observations; one fast outlier cannot turn.
            const double required = std::clamp(0.6 / std::max(speed, 1.0), 0.025, 0.18);
            if (a.motionSamples >= 3 && a.motionHold >= required) {
                const int oldDirection = a.motionDirection;
                const double oldLead = std::max(std::abs(a.offset), std::abs(a.applied));
                const bool reversal = oldDirection != 0 && a.movingTime >= 0.12;
                const bool reuse = reversal && a.memoryAge < 0.5 &&
                    sign(a.memoryLead) == oldDirection && a.memorySpeed > 0.5 &&
                    sign(velocity) == observed && std::abs(velocity) >= 0.75 * speed &&
                    a.saturated == 0.0;
                const double ratio = a.memorySpeed > 0.5 ? speed / a.memorySpeed : 0.0;
                if (reversal) {
                    clearCorrection(a, error);
                    a.state = Reversed;
                    a.eventLeft = 0.25;
                    if (reuse && ratio >= 0.25 && ratio <= 2.0) {
                        const double initial = std::min(extent * 0.5,
                            std::abs(a.memoryLead) * 0.8 * std::min(ratio, 1.25));
                        a.offset = observed * initial;
                        a.presetAmount = a.offset;
                        a.presetSmoothLeft = 0.10;
                        a.burstCooldown = 0.5;
                        a.state = Preset;
                    } else armBurst(a, oldLead);
                    // Never reuse the same memory for repeated quick turns.
                    a.memoryAge = 1.0;
                    a.settledTime = 0.0;
                } else if (oldDirection == 0) armBurst(a);
                else cancelBurst(a);
                a.motionDirection = observed;
                a.speedReference = speed;
                a.movingTime = 0.0;
                a.motionHold = 0.0;
                a.motionSamples = a.pendingDirection = 0;
            }
        } else {
            a.motionHold = 0.0;
            a.motionSamples = a.pendingDirection = 0;
            if (observed == a.motionDirection && observed != 0) {
                a.movingTime += dt;
                // Compare against the slower reference BEFORE updating it.
                // A stable speed cannot repeatedly re-arm the extra gain.
                const bool accelerating = a.movingTime >= 0.12 && a.burstCooldown == 0.0 &&
                    a.burstPendingLeft == 0.0 && a.burstTimeLeft == 0.0 &&
                    speed > 1.3 * a.speedReference &&
                    speed - a.speedReference > std::max(2.0, 2.0 * uncertainty) &&
                    sign(velocity) == observed && std::abs(velocity) >= 0.85 * speed;
                a.accelerationHold = accelerating ? a.accelerationHold + dt : 0.0;
                a.accelerationSamples = accelerating ? a.accelerationSamples + 1 : 0;
                if (a.accelerationSamples >= 3 && a.accelerationHold >= 0.03) {
                    armBurst(a);
                    a.memoryAge = 1.0;
                    a.settledTime = 0.0;
                }
                a.speedReference += -std::expm1(-dt / 0.2) * (speed - a.speedReference);
            }
        }
        if (observed == 0 || observed != a.motionDirection || a.pendingDirection != 0) {
            a.accelerationHold = 0.0;
            a.accelerationSamples = 0;
        }

        // Require sustained near-zero RAW motion as well as a settled mean.
        // Slow motion with quantized, intermittent zero samples gets a longer
        // grace period (up to 1.5s); a reduction to a nonzero speed is not a stop.
        const bool quiet = a.motionDirection != 0 && a.movingTime >= 0.25 &&
            std::abs(velocity) < 0.5 && speed < std::max(0.5, 0.05 * a.speedReference);
        a.quietTime = quiet ? a.quietTime + dt : 0.0;
        const double stopWait = std::clamp(1.5 / std::max(a.speedReference, 1.0), 0.12, 1.5);
        if (a.quietTime >= stopWait) {
            clearCorrection(a, error);
            a.motionDirection = a.pendingDirection = a.motionSamples = 0;
            a.movingTime = a.quietTime = a.motionHold = a.settledTime = 0.0;
            a.memoryAge = 1.0;
            a.state = Stopped;
            a.eventLeft = 0.25;
        }

        const double lead = std::abs(a.applied);
        const double leadRate = std::abs(a.applied - a.lastApplied) / dt;
        const bool settled = a.motionDirection != 0 && observed == a.motionDirection &&
            a.movingTime >= 0.25 && a.pendingDirection == 0 && a.saturated == 0.0 &&
            sign(a.applied) == a.motionDirection && lead >= 1.0 &&
            leadRate <= std::max(2.0, 0.25 * lead) &&
            std::abs(a.offset - a.applied) <= std::max(1.0, 0.1 * lead) &&
            std::abs(speed - a.speedReference) <= std::max(1.0, 0.25 * a.speedReference) &&
            speed > 2.0 * uncertainty;
        a.settledTime = settled ? a.settledTime + dt : 0.0;
        if (a.settledTime >= 0.2) {
            a.memoryLead = a.applied;
            a.memorySpeed = a.speedReference;
            a.memoryAge = 0.0;
            if (a.eventLeft == 0.0) a.state = Remembered;
        }
        a.lastApplied = a.applied;
    }

    static void learn(Axis& axis, double error, double frameExtent, double strength, double dt,
                      double velocity, bool motionValid)
    {
        if (strength == 0.0) return;
        if (!axis.ready) { seed(axis, error, strength); return; }
        if (strength != axis.lastStrength) {
            axis.memoryAge = 1.0;
            axis.settledTime = 0.0;
            cancelBurst(axis);
            axis.burstCooldown = 0.5;
            axis.presetAmount = axis.presetSmoothLeft = axis.eventLeft = 0.0;
            axis.lastStrength = strength;
        }
        observeMotion(axis, error, frameExtent, dt, velocity, motionValid);
        // Error sign and magnitude NEVER trigger a correction reset.
        const double band = noiseBand(axis);
        const double previousMean = axis.mean;
        // Respond sooner to a substantial lag without increasing the user's
        // integral gain. Near the noise floor, retain the slower filter.
        const double tau = 0.08 - 0.055 * response(error, band);
        const double alpha = -std::expm1(-dt / tau);
        axis.mean += alpha * (error - axis.mean);
        axis.variation += alpha * (std::abs(error - axis.mean) - axis.variation);
        const int direction = sign(axis.mean);
        // Judge persistence from the mean. Rounded 0/1px observations must not
        // restart the timer on every zero sample when their mean stays positive.
        axis.sameDirectionSec = direction != 0 && direction == axis.direction
            ? std::min(0.5, axis.sameDirectionSec + dt) : 0.0;
        if (direction != axis.direction) axis.directionReady = false;
        axis.direction = direction;

        // Bound extrapolation by the observation field, never by a target box.
        // A narrow target can require a lead of many target widths.
        const double limit = frameExtent * 0.5;
        axis.offset = std::clamp(axis.offset, -limit, limit);
        if (axis.sameDirectionSec >= 0.03) axis.directionReady = true;
        const double closingRate = std::max(0.0, -direction * (axis.mean - previousMean) / dt);
        // The burst changes accumulation speed, never seeds a nonzero lead.
        // Only use fresh motion evidence, with raw and filtered lag pointing
        // along that motion. Crossing the aimpoint only disables EXTRA gain;
        // it does not clear or restrict ordinary integration.
        const bool canBoost = motionValid && axis.motionReady && axis.pendingDirection == 0 &&
            axis.motionDirection != 0 && sign(velocity) == axis.motionDirection &&
            std::abs(velocity) > 0.5 && sign(axis.motionMean) == axis.motionDirection &&
            std::abs(velocity) >= 0.5 * std::abs(axis.motionMean) &&
            direction == axis.motionDirection && sign(error) == direction &&
            std::abs(error) > band && std::abs(axis.mean) > band &&
            axis.directionReady && axis.saturated == 0.0;
        if (axis.burstPendingLeft > 0.0 && canBoost) {
            axis.burstTimeLeft = burstDuration;
            // Extra pixels, not a limit on the ordinary learned offset.
            axis.burstBudget = std::min(frameExtent * 0.125,
                std::max(std::abs(axis.mean), 0.5 * axis.burstReferenceLead));
            axis.burstPendingLeft = 0.0;
        }
        axis.burstPendingLeft = std::max(0.0, axis.burstPendingLeft - dt);
        const double activeDt = std::min(dt, axis.burstTimeLeft);
        const double elapsed = burstDuration - axis.burstTimeLeft;
        // Integrate the exponential over this image interval. Its average
        // avoids a different burst strength at different capture rates.
        const double extraGain = burstExtraGain * std::exp(-elapsed / burstDecay) *
            burstDecay * -std::expm1(-activeDt / burstDecay) / dt;
        axis.burstTimeLeft = std::max(0.0, axis.burstTimeLeft - dt);
        if (!axis.directionReady || axis.saturated * axis.mean > 0.0) return;
        // Reduce buildup while the original point is already being caught.
        // A growing lag must not be penalized as if it were converging.
        const double signal = std::max(0.75, std::abs(axis.mean));
        const double confidence = signal / (signal + axis.variation + 0.15 * closingRate);
        // Integrate the full filtered error, with no aimpoint deadband. A zero
        // or small opposite raw sample does not abruptly stop the existing
        // trend; its filtered mean decays/reverses naturally. No error crossing
        // (large or small) is treated as a target-motion event.
        const double persistentError = axis.mean;
        const double increment = 0.25 * strength * confidence * persistentError * dt;
        const double ordinaryNext = std::clamp(axis.offset + increment, -limit, limit);
        // Peak total gain is 4x, falling exponentially toward 1x. Keep the
        // existing noise/closing attenuation and a finite per-event budget.
        const double extraMagnitude = canBoost ? std::min(axis.burstBudget,
            std::abs(increment) * extraGain *
            response(axis.mean, noiseBand(axis)) * confidence) : 0.0;
        axis.offset = std::clamp(ordinaryNext + std::copysign(extraMagnitude, increment),
                                 -limit, limit);
        const double appliedExtra = std::abs(axis.offset - ordinaryNext);
        axis.burstBudget = std::max(0.0, axis.burstBudget - appliedExtra);
        if (appliedExtra > 0.0) axis.state = Burst;
    }

    Axis x_{}, y_{};
    int64_t previousUs_ = 0;
    bool ready_ = false;
    Vec2 previousPosition_{};
    bool positionReady_ = false;
};

} // namespace control
