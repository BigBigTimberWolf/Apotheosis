#pragma once

#include "trigger_fsm.h"
#include "trigger_scope.h"

namespace boss {

struct TargetLossRelease
{
    bool left = false;
    bool right = false;
};

// 丢失目标时归还长按的按键。点按开镜只释放待完成的短按，保留已开镜状态，
// 避免重新发现目标时再点一次右键而把游戏里的镜关掉。
template<class Trigger>
inline TargetLossRelease releaseOnTargetLoss(Trigger& trigger,
                                             ScopeController& scope,
                                             int scopeMode, int64_t now_ms,
                                             int holdGraceMs = 0,
                                             bool finishTimedPress = false)
{
    TargetLossRelease release;
    bool timedRelease = false;
    if (finishTimedPress && scope.mode() == scopeMode && trigger.pressed()) {
        const auto timed = trigger.tick({false, false, -1, now_ms},
                                        false, 0, 1, 0, 0, 0, 0, 0);
        timedRelease = timed.release_left;
        if (!timedRelease && trigger.pressed()) {
            release.right = scope.tick(scope.engaged(), true,
                                       scopeMode, 0, now_ms).release_right;
            return release;
        }
    }
    const bool keepHolding = scope.mode() == scopeMode &&
        trigger.keepHoldingOnTargetLoss(now_ms, holdGraceMs);
    if (!keepHolding) {
        const bool wasPressed = trigger.reset();
        release.left = timedRelease || wasPressed;
    }
    // Preserve an already held scope during the grace period, but never open
    // a new scope without a target. Pending tap releases still run on time.
    release.right = scope.tick(keepHolding && scope.engaged(), true,
                               scopeMode, 0, now_ms).release_right;
    return release;
}

// 自动开镜尚未就绪时扳机不会 tick；若左键已按下，必须在跳过状态机前归还。
template<class Trigger>
inline bool releaseTriggerIfUnavailable(Trigger& trigger,
                                        bool triggerEnabled,
                                        bool scopeReady)
{
    return (!triggerEnabled || !scopeReady) && trigger.reset();
}

}
