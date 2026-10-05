#pragma once

#include "types.h"

#include <algorithm>
#include <cmath>

namespace control {

// Companion of the Smith Predictor that sits in the PID error.
//
// Smith removes every pixel of motion that was already sent but has not shown up
// in the image yet, so the PID sees the estimated current error instead of the
// delayed one. That is exact for a still target. A target that is itself moving
// covers about `velocity x delay` during the same delay, though; removing all of
// the in-flight motion then leaves the crosshair trailing by that much and
// cancels the visible extrapolation.
//
// This hands back only the part of the in-flight motion that the target's own
// movement explains. Per axis:
//     lead = min(|slow in-flight|, |in-flight now|, |filtered velocity| x delay)
// signed like the in-flight motion, and only while the filtered velocity, the
// slowly filtered in-flight motion and the in-flight motion now all point the
// same way. A still target, a noisy velocity reading, an acquisition burst
// (in-flight motion that has not persisted) and a target moving away from the
// motion we send all get no lead, so Smith keeps its full correction there.
//
// The controller adds the lead to the control anchor, so it is also the
// extrapolated aim point the preview draws. Nothing here is user-tunable.
class SmithLead
{
public:
    // velocity: tracked target velocity (px/s, self motion removed).
    // pending: motion sent but not yet visible in the image (px).
    // delaySec: calibrated send-to-image lag; <= 0 means Smith is not active.
    Vec2 update(Vec2 velocity, Vec2 pending, double delaySec, double dtSec)
    {
        if (!(delaySec > 0.0) || !(dtSec > 0.0) || !std::isfinite(delaySec) ||
            !std::isfinite(dtSec) || !finite(velocity) || !finite(pending)) {
            reset();
            return {};
        }
        if (!ready_) {
            // A new target (or a restart) must not inherit earlier motion: the
            // velocity starts at the current reading, the in-flight history at 0.
            velocity_ = velocity;
            pending_ = {};
            ready_ = true;
        }
        const double velocityAlpha = -std::expm1(-dtSec / kVelocityTauSec);
        const double pendingAlpha = -std::expm1(-dtSec / kPendingTauSec);
        velocity_ += (velocity - velocity_) * velocityAlpha;
        pending_ += (pending - pending_) * pendingAlpha;
        return {axis(velocity_.x, pending_.x, pending.x, delaySec),
                axis(velocity_.y, pending_.y, pending.y, delaySec)};
    }

    void reset() {
        velocity_ = {};
        pending_ = {};
        ready_ = false;
    }

private:
    // Velocity smoothing is short (the tracker already filters it); the in-flight
    // history is longer so a one-off burst is not mistaken for target motion.
    static constexpr double kVelocityTauSec = 0.05;
    static constexpr double kPendingTauSec = 0.12;

    static bool finite(const Vec2& v) { return std::isfinite(v.x) && std::isfinite(v.y); }

    static double axis(double velocity, double slow, double now, double delaySec) {
        if (slow * now <= 0.0 || velocity * slow <= 0.0) return 0.0;
        const double magnitude = std::min({std::abs(slow), std::abs(now),
                                           std::abs(velocity) * delaySec});
        return slow > 0.0 ? magnitude : -magnitude;
    }

    Vec2 velocity_{}, pending_{};
    bool ready_ = false;
};

} // namespace control
