#pragma once

#include "config/config.h"
#include "control/types.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace boss {

struct TriggerTarget {
    bool valid = false;
    int trackId = -1;
    int classId = -1;
    control::Box box{};
    control::Vec2 point{};
    double halfWidth = 0.0;
    double halfHeight = 0.0;

    bool contains(const control::Vec2& cross) const {
        return valid && std::abs(cross.x - point.x) < halfWidth &&
            std::abs(cross.y - point.y) < halfHeight;
    }

    bool containsExpanded(const control::Vec2& cross, int expandPercent) const {
        const double factor = 1.0 + std::clamp(expandPercent, 0, 300) / 100.0;
        return valid && std::abs(cross.x - point.x) <= halfWidth * factor &&
            std::abs(cross.y - point.y) <= halfHeight * factor;
    }
};

// Independent of aim-class filtering and the aim controller's selected track.
// The ordered class list decides priority; within a class the previous box is
// retained while it remains nearby, otherwise the nearest box is selected.
class TriggerTargetSelector {
public:
    TriggerTarget select(const std::vector<control::Candidate>& candidates,
                         const std::vector<TriggerAimClass>& classes,
                         control::Vec2 cross, int fovX, int fovY,
                         double minConfidence, bool detectionFresh) {
        if (!detectionFresh || classes.empty()) return {};
        // A lower-priority class already under the crosshair may fire when a
        // higher-priority class is merely visible elsewhere in the FOV.
        for (bool requireHit : {true, false}) for (const auto& rule : classes) {
            const control::Candidate* best = nullptr;
            double bestDistance = std::numeric_limits<double>::infinity();
            for (const auto& candidate : candidates) {
                if (candidate.classId != rule.class_id || !candidate.box.valid() ||
                    candidate.confidence < minConfidence) continue;
                const auto center = candidate.box.center();
                const double pointX = candidate.box.x + candidate.box.w * rule.x_offset;
                const double pointY = candidate.box.y + candidate.box.h * (1.0 - rule.y_offset);
                const bool hit =
                    std::abs(cross.x - pointX) < candidate.box.w * rule.range_x_percent / 200.0 &&
                    std::abs(cross.y - pointY) < candidate.box.h * rule.range_y_percent / 200.0;
                if (requireHit && !hit) continue;
                // A wide trigger range can extend beyond the aiming FOV. If
                // the judgement point is inside that range, retain the box.
                if (!hit &&
                    (std::abs(center.x - cross.x) > std::max(1, fovX) * 0.5 + candidate.box.w * 0.5 ||
                     std::abs(center.y - cross.y) > std::max(1, fovY) * 0.5 + candidate.box.h * 0.5))
                    continue;
                double distance = (center - cross).norm();
                if (last_.valid && last_.classId == rule.class_id) {
                    if (overlap(last_.box, candidate.box) >= 0.1)
                        distance -= std::max(1.0, candidate.box.diagonal());
                }
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = &candidate;
                }
            }
            if (!best) continue;
            TriggerTarget selected;
            selected.valid = true;
            selected.classId = rule.class_id;
            selected.box = best->box;
            selected.point = { best->box.x + best->box.w * rule.x_offset,
                               best->box.y + best->box.h * (1.0 - rule.y_offset) };
            selected.halfWidth = best->box.w * rule.range_x_percent / 200.0;
            selected.halfHeight = best->box.h * rule.range_y_percent / 200.0;
            const bool same = last_.valid && last_.classId == selected.classId &&
                overlap(last_.box, selected.box) >= 0.1;
            selected.trackId = same ? last_.trackId : nextId_++;
            last_ = selected;
            return selected;
        }
        return {};
    }

    void reset() { last_ = {}; nextId_ = 1; }

private:
    static double overlap(const control::Box& a, const control::Box& b) {
        const double w = std::max(0.0, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
        const double h = std::max(0.0, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
        const double intersection = w * h;
        const double total = a.area() + b.area() - intersection;
        return total > 0.0 ? intersection / total : 0.0;
    }
    TriggerTarget last_{};
    int nextId_ = 1;
};

} // namespace boss
