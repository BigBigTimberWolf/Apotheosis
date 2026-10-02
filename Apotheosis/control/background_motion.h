#pragma once
#include "types.h"
#include <algorithm>
#include <cmath>

namespace control {
// Coordinates and uncertainty are in original inference-image pixels.
// A new chain starts whenever a background estimate is unavailable. Cumulative
// displacement allows the latest-only control worker to skip inference frames.
struct BackgroundMotion {
    Vec2 cumulative{};
    double variance = 0.0;
    uint64_t chain = 0;
    int64_t timeUs = 0;
    bool valid = false;
};

class TargetDirectionObserver {
public:
    Vec2 update(Vec2 center, Vec2 size, const BackgroundMotion& bg, int64_t timeUs) {
        if (!bg.valid || bg.timeUs != timeUs || timeUs <= 0 ||
            !finite(center) || !finite(bg.cumulative) || !std::isfinite(bg.variance)) {
            reset(); return {};
        }
        if (ready_ && timeUs == previous_.timeUs) return {};
        const bool continuous = ready_ && previous_.chain == bg.chain &&
            timeUs > previous_.timeUs && timeUs - previous_.timeUs <= 200000 &&
            bg.variance >= previous_.variance;
        Vec2 reversed;
        if (continuous) {
            const auto movement = (center - center_) - (bg.cumulative - previous_.cumulative);
            const double uncertainty = std::sqrt(bg.variance - previous_.variance);
            reversed.x = observe(x_, movement.x, std::max(0.75, size.x * .01) + 2 * uncertainty);
            reversed.y = observe(y_, movement.y, std::max(0.75, size.y * .01) + 2 * uncertainty);
        } else { x_ = {}; y_ = {}; }
        center_ = center; previous_ = bg; ready_ = true;
        return reversed;
    }
    void reset() { *this = {}; }
private:
    struct Axis { int direction = 0, pending = 0, samples = 0; double travel = 0; };
    static bool finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
    static bool observe(Axis& a, double movement, double threshold) {
        // Noise/stop is not reversal. Require two agreeing intervals and enough
        // displacement to exceed image and detection uncertainty.
        const int sign = movement > .1 ? 1 : movement < -.1 ? -1 : 0;
        if (!sign || sign == a.direction) { a.pending = a.samples = 0; a.travel = 0; return false; }
        if (a.pending != sign) { a.pending = sign; a.samples = 0; a.travel = 0; }
        a.travel += std::abs(movement);
        ++a.samples;
        if (a.samples < 2 || a.travel < threshold) return false;
        const bool reversed = a.direction != 0;
        a.direction = sign; a.pending = a.samples = 0; a.travel = 0;
        return reversed;
    }
    Axis x_, y_;
    Vec2 center_;
    BackgroundMotion previous_;
    bool ready_ = false;
};
}
