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
inline TargetLossRelease releaseOnTargetLoss(TriggerFsm& trigger,
                                             ScopeController& scope,
                                             int scopeMode)
{
    TargetLossRelease release;
    release.left = trigger.reset();
    release.right = scope.tick(false, true, scopeMode, 0, 0).release_right;
    return release;
}

}
