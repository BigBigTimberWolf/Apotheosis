#include "mouse/trigger_target.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* message)
{
    if (!ok) { std::printf("FAIL: %s\n", message); ++failures; }
}
}

int main() {
    using boss::TriggerPriority;
    boss::TriggerTargetSelector selector;
    // Head first, body second: the list order is the priority.
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
    check(first.valid && first.classId == 1 && first.point.x == 140 &&
          std::abs(first.point.y - 98.0) < 0.001 && first.halfWidth == 4 && first.halfHeight == 6 &&
          first.contains(cross),
          "trigger priority, point, or hit zone failed");
    check(!first.contains({146, 98}) && first.containsExpanded({146, 98}, 50) &&
          !first.containsExpanded({160, 98}, 50),
          "expanded hit zone failed");
    const auto same = selector.select(candidates, rules, cross, 200, 200, 0.5, true);
    check(same.trackId == first.trackId, "the same box keeps its track id");
    check(!first.contains({first.point.x + first.halfWidth, first.point.y}),
          "AM excludes the zone border");

    // Higher priority in view but not under the crosshair yet: the body under the
    // crosshair must NOT fire; the head is the target and the crosshair keeps
    // moving until it enters the head zone.
    auto farHead = candidates;
    farHead[1].box.x = 190;
    const auto waiting = selector.select(farHead, rules, cross, 200, 200, 0.5, true);
    check(waiting.valid && waiting.classId == 1 && !waiting.contains(cross),
          "a visible higher-priority class holds fire while a lower one is under the crosshair");
    const auto waitingAgain = selector.select(farHead, rules, cross, 200, 200, 0.5, true);
    check(waitingAgain.trackId == waiting.trackId, "the held-for head keeps its track id");
    const control::Vec2 onHead{210, 98};
    const auto fireHead = selector.select(farHead, rules, onHead, 200, 200, 0.5, true);
    check(fireHead.valid && fireHead.classId == 1 && fireHead.contains(onHead),
          "fire once the crosshair enters the higher-priority zone");

    // ...but only while that class is really there.
    auto offscreen = candidates;
    offscreen[1].box.x = 400; // outside the 200x200 aiming FOV
    const auto headOutsideFov = selector.select(offscreen, rules, cross, 200, 200, 0.5, true);
    check(headOutsideFov.valid && headOutsideFov.classId == 0 && headOutsideFov.contains(cross),
          "a higher-priority class outside the FOV does not hold the lower one back");
    auto unsure = candidates;
    unsure[1] = {{190, 90, 40, 40}, 1, 0.3};
    const auto headNotRecognised = selector.select(unsure, rules, cross, 200, 200, 0.5, true);
    check(headNotRecognised.valid && headNotRecognised.classId == 0 && headNotRecognised.contains(cross),
          "a higher-priority class below the confidence threshold does not hold the lower one back");
    selector.reset();
    const auto bodyOnly = selector.select({candidates.front()}, rules, cross, 200, 200, 0.5, true);
    check(bodyOnly.valid && bodyOnly.classId == 0 && bodyOnly.halfWidth == 50 &&
          bodyOnly.halfHeight == 80,
          "without the higher-priority class the lower one fires");

    // Several boxes of the held class: one under the crosshair is enough to fire.
    auto twoHeads = farHead;
    twoHeads.push_back({{120, 90, 40, 40}, 1, 0.95});
    const auto oneHeadHit = selector.select(twoHeads, rules, cross, 200, 200, 0.5, true);
    check(oneHeadHit.valid && oneHeadHit.classId == 1 && oneHeadHit.box.x == 120 &&
          oneHeadHit.contains(cross),
          "a box of the higher-priority class under the crosshair fires even if another one is away");

    // Nothing under the crosshair: the highest-priority visible class is the target.
    const control::Vec2 nothingHit{250, 101};
    const auto visibleOnly = selector.select(candidates, rules, nothingHit, 200, 200, 0.5, true);
    check(visibleOnly.valid && visibleOnly.classId == 1 && !visibleOnly.contains(nothingHit),
          "nothing under the crosshair selects the highest-priority visible class");

    // No class list (legacy hit zone) has no priority: any class under the crosshair may fire.
    const auto anyHit = selector.select(farHead, rules, cross, 200, 200, 0.5, true,
                                        TriggerPriority::HitFirst);
    check(anyHit.valid && anyHit.classId == 0 && anyHit.contains(cross),
          "hit-first mode lets the class under the crosshair fire");
    const auto anyHitNothing = selector.select(candidates, rules, nothingHit, 200, 200, 0.5, true,
                                               TriggerPriority::HitFirst);
    check(anyHitNothing.valid && anyHitNothing.classId == 1 && !anyHitNothing.contains(nothingHit),
          "hit-first mode still prefers the highest-priority visible class when nothing is hit");

    const auto stale = selector.select(candidates, rules, cross, 200, 200, 0.5, false);
    check(!stale.valid, "stale detection selects nothing");
    check(!selector.select(candidates, {}, cross, 200, 200, 0.5, true).valid,
          "an empty class list selects nothing");
    selector.reset();
    const std::vector<TriggerAimClass> wideRule = {{0, 0.5f, 0.5f, 1000, 1000}};
    const std::vector<control::Candidate> wideCandidate = {
        {{300, 100, 100, 100}, 0, 0.95}
    };
    const auto wide = selector.select(wideCandidate, wideRule, cross, 100, 100, 0.5, true);
    check(wide.valid && wide.contains(cross), "wide trigger range was incorrectly clipped by aim FOV");

    std::printf("trigger target: %d failures\n", failures);
    return failures ? 1 : 0;
}
