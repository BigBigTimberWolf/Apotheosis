#include "mouse/am_trigger.h"
#include "mouse/trigger_release.h"
#include <iostream>
#include <stdexcept>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

using boss::AmFireMode;
using boss::AmTriggerFsm;
static AmTriggerFsm::Action tick(AmTriggerFsm& f, int64_t t, bool hit = true,
                                int start = 0, int down = 0, int up = 0, int track = 1)
{
    return f.tick({hit, true, track, t}, false, start, down, up, 1000, 0, 0, 0);
}
int main() {
    for (int i = 0; i < 1000; ++i) {
        CHECK(boss::amDelay(0, 20) == 0);
        CHECK(boss::amDelay(-1, 20) == 0);
        CHECK(boss::amDelay(30, 0) == 30);
        const int delay = boss::amDelay(5, 20);
        CHECK(delay >= 0 && delay <= 25);
    }
    CHECK(boss::amFireMode(-1, 0) == AmFireMode::SmartContinuous);
    CHECK(boss::amFireMode(-1, 20) == AmFireMode::SmartClick);
    {
        AmTriggerFsm f;
        CHECK(f.tick({true, true, 1, 0}, true, 0, 0, 200, 0, 20, 20, 20).press_left);
        CHECK(f.reset());
        CHECK(!f.reset());
    }
    {
        AmTriggerFsm f; f.configure(AmFireMode::SmartClick, 20);
        CHECK(tick(f, 0, true, 500, 5, 12).press_left); // start delay ignored
        CHECK(!tick(f, 4, true, 500, 5, 12).release_left);
        CHECK(tick(f, 5, true, 500, 5, 12).release_left);
        CHECK(!tick(f, 16, true, 500, 5, 12).press_left);
        CHECK(!tick(f, 17, true, 500, 5, 12).press_left); // cleanup observation
        CHECK(tick(f, 18, true, 500, 5, 12).press_left);
        CHECK(tick(f, 19, false).release_left); // loss cancels a timed press
    }
    {
        AmTriggerFsm f; f.configure(AmFireMode::Burst, 20);
        CHECK(!tick(f, 0, true, 20).press_left);
        CHECK(!tick(f, 19, true, 20, 0, 0, 2).press_left);
        CHECK(tick(f, 20, true, 20, 0, 0, 3).press_left); // IDs don't restart delay
        CHECK(!tick(f, 29).release_left);
        CHECK(tick(f, 30).release_left); // minimum down=10
        CHECK(!tick(f, 39).press_left);
        CHECK(!tick(f, 40).press_left); // minimum up=10 + cleanup
        CHECK(tick(f, 41).press_left);
        CHECK(tick(f, 42, false).release_left);
        CHECK(!tick(f, 43, true, 20).press_left); // reacquisition restarts initial delay
        CHECK(tick(f, 63, true, 20).press_left);
    }
    {
        AmTriggerFsm f; f.configure(AmFireMode::SmartContinuous, 20);
        CHECK(tick(f, 0).press_left);
        CHECK(f.holdZoneOnBriefMiss(false, 2, 10, 20));
        CHECK(!tick(f, 10, false).release_left);
        CHECK(f.holdZoneOnBriefMiss(false, 3, 29, 20));
        CHECK(!tick(f, 29, false).release_left);
        CHECK(!f.holdZoneOnBriefMiss(false, 4, 30, 20));
        CHECK(tick(f, 30, false).release_left); // repeated misses cannot extend deadline
        CHECK(tick(f, 31).press_left); // no extra cooldown for held fire
        CHECK(!tick(f, 32, false).release_left);
        CHECK(!tick(f, 45, true, 0, 0, 0, 7).release_left); // any valid target resets loss
        CHECK(!tick(f, 60, false).release_left);
        CHECK(tick(f, 80, false).release_left);
    }
    {
        AmTriggerFsm f; boss::ScopeController scope(true);
        f.configure(AmFireMode::Continuous, 0);
        CHECK(!tick(f, 0, false).press_left); // never begins outside region
        CHECK(tick(f, 1).press_left);
        CHECK(scope.tick(true, true, 2, 0, 1).press_right);
        auto lost = boss::releaseOnTargetLoss(f, scope, 2, 10000, 0);
        CHECK(!lost.left && !lost.right && f.pressed());
        CHECK(f.configure(AmFireMode::SmartContinuous, 20)); // mode switch releases
        CHECK(scope.forceRelease().release_right);
    }
    {
        AmTriggerFsm f; f.configure(AmFireMode::Continuous, 20);
        CHECK(!tick(f, 0, true, 50).press_left);
        CHECK(f.continuousActive());
        CHECK(!tick(f, 49, false, 50).press_left);
        CHECK(tick(f, 50, false, 50).press_left); // continuous activation is latched
        CHECK(f.reset());
        CHECK(!f.continuousActive());
        CHECK(!tick(f, 51, false).press_left);
    }
    {
        AmTriggerFsm f; boss::ScopeController scope(true);
        f.configure(AmFireMode::SmartContinuous, 20);
        CHECK(tick(f, 0).press_left);
        CHECK(scope.tick(true, true, 2, 0, 0).press_right);
        CHECK(!boss::releaseOnTargetLoss(f, scope, 2, 1, 20).left);
        auto lost = boss::releaseOnTargetLoss(f, scope, 2, 21, 20);
        CHECK(lost.left && !lost.right); // AM scope stays open through loss
        CHECK(scope.forceRelease().release_right);
    }
    {
        boss::ScopeController scope(true);
        CHECK(scope.tick(true, true, 1, 0, 0, 20).press_right);
        CHECK(!scope.ready(true, 1, 0, 29));
        CHECK(!scope.tick(true, true, 1, 0, 29, 20).release_right);
        CHECK(scope.tick(true, true, 1, 0, 30, 20).release_right);
        CHECK(scope.ready(true, 1, 0, 30));
        CHECK(!scope.tick(false, true, 1, 0, 31).press_right);
        CHECK(!scope.tick(true, true, 1, 0, 32).press_right);
        scope.forceRelease();
        CHECK(scope.tick(true, true, 1, 80, 100).press_right);
        CHECK(!scope.tick(true, true, 1, 80, 179).release_right);
        CHECK(scope.tick(true, true, 1, 80, 180).release_right);
        scope.forceRelease();
        CHECK(scope.tick(true, true, 2, 500, 200).press_right);
        CHECK(scope.ready(true, 2, 500, 200));
        CHECK(scope.tick(false, false, 2, 0, 201).release_right);
        CHECK(!scope.engaged());
        CHECK(scope.tick(true, true, 1, 0, 202).press_right);
        CHECK(scope.tick(false, false, 1, 0, 203).release_right);
        CHECK(!scope.engaged());
    }
    std::cout << "AM trigger timing, modes, cancellation and scope checks passed\n";
}
