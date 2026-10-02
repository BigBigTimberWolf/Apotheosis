#include "mouse/trigger_target.h"

#include <cstdio>
#include <cmath>
#include <vector>

int main() {
    boss::TriggerTargetSelector selector;
    const std::vector<TriggerAimClass> rules = {
        {1, 0.5f, 0.8f, 20, 30},
        {0, 0.5f, 0.5f, 100, 100}
    };
    const std::vector<control::Candidate> candidates = {
        {{100, 100, 100, 160}, 0, 0.95},
        {{120, 90, 40, 40}, 1, 0.95}
    };
    const control::Vec2 cross{140, 101};
    const auto first = selector.select(candidates, rules, cross, 200, 200, 0.5, true);
    if (!first.valid || first.classId != 1 || first.point.x != 140 ||
        std::abs(first.point.y - 98.0) > 0.001 || first.halfWidth != 4 || first.halfHeight != 6 ||
        !first.contains(cross)) {
        std::puts("trigger priority, point, or hit zone failed");
        return 1;
    }
    if (first.contains({146, 98}) ||
        !first.containsExpanded({146, 98}, 50) ||
        first.containsExpanded({160, 98}, 50)) return 8;
    const auto same = selector.select(candidates, rules, cross, 200, 200, 0.5, true);
    if (same.trackId != first.trackId) return 2;
    if (first.contains({first.point.x + first.halfWidth, first.point.y})) return 9; // AM excludes borders
    auto farHead = candidates;
    farHead[1].box.x = 190;
    const auto bodyUnderCross = selector.select(farHead, rules, cross, 200, 200, 0.5, true);
    if (!bodyUnderCross.valid || bodyUnderCross.classId != 0 || !bodyUnderCross.contains(cross)) return 6;
    const auto stale = selector.select(candidates, rules, cross, 200, 200, 0.5, false);
    if (stale.valid) return 3;
    selector.reset();
    const auto bodyOnly = selector.select({candidates.front()}, rules, cross, 200, 200, 0.5, true);
    if (!bodyOnly.valid || bodyOnly.classId != 0 || bodyOnly.halfWidth != 50 ||
        bodyOnly.halfHeight != 80) return 4;
    if (selector.select(candidates, {}, cross, 200, 200, 0.5, true).valid) return 5;
    selector.reset();
    const std::vector<TriggerAimClass> wideRule = {{0, 0.5f, 0.5f, 1000, 1000}};
    const std::vector<control::Candidate> wideCandidate = {
        {{300, 100, 100, 100}, 0, 0.95}
    };
    const auto wide = selector.select(wideCandidate, wideRule, cross, 100, 100, 0.5, true);
    if (!wide.valid || !wide.contains(cross)) {
        std::puts("wide trigger range was incorrectly clipped by aim FOV");
        return 7;
    }
    return 0;
}
