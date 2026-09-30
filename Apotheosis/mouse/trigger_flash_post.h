#pragma once

#include "control/types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace boss {

// Owns the search turn between targets and the post-shot return. Aiming and
// firing remain in the ordinary aim controller and TriggerFsm path.
class TriggerFlashPostController {
public:
    enum class Phase { Idle, Tracking, Return, ReturnSettle, Turn, Settle, Stopped };

    struct Settings {
        int mode = 1;
        double pixelsPerCount = 1.0;
        int maxCounts = 500;
        int turnMaxCounts = 10000;
        int returnCountsPerSecond = 16000;
        int returnYPercent = 75;
        int spinCountsPerTurn = 2400;
        int turnStepDegrees = 90;
        int turnDurationMs = 40;
        int turnHoldMs = 300;
        int disappearMs = 80;
    };
    struct Input {
        bool fresh = false;
        bool selected = false;
        int selectedClassId = -1;
        control::Box selectedBox{};
        std::vector<control::Candidate> candidates;
        control::Counts confirmedMove{};
        bool feedbackArrived = false;
        bool scanCandidateVisible = false;
        int64_t nowMs = 0;
    };
    struct Action {
        control::Counts move{};
        bool blockNormal = false;
        bool targetMissing = false;
        bool cancelPendingMove = false;
        Phase phase = Phase::Idle;
    };

    void configure(Settings value) {
        value.mode = std::clamp(value.mode, 1, 2);
        value.pixelsPerCount = std::isfinite(value.pixelsPerCount)
            ? std::clamp(value.pixelsPerCount, 0.05, 50.0) : 1.0;
        value.maxCounts = std::clamp(value.maxCounts, 1, 500);
        value.turnMaxCounts = std::clamp(value.turnMaxCounts, 1, 30000);
        value.returnCountsPerSecond = std::clamp(value.returnCountsPerSecond, 2000, 40000);
        value.returnYPercent = std::clamp(value.returnYPercent, 0, 150);
        value.spinCountsPerTurn = std::clamp(value.spinCountsPerTurn, 100, 200000);
        value.turnStepDegrees = std::clamp(value.turnStepDegrees, 1, 180);
        value.turnDurationMs = std::clamp(value.turnDurationMs, 10, 1000);
        value.turnHoldMs = std::clamp(value.turnHoldMs, 0, 10000);
        value.disappearMs = std::clamp(value.disappearMs, 0, 2000);
        settings_ = value;
    }

    Action tick(const Input& in) {
        Action action;
        if (in.nowMs <= 0) return action;
        const int64_t elapsed = previousTickMs_ > 0
            ? std::clamp(in.nowMs - previousTickMs_, int64_t{1}, int64_t{100}) : 16;
        previousTickMs_ = in.nowMs;
        auto budget = [&](int rate, int maxStep) {
            credit_ = std::min(static_cast<double>(maxStep),
                credit_ + rate * elapsed / 1000.0);
            return static_cast<int>(std::floor(credit_));
        };

        if (phase_ == Phase::Tracking || phase_ == Phase::Return ||
            phase_ == Phase::ReturnSettle) {
            balance_.x += in.confirmedMove.x;
            balance_.y += in.confirmedMove.y;
            observed_.x += in.confirmedMove.x;
            observed_.y += in.confirmedMove.y;
        }
        if (phase_ == Phase::Turn && turnCommandSent_ && in.confirmedMove.x > 0)
            turnProgress_ = std::min(settings_.spinCountsPerTurn,
                                     turnProgress_ + in.confirmedMove.x);
        if (pending_ && (in.feedbackArrived || in.confirmedMove.x != 0 ||
                         in.confirmedMove.y != 0))
            pending_ = false;
        const int pendingTimeoutMs = phase_ == Phase::Turn ? 400 : 100;
        if (pending_ && in.nowMs - pendingSinceMs_ >= pendingTimeoutMs) {
            pending_ = false;
            action.cancelPendingMove = true;
        }

        if (phase_ == Phase::Idle && in.fresh) {
            if (eligibleSelection(in)) startTracking(in);
            else if (settings_.mode == 2) startNextTurn(in.nowMs);
        }

        if (phase_ == Phase::Tracking && in.fresh) {
            const bool present = matchTracked(in.candidates);
            observed_ = {};
            if (present) {
                missingSinceMs_ = -1;
                missingFrames_ = 0;
                // Once a shot has been sent, finish this target before the
                // ordinary selector is allowed to aim or fire at another one.
                if (fired_ && eligibleSelection(in) &&
                    !sameBox(in.selectedClassId, in.selectedBox,
                             trackedClassId_, trackedBox_))
                    action.blockNormal = true;
            } else if (fired_) {
                if (missingSinceMs_ < 0) {
                    missingSinceMs_ = in.nowMs;
                    missingFrames_ = 0;
                }
                ++missingFrames_;
                action.blockNormal = true;
                action.targetMissing = true;
                if (missingFrames_ >= 2 &&
                    in.nowMs - missingSinceMs_ >= settings_.disappearMs) {
                    completedClassId_ = trackedClassId_;
                    completedBox_ = trackedBox_;
                    completedUntilMs_ = in.nowMs + 500;
                    action.cancelPendingMove = true;
                    pending_ = false;
                    if (settings_.mode == 1) {
                        returnResidualY_ = static_cast<int>(std::lround(
                            balance_.y * (1.0 - settings_.returnYPercent / 100.0)));
                        phase_ = Phase::Return;
                    }
                    else startSettle(in.nowMs);
                }
            } else if (eligibleSelection(in)) {
                startTracking(in);
            } else {
                if (settings_.mode == 2) startSettle(in.nowMs);
                else phase_ = Phase::Idle;
            }
        }
        if (phase_ == Phase::Tracking && fired_ && missingSinceMs_ >= 0) {
            action.blockNormal = true;
            action.targetMissing = true;
        }

        if (phase_ == Phase::Return) {
            action.blockNormal = true;
            if (!pending_) {
                if (balance_.x == 0 && balance_.y == returnResidualY_) {
                    phase_ = Phase::ReturnSettle;
                    returnSettledAfterMs_ = in.nowMs + 50;
                    returnSettleFrames_ = 0;
                } else {
                    const int limit = std::min(settings_.maxCounts,
                        budget(settings_.returnCountsPerSecond, settings_.maxCounts));
                    action.move.x = std::clamp(-balance_.x, -limit, limit);
                    action.move.y = std::clamp(returnResidualY_ - balance_.y,
                                               -limit, limit);
                    if (action.move.x || action.move.y) markPending(in.nowMs);
                }
            }
        }
        if (phase_ == Phase::ReturnSettle) {
            action.blockNormal = true;
            if (balance_.x != 0 || balance_.y != returnResidualY_) {
                // A normal aim move can be confirmed after the return first
                // appears complete. Account for it before releasing the gate.
                phase_ = Phase::Return;
                returnSettleFrames_ = 0;
            } else {
                if (in.fresh) ++returnSettleFrames_;
                if (returnSettleFrames_ >= 2 && in.nowMs >= returnSettledAfterMs_) {
                    phase_ = Phase::Idle;
                    originActive_ = false;
                }
            }
        }
        if (phase_ == Phase::Turn) {
            action.blockNormal = true;
            if (!pending_) {
                if (turnProgress_ >= turnTargetCounts_) {
                    startSettle(in.nowMs);
                } else {
                    const auto elapsedTurn = std::max<int64_t>(0, in.nowMs - turnStartedMs_);
                    const double fraction = std::min(1.0,
                        static_cast<double>(elapsedTurn) / settings_.turnDurationMs);
                    const int scheduled = turnStartCounts_ + static_cast<int>(std::lround(
                        (turnTargetCounts_ - turnStartCounts_) * fraction));
                    action.move.x = std::min({settings_.turnMaxCounts,
                        turnTargetCounts_ - turnProgress_,
                        std::max(0, scheduled - turnProgress_)});
                    if (action.move.x > 0) {
                        turnCommandSent_ = true;
                        markPending(in.nowMs);
                    }
                }
            }
        }
        if (phase_ == Phase::Settle) {
            action.blockNormal = true;
            if (in.fresh) ++settleFrames_;
            if (in.fresh && eligibleSelection(in)) {
                startTracking(in);
            } else if (in.fresh && settleFrames_ >= 2 &&
                       in.nowMs >= settleUntilMs_) {
                startNextTurn(in.nowMs);
            }
        }
        if (phase_ == Phase::Stopped) {
            action.blockNormal = true;
            if (in.fresh && eligibleSelection(in)) startTracking(in);
        }
        if (phase_ != Phase::Tracking) action.targetMissing = false;
        action.phase = phase_;
        credit_ = std::max(0.0, credit_ -
            static_cast<double>(std::max(std::abs(action.move.x), std::abs(action.move.y))));
        return action;
    }

    void shotSent(int classId, const control::Box& box) {
        if (phase_ == Phase::Idle && box.valid()) {
            phase_ = Phase::Tracking;
            if (!originActive_) balance_ = {};
            observed_ = {};
            originActive_ = true;
        }
        if (phase_ != Phase::Tracking) return;
        fired_ = true;
        if (box.valid()) {
            trackedClassId_ = classId;
            trackedBox_ = box;
            missingSinceMs_ = -1;
            missingFrames_ = 0;
        }
    }
    void reset() {
        phase_ = Phase::Idle;
        trackedClassId_ = completedClassId_ = -1;
        trackedBox_ = completedBox_ = {};
        balance_ = observed_ = {};
        fired_ = pending_ = false;
        missingSinceMs_ = -1;
        missingFrames_ = 0;
        pendingSinceMs_ = previousTickMs_ = completedUntilMs_ = 0;
        settleUntilMs_ = turnStartedMs_ = 0;
        returnSettledAfterMs_ = 0;
        turnProgress_ = turnStartCounts_ = turnTargetCounts_ = 0;
        turnAngleDegrees_ = settleFrames_ = 0;
        returnSettleFrames_ = 0;
        turnCommandSent_ = false;
        credit_ = 0.0;
        originActive_ = false;
        returnResidualY_ = 0;
    }
    Phase phase() const { return phase_; }
    bool shotTargetLocked() const {
        return phase_ == Phase::Tracking && fired_;
    }
    double shotTargetDistance(const control::Candidate& candidate,
                              control::Counts newConfirmedMove) const {
        if (!shotTargetLocked() || candidate.classId != trackedClassId_ ||
            !candidate.box.valid()) return std::numeric_limits<double>::infinity();
        control::Box predicted = trackedBox_;
        predicted.x -= (observed_.x + newConfirmedMove.x) * settings_.pixelsPerCount;
        predicted.y -= (observed_.y + newConfirmedMove.y) * settings_.pixelsPerCount;
        const double distance = std::min(
            (candidate.box.center() - predicted.center()).norm(),
            (candidate.box.center() - trackedBox_.center()).norm());
        const double radius = std::max(30.0,
            std::max(candidate.box.diagonal(), trackedBox_.diagonal()) * 0.75);
        return distance <= radius ? distance
                                  : std::numeric_limits<double>::infinity();
    }

private:
    static bool sameBox(int aClass, const control::Box& a,
                        int bClass, const control::Box& b) {
        if (aClass != bClass || !a.valid() || !b.valid()) return false;
        return (a.center() - b.center()).norm() <=
            std::max(12.0, std::max(a.diagonal(), b.diagonal()) * 0.35);
    }
    bool eligibleSelection(const Input& in) const {
        return in.selected && in.selectedBox.valid() &&
            !(in.nowMs < completedUntilMs_ && sameBox(in.selectedClassId,
                in.selectedBox, completedClassId_, completedBox_));
    }
    void startTracking(const Input& in) {
        const bool keepOriginalView = settings_.mode == 1 && originActive_;
        phase_ = Phase::Tracking;
        trackedClassId_ = in.selectedClassId;
        trackedBox_ = in.selectedBox;
        if (!keepOriginalView) balance_ = {};
        observed_ = {};
        originActive_ = true;
        fired_ = pending_ = false;
        missingSinceMs_ = -1;
        missingFrames_ = 0;
    }
    void startNextTurn(int64_t nowMs) {
        pending_ = false;
        if (turnAngleDegrees_ >= 360 ||
            turnProgress_ >= settings_.spinCountsPerTurn) {
            phase_ = Phase::Stopped;
            return;
        }
        turnAngleDegrees_ = std::min(360,
            turnAngleDegrees_ + settings_.turnStepDegrees);
        turnStartCounts_ = turnProgress_;
        turnTargetCounts_ = static_cast<int>(std::lround(
            static_cast<double>(settings_.spinCountsPerTurn) *
            turnAngleDegrees_ / 360.0));
        turnStartedMs_ = nowMs;
        turnCommandSent_ = false;
        phase_ = Phase::Turn;
    }
    void startSettle(int64_t nowMs) {
        phase_ = Phase::Settle;
        settleUntilMs_ = nowMs + settings_.turnHoldMs;
        settleFrames_ = 0;
    }
    void markPending(int64_t nowMs) {
        pending_ = true;
        pendingSinceMs_ = nowMs;
    }
    bool matchTracked(const std::vector<control::Candidate>& candidates) {
        control::Box predicted = trackedBox_;
        predicted.x -= observed_.x * settings_.pixelsPerCount;
        predicted.y -= observed_.y * settings_.pixelsPerCount;
        const control::Candidate* best = nullptr;
        double distance = 1e100;
        for (const auto& candidate : candidates) {
            if (candidate.classId != trackedClassId_ || !candidate.box.valid()) continue;
            // Frames can be captured before the latest confirmed mouse move.
            // Accept proximity to either the last observed box or its
            // movement-adjusted estimate before declaring the target gone.
            const double d = std::min(
                (candidate.box.center() - predicted.center()).norm(),
                (candidate.box.center() - trackedBox_.center()).norm());
            const double radius = std::max(30.0,
                std::max(candidate.box.diagonal(), trackedBox_.diagonal()) * 0.75);
            if (d < distance && d <= radius) {
                distance = d;
                best = &candidate;
            }
        }
        if (!best) return false;
        trackedBox_ = best->box;
        return true;
    }

    Settings settings_{};
    Phase phase_ = Phase::Idle;
    int trackedClassId_ = -1, completedClassId_ = -1;
    control::Box trackedBox_{}, completedBox_{};
    control::Counts balance_{}, observed_{};
    int64_t missingSinceMs_ = -1;
    int missingFrames_ = 0;
    int64_t pendingSinceMs_ = 0, previousTickMs_ = 0;
    int64_t completedUntilMs_ = 0;
    int64_t settleUntilMs_ = 0, turnStartedMs_ = 0;
    int64_t returnSettledAfterMs_ = 0;
    int turnProgress_ = 0, turnStartCounts_ = 0, turnTargetCounts_ = 0;
    int turnAngleDegrees_ = 0, settleFrames_ = 0;
    int returnSettleFrames_ = 0;
    int returnResidualY_ = 0;
    double credit_ = 0.0;
    bool fired_ = false, pending_ = false;
    bool turnCommandSent_ = false;
    bool originActive_ = false;
};

} // namespace boss
