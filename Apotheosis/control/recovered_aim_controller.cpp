#include "recovered_aim_controller.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace control {
namespace {

double distanceToBox(const Vec2& point, const Box& box)
{
    const double dx = std::max({ box.x - point.x, 0.0, point.x - (box.x + box.w) });
    const double dy = std::max({ box.y - point.y, 0.0, point.y - (box.y + box.h) });
    return std::hypot(dx, dy);
}

size_t chooseFresh(const std::vector<RecoveredTrack>& tracks,
                   const std::vector<size_t>& indices, const Vec2& cross)
{
    double minDistance = std::numeric_limits<double>::infinity();
    double maxDistance = 0.0;
    double minArea = std::numeric_limits<double>::infinity();
    double maxArea = 0.0;
    for (size_t index : indices)
    {
        const double d = distanceToBox(cross, tracks[index].box);
        const double a = tracks[index].box.area();
        minDistance = std::min(minDistance, d);
        maxDistance = std::max(maxDistance, d);
        minArea = std::min(minArea, a);
        maxArea = std::max(maxArea, a);
    }
    auto better = [&](size_t a, size_t b) {
        const double da = distanceToBox(cross, tracks[a].box);
        const double db = distanceToBox(cross, tracks[b].box);
        const double aa = tracks[a].box.area(), ab = tracks[b].box.area();
        const double nearA = maxDistance - minDistance > 0.001
            ? std::clamp(1.0 - (da - minDistance) / (maxDistance - minDistance), 0.0, 1.0) : 1.0;
        const double nearB = maxDistance - minDistance > 0.001
            ? std::clamp(1.0 - (db - minDistance) / (maxDistance - minDistance), 0.0, 1.0) : 1.0;
        const double sizeA = maxArea - minArea > 0.001
            ? std::clamp((aa - minArea) / (maxArea - minArea), 0.0, 1.0) : 1.0;
        const double sizeB = maxArea - minArea > 0.001
            ? std::clamp((ab - minArea) / (maxArea - minArea), 0.0, 1.0) : 1.0;
        if (nearA + 0.02 < nearB) return true;
        if (nearB + 0.02 < nearA) return false;
        if (sizeA + 0.02 < sizeB) return true;
        if (sizeB + 0.02 < sizeA) return false;
        if (da > db + 1.0) return true;
        if (db > da + 1.0) return false;
        if (aa + 1.0 < ab) return true;
        if (ab + 1.0 < aa) return false;
        return tracks[b].id < tracks[a].id;
    };
    size_t chosen = indices.front();
    for (size_t i = 1; i < indices.size(); ++i)
        if (better(chosen, indices[i])) chosen = indices[i];
    return chosen;
}

} // namespace

void RecoveredAimController::setConfig(const ControllerConfig& config,
                                       const RecoveredPidConfig& pid)
{
    const auto oldPid = pid_.config();
    pid_.setConfig(pid);
    const auto& cleanPid = pid_.config();
    if (oldPid.motionPixelsPerCountX != cleanPid.motionPixelsPerCountX ||
        oldPid.motionPixelsPerCountY != cleanPid.motionPixelsPerCountY ||
        oldPid.motionDelayMs != cleanPid.motionDelayMs)
        reset(); // never blend velocity learned under two different calibrations
    config_ = config;
    fov_.configure({double(config.fovWidth), double(config.fovHeight)},
                   config.dynamicFovEnabled, config.dynamicFovSize,
                   config.dynamicFovShrinkMs, config.dynamicFovExpandMs);
    tracker_.setFrameSize(config.frameWidth, config.frameHeight);
    tracker_.setMotionConversion({pid_.config().motionPixelsPerCountX,
                                  pid_.config().motionPixelsPerCountY});
}

ControlOutput RecoveredAimController::update(const ControlInput& input)
{
    ControlOutput out;
    out.cross = input.cross;
    out.fovRadii = fov_.radii();
    auto releaseFov = [&] {
        if (input.dtSec > 0.0 && std::isfinite(input.dtSec)) fov_.release(input.dtSec);
        else fov_.reset();
        out.fovRadii = fov_.radii();
    };
    if (!(input.dtSec > 0.0) || !std::isfinite(input.dtSec))
    {
        resetCompensation();
        out.idleReason = ControlOutput::IdleReason::BadDt;
        releaseFov();
        return out;
    }
    // Tracking still advances on empty detections, while PID and direct-output
    // carry are preserved across a plain no-target frame (105/106).
    const auto tracks = tracker_.update(input.detectionFresh ? input.candidates
                                                            : std::vector<Candidate>{},
                                        input.trackingDtSec > 0.0 ? input.trackingDtSec : input.dtSec,
                                        input.motionEventSum);
    const bool macroChanged=input.macro.commandSerial!=macroRevision_;
    const bool macroSelect=macroChanged&&(input.macro.command==1||input.macro.command==3);
    if(macroChanged){macroRevision_=input.macro.commandSerial;macroLockedId_=-1;}
    if(macroChanged&&input.macro.command==4){selectedId_=-1;resetCompensation();return out;}
    if (config_.requireFreshDetection && !input.detectionFresh)
    {
        resetCompensation();
        out.idleReason = ControlOutput::IdleReason::StaleDetection;
        releaseFov();
        return out;
    }
    if (config_.requireFreshCrosshair && !input.crosshairFresh)
    {
        resetCompensation();
        out.idleReason = ControlOutput::IdleReason::StaleCrosshair;
        releaseFov();
        return out;
    }

    std::vector<size_t> eligible;
    for (size_t index = 0; index < tracks.size(); ++index)
    {
        const auto& track = tracks[index];
        if(input.macro.classId>=0&&track.classId!=input.macro.classId)continue;
        if(macroLockedId_>=0&&track.id!=macroLockedId_)continue;
        if (input.macro.classId<0 && config_.buckets.bucketOf(track.classId) != Bucket::Aim) continue;
        if (track.confidence < config_.selector.minConfOf(track.classId)) continue;
        if (!fov_.contains(track.box, input.cross, track.id == selectedId_)) continue;
        if (config_.selector.maxDistancePx > 0.0 &&
            distanceToBox(input.cross, track.box) > config_.selector.maxDistancePx) continue;
        eligible.push_back(index);
    }
    if (eligible.empty())
    {
        macroLockedId_=-1;
        releaseFov();
        resetCompensation();
        if (++selectionMisses_ >= 3) { selectedId_ = -1; selectedClassId_ = -1; }
        out.idleReason = ControlOutput::IdleReason::NoCandidates;
        return out;
    }

    // The UI's class order is a selection priority, not merely display order.
    // Compare distance/size and retain a previous lock only within the best
    // currently visible class rank.
    int bestRank = std::numeric_limits<int>::max();
    for (size_t index : eligible) {
        const int id = tracks[index].classId;
        if (id >= 0 && static_cast<size_t>(id) < config_.classPriorityById.size()) {
            const int rank = config_.classPriorityById[static_cast<size_t>(id)];
            if (rank >= 0) bestRank = std::min(bestRank, rank);
        }
    }
    if (bestRank != std::numeric_limits<int>::max() && !input.macro.priority && !macroSelect) {
        eligible.erase(std::remove_if(eligible.begin(), eligible.end(), [&](size_t index) {
            const int id = tracks[index].classId;
            return id < 0 || static_cast<size_t>(id) >= config_.classPriorityById.size()
                || config_.classPriorityById[static_cast<size_t>(id)] != bestRank;
        }), eligible.end());
    }

    size_t chosen = eligible.front();
    bool reacquired = false;
    if (selectedId_ >= 0)
        for (size_t index : eligible)
            if (tracks[index].id == selectedId_)
            {
                const double oldArea = std::max(selectedBox_.area(), 1.0);
                const double newArea = std::max(tracks[index].box.area(), 1.0);
                const double ratio = std::max(oldArea, newArea) / std::min(oldArea, newArea);
                const double radius = std::max(0.75 * selectedBox_.diagonal(), 40.0);
                const double distance = (tracks[index].box.center() - selectedBox_.center()).norm();
                if (ratio <= 3.52 && distance <= 2.0 * radius)
                {
                    chosen = index;
                    reacquired = true;
                }
                break;
            }
    const auto rankOf = [&](int id) {
        return id >= 0 && static_cast<size_t>(id) < config_.classPriorityById.size() &&
            config_.classPriorityById[static_cast<size_t>(id)] >= 0
            ? config_.classPriorityById[static_cast<size_t>(id)]
            : std::numeric_limits<int>::max();
    };
    const bool higherPriorityAvailable = selectedId_ >= 0 && !eligible.empty() &&
        rankOf(tracks[eligible.front()].classId) < rankOf(selectedClassId_);
    if (selectedId_ >= 0 && !reacquired && !higherPriorityAvailable && !macroSelect && !input.macro.priority && ++selectionMisses_ < 3)
    {
        releaseFov();
        resetCompensation();
        out.idleReason = ControlOutput::IdleReason::NoCandidates;
        return out;
    }
    if (!reacquired)
        chosen = chooseFresh(tracks, eligible, input.cross);
    if((macroChanged&&(input.macro.command==1||input.macro.command==3))||input.macro.priority) {
        const bool requested=macroChanged&&(input.macro.command==1||input.macro.command==3);
        auto score=[&](size_t i){const auto& t=tracks[i];const auto point=requested?Vec2{input.macro.x,input.macro.y}:input.cross;
            const double distance=(t.box.center()-point).normSq();
            if(requested||input.macro.priority==1)return -distance;
            if(input.macro.priority==2)return distance;
            if(input.macro.priority==3)return t.confidence;
            return t.box.area();};
        for(auto i:eligible)if(score(i)>score(chosen))chosen=i;
        reacquired=tracks[chosen].id==selectedId_;
        if(requested&&input.macro.command==1)macroLockedId_=tracks[chosen].id;
    }
    if (!reacquired)
    {
        resetCompensation();
        // A new lock keeps the current radius: resetting here bypasses the
        // configured expansion time and abruptly admits distant candidates.
    }
    selectionMisses_ = 0;
    const auto& target = tracks[chosen];
    const bool changedTarget = selectedId_ != target.id;
    selectedId_ = target.id;
    selectedClassId_ = target.classId;
    selectedBox_ = target.box;
    fov_.follow(target.box, input.cross, input.dtSec);
    out.fovRadii = fov_.radii();

    out.targetId = target.id;
    out.targetClassId = target.classId;
    out.targetBox = target.box;
    out.trackedVelocity = target.velocity;
    out.filteredCenter = target.box.center();
    out.lockState = reacquired ? TrackLockState::Existing : TrackLockState::New;
    out.hasTarget = true;

    AimPointConfig point = config_.aimPoint;
    for (const auto& classPoint : config_.classAimPoints)
        if (classPoint.classId == target.classId)
        {
            point.xOffset = classPoint.xOffset;
            point.xOffsetMax = classPoint.xOffsetMax;
            point.yOffset = classPoint.yOffset;
            point.yOffsetMax = classPoint.yOffsetMax;
            break;
        }
    if (anchorTargetId_ != target.id)
    {
        anchorTargetId_ = target.id;
        anchorSampleIndex_ = input.frameIndex;
    }
    // Freeze the sample, not the screen coordinate: the selected position
    // continues to follow the target's current box as it moves or changes size.
    if(input.macro.partX>=0)point.xOffset=point.xOffsetMax=std::clamp(input.macro.partX,0.,1.);
    if(input.macro.partY>=0)point.yOffset=point.yOffsetMax=1-std::clamp(input.macro.partY,0.,1.);
    const Vec2 sampledPoint = computeAnchor(out.filteredCenter, target.box,
                                            point, anchorSampleIndex_);
    const double maxX = static_cast<double>(std::max(config_.frameWidth - 1, 0));
    const double maxY = static_cast<double>(std::max(config_.frameHeight - 1, 0));
    const double cx = std::round(std::clamp(target.box.centerX(), 0.0, maxX));
    const double cy = std::round(std::clamp(target.box.centerY(), 0.0, maxY));
    const double width = std::max(1.0, std::round(target.box.w));
    const double height = std::max(1.0, std::round(target.box.h));
    const double xRatio = (sampledPoint.x - out.filteredCenter.x) / target.box.w;
    const double yRatio = (sampledPoint.y - out.filteredCenter.y) / target.box.h;
    // The source rounds X to nearest (half away), but truncates Y before
    // clamping. Prediction displacement is added after this base point (21).
    const double x = std::round(std::clamp(cx + xRatio * width, 0.0, maxX));
    const double recoilY = std::isfinite(input.aimpointRecoilYpx)
        ? std::max(0.0, input.aimpointRecoilYpx) : 0.0;
    const double y = std::clamp(std::trunc(cy + yRatio * height) + recoilY, 0.0, maxY);
    out.anchor = Vec2{ x, y };
    out.controlAnchor = out.anchor;
    out.error = out.anchor - input.cross;

    RecoveredPidConfig pidConfig = pid_.config();
    // Preserve the original auto-fire deadzone bypass.
    pidConfig.skipDeadzone = input.autoFire &&
        distanceToBox(input.cross, target.box) <= target.box.diagonal() * 0.25;
    pid_.setConfig(pidConfig);
    out.followStrength = {pidConfig.maskX ? 0.0 : pidConfig.followX,
                          pidConfig.maskY ? 0.0 : pidConfig.followY};
    // A confirmed target maneuver (reversal/sudden stop, from the self-motion-
    // compensated predict velocity) clears the follow lead immediately; normal
    // movement across the aim point keeps it.
    const auto offset = compensator_.update(out.error,
        {double(config_.frameWidth), double(config_.frameHeight)},
        out.followStrength, input.observationTimeUs, input.dtSec, target.maneuver);
    // Compensation telemetry remains original-error rate, independent of FF.
    out.followMotion = compensator_.errorRate();
    // Selected track velocity includes frame-aligned, calibrated self-motion.
    const Vec2 velocityFeedforward = target.velocity;
    out.followStateX = compensator_.stateX();
    out.followStateY = compensator_.stateY();
    // Extrapolation always uses the maneuver-aware predict velocity (snaps within
    // one observation frame on reversal/stop/new target); the PID feedforward
    // above keeps the frozen smooth velocity.
    // The Smith lead hands back the in-flight motion that the target's own movement
    // explains, so the subtraction of `pendingMotionPx` below only removes what the
    // target cannot account for. It is zero until the send-to-image delay is calibrated.
    const Vec2 smithLead = smithLead_.update(velocityFeedforward, input.pendingMotionPx,
                                             pidConfig.motionDelayMs >= 0.0f ? pidConfig.motionDelayMs * 1e-3 : 0.0,
                                             input.dtSec);
    out.controlAnchor = out.anchor + offset + target.predictVelocity*(std::clamp(input.macro.predictionMs,0.,500.)/1000.)
        + smithLead;
    if (changedTarget) {
        // Frozen second-port switch rule: do not carry old-target I or produce
        // a one-frame D spike from the old target's error.
        pid_.resetIntegral();
        pid_.seedDerivativeAfterPause();
    }
    const Vec2 pidError = out.controlAnchor - input.cross - input.pendingMotionPx;
    const auto step = pid_.update(pidError, velocityFeedforward, input.dtSec);
    out.counts = step.counts;
    if(input.macro.smoothing<1||input.macro.speed>0) {
        const double alpha=std::clamp(input.macro.smoothing,0.01,1.);
        macroSmoothed_=macroSmoothed_*(1-alpha)+Vec2{double(out.counts.x),double(out.counts.y)}*alpha;
        Vec2 movement=macroSmoothed_;
        const double limit=input.macro.speed*input.dtSec;
        if(limit>0&&movement.norm()>limit)movement=movement*(limit/movement.norm());
        movement+=macroCarry_;out.counts={int(std::trunc(movement.x)),int(std::trunc(movement.y))};
        macroCarry_=movement-Vec2{double(out.counts.x),double(out.counts.y)};
    } else macroSmoothed_=macroCarry_={};
    // Use the PID's actual clipping decision, including masks, free-wiggle
    // deadzones and skipped sends. Never reconstruct FF in a second formula.
    compensator_.setSaturation(step.saturation);
    out.derivativeRaw = step.derivativeRaw;
    out.engaged = true;
    return out;
}

void RecoveredAimController::reset()
{
    macroRevision_=0;macroLockedId_=-1;macroSmoothed_=macroCarry_={};
    fov_.reset();
    tracker_.reset();
    pid_.reset();
    resetCompensation();
    selectedId_ = -1;
    selectedClassId_ = -1;
    anchorTargetId_ = -1;
    anchorSampleIndex_ = 0;
    selectedBox_ = {};
    selectionMisses_ = 0;
}

} // namespace control
