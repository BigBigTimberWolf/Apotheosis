#pragma once

#include "controller_contract.h"
#include "follow_compensator.h"
#include "dynamic_fov.h"
#include "recovered_pid.h"
#include "recovered_tracker.h"

namespace control {

// Runtime controller for the recovered direct-output path.
class RecoveredAimController
{
public:
    void setConfig(const ControllerConfig& config, const RecoveredPidConfig& pid);
    ControlOutput update(const ControlInput& input);
    void reset();
    void resetCompensation() { compensator_.reset(); directionObserver_.reset(); macroSmoothed_=macroCarry_={}; }
    void resetPidAxes(bool x, bool y) { pid_.resetAxes(x, y); }
    void seedPidDerivativeAfterPause() { pid_.seedDerivativeAfterPause(); }

private:
    ControllerConfig config_{};
    RecoveredDualTracker tracker_;
    RecoveredPid pid_;
    FollowCompensator compensator_;
    TargetDirectionObserver directionObserver_;
    DynamicFov fov_;
    int selectedId_ = -1;
    int selectedClassId_ = -1;
    // Keep the random X/Y sample for this target, including brief missed
    // detections that the tracker can associate back to the same ID.
    int anchorTargetId_ = -1;
    uint64_t anchorSampleIndex_ = 0;
    Box selectedBox_{};
    int selectionMisses_ = 0;
    uint64_t macroRevision_ = 0;
    int macroLockedId_ = -1;
    Vec2 macroSmoothed_{}, macroCarry_{};
};

} // namespace control
