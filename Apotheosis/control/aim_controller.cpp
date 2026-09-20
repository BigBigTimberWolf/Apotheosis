#include "aim_controller.h"

#include "alpha_beta_filter.h"

#include <cmath>

namespace control {

AimController::AimController()
{
    filter_ = std::make_unique<AlphaBetaFilter>();
}

AimController::~AimController() = default;

void AimController::setFilter(std::unique_ptr<IFilter> filter)
{
    filter_ = filter ? std::move(filter) : std::make_unique<AlphaBetaFilter>();
    if (filter_)
        filter_->reset();
}

void AimController::reset()
{
    selectorState_.reset();
    stabilizerState_.reset();
    predictorState_.reset();
    if (filter_)
        filter_->reset();
    pid_.reset();
    hasLastBox_ = false;
    lastBox_ = Box{};
    lastSentCounts_ = Counts{ 0, 0 };
}

ControlOutput AimController::update(const ControlInput& in)
{
    ControlOutput out;
    out.cross = in.cross;

    if (!(in.dtSec > 0.0))
    {
        out.idleReason = ControlOutput::IdleReason::BadDt;
        return out;
    }

    if (cfg_.requireFreshDetection && !in.detectionFresh)
    {
        out.idleReason = ControlOutput::IdleReason::StaleDetection;
        return out;
    }
    if (cfg_.requireFreshCrosshair && !in.crosshairFresh)
    {
        out.idleReason = ControlOutput::IdleReason::StaleCrosshair;
        return out;
    }

    const std::vector<size_t> aimIdx = filterAimCandidates(in.candidates, cfg_.buckets,
                                                           cfg_.selector);
    if (aimIdx.empty())
    {
        selectorState_.reset();
        stabilizerState_.reset();
        predictorState_.reset();
        if (filter_) filter_->reset();
        pid_.reset();
        hasLastBox_ = false;
        out.idleReason = ControlOutput::IdleReason::NoCandidates;
        return out;
    }

    const TargetSelection sel = selectTarget(
        in.candidates, aimIdx, in.cross, cfg_.selector, selectorState_);
    if (!sel.found)
    {
        out.idleReason = ControlOutput::IdleReason::NoCandidates;
        return out;
    }

    Candidate chosen;
    chosen.box = sel.box;
    chosen.classId = sel.classId;
    chosen.confidence = sel.confidence;

    const StabilizerResult stab = stabilize(chosen, cfg_.stabilizer, stabilizerState_);
    if (!stab.accepted)
    {
        out.idleReason = ControlOutput::IdleReason::RejectedByStabilizer;
        return out;
    }

    if (stab.verdict == StabilizerVerdict::Snap || stab.verdict == StabilizerVerdict::NoHistory)
    {
        if (filter_) filter_->reset();
        pid_.reset();
        // 目标跳变/新目标：清空预测状态，否则上一个目标遗留的延迟缓存
        // 会污染新目标的第一次预测。
        predictorState_.reset();
    }

    const Vec2 obsCenter = stab.box.center();
    filter_->observe(obsCenter, in.dtSec);
    const Vec2 filteredCenter = filter_->position();

    hasLastBox_ = true;
    lastBox_ = stab.box;

    AimPointConfig aimCfg = cfg_.aimPoint;
    for (const ClassAimPoint& cap : cfg_.classAimPoints)
    {
        if (cap.classId == sel.classId)
        {
            aimCfg.yOffset = cap.yOffset;
            aimCfg.yOffsetMax = cap.yOffsetMax;
            break;
        }
    }

    // 在途补偿：按目标速度把瞄准点往前推一段，抵消整条链路的延迟。
    // ★ 关键物理修正：画面上看到的目标移动并不等于目标的真实速度！
    // 因为准星在追着目标走，准星每追上一拍，画面里的目标相对位移就被抵消掉了一拍。
    // 目标真实速度 = 画面观测速度 + k * 鼠标自身下发速率 (把自身运动补偿回去)
    Vec2 trueVelocity = filter_->velocity();
    if (cfg_.pid.kPxPerCount > 0.0 && in.dtSec > 0.0)
    {
        // out.counts 尚未计算，使用上一拍下发的 countsRate
        const double mouseRateX = static_cast<double>(lastSentCounts_.x) / in.dtSec;
        const double mouseRateY = static_cast<double>(lastSentCounts_.y) / in.dtSec;
        trueVelocity.x += cfg_.pid.kPxPerCount * mouseRateX;
        trueVelocity.y += cfg_.pid.kPxPerCount * mouseRateY;
    }

    out.predictor = predictAnchor(filteredCenter, trueVelocity, stab.box,
                                  cfg_.predictor, predictorState_);

    out.anchor = computeAnchor(out.predictor.predictedCenter, stab.box, aimCfg,
                               in.frameIndex);

    out.targetBox = stab.box;
    out.hasTarget = true;
    if (stab.verdict == StabilizerVerdict::Snap || stab.verdict == StabilizerVerdict::NoHistory)
        ++targetIdCounter_;
    out.targetId = targetIdCounter_;

    out.error = out.anchor - out.cross;
    out.counts = pid_.update(out.anchor, out.cross, in.dtSec);
    lastSentCounts_ = out.counts;
    out.engaged = true;
    out.idleReason = ControlOutput::IdleReason::None;
    return out;
}

}
