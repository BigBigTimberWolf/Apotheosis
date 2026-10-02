#pragma once

#include "types.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace control {

// Learns lead from original aimpoint error. A sustained direction flip only
// restarts persistence and unwinds the opposite error gradually; crossing the
// aimpoint preserves the learned and applied lead.
class FollowCompensator
{
public:
    // Preserve old recording values; new recordings use explicit error states.
    enum State { Learning, Checking, Reversed, Stopped, Preset, Remembered, Uncertain, Burst,
                 ErrorLearning, ErrorHolding, ErrorUnwinding, ErrorDisabled };

    Vec2 update(Vec2 error, Vec2 frameSize, Vec2 strength,
                int64_t observationUs, double controlDt)
    {
        if (!finite(error) || !finite(frameSize) || frameSize.x <= 0.0 || frameSize.y <= 0.0 ||
            !std::isfinite(controlDt) || controlDt <= 0.0) {
            reset();
            return {};
        }
        strength = {clean(strength.x), clean(strength.y)};
        if (strength.x == 0.0) x_ = {};
        if (strength.y == 0.0) y_ = {};
        const bool stamped = observationUs > 0;
        const bool newImage = !ready_ || !stamped || observationUs > previousUs_;
        double imageDt = controlDt;
        if (ready_ && stamped && previousUs_ > 0 && newImage)
            imageDt = double(observationUs - previousUs_) * 1e-6;
        if (!ready_ || stamped != (previousUs_ > 0) || imageDt > 0.2 ||
            (stamped && observationUs < previousUs_)) {
            reset();
            ready_ = true;
            previousUs_ = observationUs;
            previousError_ = error;
            seed(x_, error.x, strength.x);
            seed(y_, error.y, strength.y);
            return {};
        }
        if (newImage) {
            // Diagnostic error trend for the compensation overlay, not FF.
            // Units are image pixels/second, not target/world velocity.
            const Vec2 rawRate = (error - previousError_) / imageDt;
            const double rateAlpha = -std::expm1(-imageDt / 0.04);
            errorRate_ += (rawRate - errorRate_) * rateAlpha;
            previousError_ = error;
            const double dt = std::clamp(imageDt, 0.000001, 0.05);
            learn(x_,error.x,frameSize.x,strength.x,dt);
            learn(y_,error.y,frameSize.y,strength.y,dt);
            previousUs_ = observationUs;
        }
        auto apply = [&](Axis& axis) {
            const double tau = 0.06 - 0.035 * response(axis.mean, noiseBand(axis));
            const double alpha = -std::expm1(-std::min(controlDt, 0.05) / tau);
            axis.applied += alpha * (axis.offset - axis.applied);
        };
        apply(x_);
        apply(y_);
        return {x_.applied, y_.applied};
    }

    Vec2 errorRate() const { return errorRate_; }
    int stateX() const { return x_.state; }
    int stateY() const { return y_.state; }
    void setSaturation(Vec2 direction) { x_.saturated = direction.x; y_.saturated = direction.y; }
    void reset() {
        x_ = {}; y_ = {};
        ready_ = false;
        previousUs_ = 0;
        previousError_ = errorRate_ = {};
    }

private:
    struct Axis {
        double mean = 0.0, variation = 0.0, sameDirectionSec = 0.0;
        double offset = 0.0, applied = 0.0, saturated = 0.0;
        int direction = 0, rawDirection = 0;
        bool ready = false, directionReady = false;
        int state = ErrorDisabled;
    };
    static bool finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
    static double clean(double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 50.0) : 0.0; }
    static int sign(double v) { return v > 0.0 ? 1 : v < 0.0 ? -1 : 0; }
    static double noiseBand(const Axis& a) { return std::clamp(1.5 * a.variation, 1.0, 4.0); }
    static double response(double error, double band) {
        return std::clamp((std::abs(error) - band) / (4.0 * band), 0.0, 1.0);
    }
    static void seed(Axis& a, double error, double strength) {
        if (strength == 0.0) return;
        a.mean = error;
        a.direction = sign(error);
        a.rawDirection = a.direction;
        a.ready = true;
        a.state = ErrorHolding;
    }
    static void learn(Axis& a, double error, double extent, double strength, double dt) {
        if (strength == 0.0) return;
        if (!a.ready) { seed(a, error, strength); return; }
        const int rawDirection = sign(error);
        if (rawDirection != 0 && a.rawDirection != 0 && rawDirection != a.rawDirection) {
            // Restart persistence, but preserve learned AND applied lead.
            // Alternating noise must not count as sustained directional error.
            a.sameDirectionSec = 0;
            a.directionReady = false;
        }
        if (rawDirection != 0) a.rawDirection = rawDirection;
        const double previousMean = a.mean;
        const double tau = 0.08 - 0.055 * response(error, noiseBand(a));
        const double alpha = -std::expm1(-dt / tau);
        a.mean += alpha * (error - a.mean);
        a.variation += alpha * (std::abs(error - a.mean) - a.variation);
        const int direction = sign(a.mean);
        a.sameDirectionSec = direction != 0 && direction == a.direction
            ? std::min(0.5, a.sameDirectionSec + dt) : 0.0;
        if (direction != a.direction) a.directionReady = false;
        a.direction = direction;
        if (a.sameDirectionSec >= 0.03) a.directionReady = true;
        const double limit = extent * 0.5;
        a.offset = std::clamp(a.offset, -limit, limit);
        a.state = ErrorHolding;
        if (!a.directionReady || a.saturated * a.mean > 0.0) return;
        // Keep the ordinary integral, catch-up attenuation and anti-windup.
        // Opposite error can unwind gradually; it is not evidence of a turn.
        const double closingRate = std::max(0.0, -direction * (a.mean - previousMean) / dt);
        const double signal = std::max(0.75, std::abs(a.mean));
        const double confidence = signal / (signal + a.variation + 0.15 * closingRate);
        const double next = std::clamp(a.offset + 0.25 * strength * confidence * a.mean * dt,
                                       -limit, limit);
        if (std::abs(next - a.offset) > 1e-9)
            a.state = (next - a.offset) * a.offset < 0.0 ? ErrorUnwinding : ErrorLearning;
        a.offset = next;
    }

    Axis x_{}, y_{};
    int64_t previousUs_ = 0;
    bool ready_ = false;
    Vec2 previousError_{}, errorRate_{};
};

} // namespace control
