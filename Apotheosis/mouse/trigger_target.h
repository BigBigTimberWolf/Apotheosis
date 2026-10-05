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

// Which box may fire when several classes are on screen.
//  Strict   - the class list is a real priority: the first class (top of the list) that
//             has a box in view decides. A box under the crosshair fires; a box in view
//             but not under the crosshair yet holds fire for every class below it, so the
//             crosshair keeps moving into the higher-priority zone instead of shooting a
//             lower-priority class that happens to be under it. Only when the higher class
//             is absent (not detected, below the confidence threshold, outside the FOV)
//             may a lower one fire.
//  HitFirst - no real priority (the legacy trigger zone without a class list): any class
//             under the crosshair may fire, the order only picks between them.
enum class TriggerPriority { Strict, HitFirst };

// Independent of aim-class filtering and the aim controller's selected track.
// The ordered class list decides priority; within a class the previous box is
// retained while it remains nearby, otherwise the nearest box is selected.
class TriggerTargetSelector {
public:
    TriggerTarget select(const std::vector<control::Candidate>& candidates,
                         const std::vector<TriggerAimClass>& classes,
                         control::Vec2 cross, int fovX, int fovY,
                         double minConfidence, bool detectionFresh,
                         TriggerPriority priority = TriggerPriority::Strict) {
        if (!detectionFresh || classes.empty()) return {};
        const View view{candidates, cross, fovX, fovY, minConfidence};
        if (priority == TriggerPriority::Strict) {
            for (const auto& rule : classes)
                for (bool requireHit : {true, false})
                    if (const auto* best = nearest(view, rule, requireHit))
                        return choose(*best, rule);
            return {};
        }
        for (bool requireHit : {true, false}) for (const auto& rule : classes)
            if (const auto* best = nearest(view, rule, requireHit))
                return choose(*best, rule);
        return {};
    }

    void reset() { last_ = {}; nextId_ = 1; }

private:
    struct View {
        const std::vector<control::Candidate>& candidates;
        control::Vec2 cross;
        int fovX, fovY;
        double minConfidence;
    };

    // Best box of one class: under the crosshair when `requireHit`, otherwise any box in view.
    const control::Candidate* nearest(const View& view, const TriggerAimClass& rule,
                                      bool requireHit) const {
        const control::Candidate* best = nullptr;
        double bestDistance = std::numeric_limits<double>::infinity();
        for (const auto& candidate : view.candidates) {
            if (candidate.classId != rule.class_id || !candidate.box.valid() ||
                candidate.confidence < view.minConfidence) continue;
            const auto center = candidate.box.center();
            const double pointX = candidate.box.x + candidate.box.w * rule.x_offset;
            const double pointY = candidate.box.y + candidate.box.h * (1.0 - rule.y_offset);
            const bool hit =
                std::abs(view.cross.x - pointX) < candidate.box.w * rule.range_x_percent / 200.0 &&
                std::abs(view.cross.y - pointY) < candidate.box.h * rule.range_y_percent / 200.0;
            if (requireHit && !hit) continue;
            // A wide trigger range can extend beyond the aiming FOV. If
            // the judgement point is inside that range, retain the box.
            if (!hit &&
                (std::abs(center.x - view.cross.x) > std::max(1, view.fovX) * 0.5 + candidate.box.w * 0.5 ||
                 std::abs(center.y - view.cross.y) > std::max(1, view.fovY) * 0.5 + candidate.box.h * 0.5))
                continue;
            double distance = (center - view.cross).norm();
            if (last_.valid && last_.classId == rule.class_id) {
                if (overlap(last_.box, candidate.box) >= 0.1)
                    distance -= std::max(1.0, candidate.box.diagonal());
            }
            if (distance < bestDistance) {
                bestDistance = distance;
                best = &candidate;
            }
        }
        return best;
    }

    TriggerTarget choose(const control::Candidate& best, const TriggerAimClass& rule) {
        TriggerTarget selected;
        selected.valid = true;
        selected.classId = rule.class_id;
        selected.box = best.box;
        selected.point = { best.box.x + best.box.w * rule.x_offset,
                           best.box.y + best.box.h * (1.0 - rule.y_offset) };
        selected.halfWidth = best.box.w * rule.range_x_percent / 200.0;
        selected.halfHeight = best.box.h * rule.range_y_percent / 200.0;
        const bool same = last_.valid && last_.classId == selected.classId &&
            overlap(last_.box, selected.box) >= 0.1;
        selected.trackId = same ? last_.trackId : nextId_++;
        last_ = selected;
        return selected;
    }

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
