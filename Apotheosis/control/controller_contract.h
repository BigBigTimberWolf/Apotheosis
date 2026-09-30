#pragma once

#include "anchor.h"
#include "selector.h"
#include "types.h"

#include <vector>
#include <cstdint>

namespace control {

enum class TrackLockState { Existing, New };

struct ClassAimPoint
{
    int classId = -1;
    double yOffset = 0.5;
    double yOffsetMax = 0.5;
    double xOffset = 0.5;
    double xOffsetMax = 0.5;
};

struct ControllerConfig
{
    int frameWidth = 320;
    int frameHeight = 320;
    int fovWidth = 0, fovHeight = 0;
    bool dynamicFovEnabled = false;
    int dynamicFovSize = 40;
    int dynamicFovShrinkMs = 200;
    int dynamicFovExpandMs = 120;
    ClassBuckets buckets;
    SelectorConfig selector;
    AimPointConfig aimPoint;
    std::vector<ClassAimPoint> classAimPoints;
    // Order in the active hotkey's aim_classes: index 0 has highest priority.
    std::vector<int> classPriorityById;
    bool requireFreshDetection = true;
    bool requireFreshCrosshair = true;
};

struct ControlInput
{
    std::vector<Candidate> candidates;
    Vec2 cross{};
    double dtSec = 0.0;
    int64_t observationTimeUs = 0;
    Vec2 motionEventSum{};
    uint64_t frameIndex = 0;
    bool detectionFresh = true;
    bool crosshairFresh = true;
    bool autoFire = false;
    // Extra downward target-point displacement while the selected hotkey is held.
    double aimpointRecoilYpx = 0.0;
};

struct ControlOutput
{
    bool engaged = false;
    Counts counts{};
    Vec2 anchor{};
    Vec2 controlAnchor{};
    Vec2 followStrength{}; // Active profile, after PID config sanitization.
    Vec2 followMotion{}, followPreset{};
    int followStateX = 0, followStateY = 0;
    Vec2 cross{};
    Vec2 error{};
    Vec2 fovRadii{};
    Vec2 trackedVelocity{};
    Vec2 derivativeRaw{};
    Box targetBox{};
    bool hasTarget = false;
    int targetId = -1;
    Vec2 filteredCenter{};
    TrackLockState lockState = TrackLockState::New;
    int targetClassId = -1;

    enum class IdleReason
    {
        None = 0,
        NoCandidates,
        StaleDetection,
        StaleCrosshair,
        BadDt,
    };
    IdleReason idleReason = IdleReason::None;
};

} // namespace control
