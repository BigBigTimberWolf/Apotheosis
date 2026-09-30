#include "mouse/auto_stop.h"
#include "mouse/trigger_fsm.h"
#include "mouse/trigger_scope.h"
#include "mouse/switch31_shot_gate.h"

#include <stdexcept>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

static boss::TriggerFsm::Action tick(boss::TriggerFsm& fsm, int64_t at,
                                     bool inZone, int fireDelay, int preMs)
{
    boss::TriggerFsm::Input input;
    input.in_zone = inZone;
    input.track_id = 1;
    input.now_ms = at;
    return fsm.tick(input, false, fireDelay, 20, 200, 0, 0, 0, 0, preMs);
}

int main()
{
    // A requested 50 ms lead delays a zero-delay shot until the mask has
    // actually had 50 ms to work.
    boss::TriggerFsm fsm;
    CHECK(tick(fsm, 1000, true, 0, 50).prepare_fire);
    CHECK(!tick(fsm, 1040, true, 0, 50).fired);
    CHECK(tick(fsm, 1050, true, 0, 50).fired);

    // For an existing 100 ms fire delay, preparation starts 30 ms before it.
    fsm.reset();
    CHECK(!tick(fsm, 2000, true, 100, 30).prepare_fire);
    CHECK(!tick(fsm, 2060, true, 100, 30).prepare_fire);
    CHECK(tick(fsm, 2070, true, 100, 30).prepare_fire);
    CHECK(tick(fsm, 2100, true, 100, 30).fired);

    // A late detection of the preparation window moves the shot, preserving
    // the full lead time even at a coarse frame cadence.
    fsm.reset();
    tick(fsm, 3000, true, 100, 30);
    CHECK(tick(fsm, 3080, true, 100, 30).prepare_fire);
    CHECK(!tick(fsm, 3100, true, 100, 30).fired);
    CHECK(tick(fsm, 3110, true, 100, 30).fired);

    // Losing the target during the lead window cancels that pending shot.
    fsm.reset();
    CHECK(tick(fsm, 4000, true, 0, 50).prepare_fire);
    CHECK(!tick(fsm, 4020, false, 0, 50).fired);
    CHECK(fsm.phase() == boss::TriggerPhase::Idle);
    CHECK(tick(fsm, 4030, true, 0, 50).prepare_fire);
    CHECK(!tick(fsm, 4060, true, 0, 50).fired);
    CHECK(tick(fsm, 4080, true, 0, 50).fired);

    // The scope opening delay and stop lead run on the same timeline.
    // A 30 ms scope delay with a 50 ms stop lead fires at 50 ms, not 80 ms.
    fsm.reset();
    boss::ScopeController scope;
    CHECK(scope.tick(true, true, 2, 30, 6000).press_right);
    CHECK(scope.remainingDelayMs(true, 2, 30, 6000) == 30);
    CHECK(tick(fsm, 6000, true, scope.remainingDelayMs(true, 2, 30, 6000), 50).prepare_fire);
    CHECK(!tick(fsm, 6030, true, 0, 50).fired);
    CHECK(scope.ready(true, 2, 30, 6030));
    CHECK(tick(fsm, 6050, true, 0, 50).fired);

    CHECK(boss::physicalScopeRemainingDelayMs(30, 8000, 8010) == 20);
    CHECK(boss::physicalScopeRemainingDelayMs(30, 8000, 8030) == 0);

    // Lua-style continuous stop can fire in the entry tick without a timed lead.
    fsm.reset();
    CHECK(tick(fsm, 7000, true, 0, 0).fired);

    boss::AutoStopController stop;
    stop.markPrepared();
    CHECK(stop.active() && stop.preparing());
    stop.markFired(5000, 60);
    CHECK(!stop.shouldCancel(5059));
    CHECK(stop.shouldCancel(5060));
    stop.reset();
    CHECK(!stop.active());

    // 连续两个目标：每一枪都要先点开镜、准备急停，再触发左键。
    boss::TriggerFsm cycles;
    boss::ScopeController repeatedScope;
    mouse_async::Switch31ShotGate shotGate;
    auto cycleTick = [&](int64_t at, int target) {
        boss::TriggerFsm::Input input;
        input.in_zone = true;
        input.prerequisite_ready = repeatedScope.ready(true, 1, 20, at);
        input.track_id = target;
        input.now_ms = at;
        return cycles.tick(input, false, 0, 20, 10, 0, 0, 0, 0, 50);
    };
    CHECK(repeatedScope.tick(true, true, 1, 20, 8000).press_right);
    CHECK(!cycleTick(8000, 1).fired);
    CHECK(repeatedScope.tick(true, true, 1, 20, 8020).release_right);
    CHECK(cycleTick(8020, 1).prepare_fire);
    CHECK(cycleTick(8070, 1).fired);
    shotGate.onPress(true, true, 50, 20, 8070);
    CHECK(cycleTick(8090, 1).release_left);
    CHECK(shotGate.onRelease(8090) == 50);
    CHECK(shotGate.onRelease(8100) == -1);
    repeatedScope.forceRelease(); // 3-1 切枪完成，游戏退出镜内
    CHECK(repeatedScope.tick(true, true, 1, 20, 8300).press_right);
    CHECK(!cycleTick(8300, 2).fired);
    CHECK(repeatedScope.tick(true, true, 1, 20, 8320).release_right);
    CHECK(cycleTick(8320, 2).prepare_fire);
    CHECK(!cycleTick(8360, 2).fired);
    CHECK(cycleTick(8370, 2).fired);
    shotGate.onPress(true, true, 50, 20, 8370);
    CHECK(cycleTick(8390, 2).release_left);
    CHECK(shotGate.onRelease(8390) == 50);

    // A rejected left-down must never authorize a weapon switch.
    shotGate.onPress(false, true, 50, 20, 8500);
    CHECK(shotGate.onRelease(8520) == -1);

    stop.markContinuous(6000);
    CHECK(stop.active() && stop.continuous());
    CHECK(!stop.continuousNeedsRefresh(7499));
    CHECK(stop.continuousNeedsRefresh(7500));
    CHECK(!stop.shouldCancel(7500));
    stop.reset();
    CHECK(!stop.active());
    return 0;
}
