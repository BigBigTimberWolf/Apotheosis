#pragma once

#include "types.h"
#include <algorithm>
#include <cmath>

namespace control {

class DynamicFov
{
public:
    void configure(Vec2 diameter, bool enabled, int targetDiameter, int shrinkMs, int expandMs)
    {
        const Vec2 base{std::max(0.0, diameter.x) * 0.5,
                        std::max(0.0, diameter.y) * 0.5};
        if (base.x != base_.x || base.y != base_.y || enabled != enabled_)
            radii_ = base;
        base_ = base;
        enabled_ = enabled;
        targetDiameter_ = std::clamp(targetDiameter, 1, 4096);
        shrinkMs_ = std::clamp(shrinkMs, 0, 2000);
        expandMs_ = std::clamp(expandMs, 0, 2000);
        if (!enabled_) reset();
    }

    void reset() { radii_ = base_; }
    Vec2 radii() const { return radii_; }
    void release(double dt)
    {
        if (!enabled_) { reset(); return; }
        if (!(dt > 0.0) || !std::isfinite(dt)) return;
        const double alpha = blend(dt, expandMs_);
        radii_.x += alpha * (base_.x - radii_.x);
        radii_.y += alpha * (base_.y - radii_.y);
    }

    bool contains(const Box& box, Vec2 cross, bool alreadyLocked) const
    {
        if (base_.x <= 0.0 || base_.y <= 0.0) return true;
        // Shrinking restricts competitors, not the established lock. The
        // established target still has to intersect the original FOV.
        const Vec2 r = alreadyLocked ? base_ : radii_;
        const double dx = std::max({box.x - cross.x, 0.0, cross.x - box.x - box.w});
        const double dy = std::max({box.y - cross.y, 0.0, cross.y - box.y - box.h});
        return std::hypot(dx / std::max(0.5, r.x), dy / std::max(0.5, r.y)) <= 1.0;
    }

    void follow(const Box& target, Vec2 cross, double dt)
    {
        if (!enabled_) { reset(); return; }
        if (!(dt > 0.0) || !std::isfinite(dt)) return;
        const double alpha = blend(dt, shrinkMs_);
        const double expandAlpha = blend(dt, expandMs_);
        auto update = [&](double current, double base, double distance) {
            const double settled = std::min(base, targetDiameter_ * 0.5);
            // Contract with tracking progress, not just elapsed lock time.
            // Leave room for the target while the crosshair is still catching
            // up. The selected-ID bypass in contains() also prevents clipping
            // during the short expansion response after sudden target motion.
            const double trackingRoom = std::min(base, settled + std::abs(distance) / 0.75);
            const double wanted = std::max(settled, trackingRoom);
            const double blend = wanted > current ? expandAlpha : alpha;
            return current + blend * (wanted - current);
        };
        radii_ = {update(radii_.x, base_.x, target.centerX() - cross.x),
                  update(radii_.y, base_.y, target.centerY() - cross.y)};
    }

private:
    Vec2 base_{}, radii_{};
    bool enabled_ = false;
    static double blend(double dt, int ms) {
        return ms == 0 ? 1.0 : -std::expm1(-2.995732273554 * std::min(dt, 0.25) * 1000.0 / ms);
    }
    int targetDiameter_ = 40;
    int shrinkMs_ = 200;
    int expandMs_ = 120;
};

} // namespace control
