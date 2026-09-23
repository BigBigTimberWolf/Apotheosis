#pragma once

#include "anchor.h"
#include "filter.h"
#include "pid_controller.h"
#include "predictor.h"
#include "selector.h"
#include "stabilizer.h"
#include "types.h"

#include <memory>
#include <vector>

namespace control {

struct ClassAimPoint
{
    int    classId = -1;
    double yOffset = 0.5;
    double yOffsetMax = 0.5;
};

struct ControllerConfig
{
    ClassBuckets buckets;
    SelectorConfig selector;
    StabilizerConfig stabilizer;
    AimPointConfig aimPoint;
    PidConfig pid;
    PredictorConfig predictor;

    // 灵敏度折算系数 k (像素/计数)。用于修正画面观测到的目标速度——准星追近
    // 一截，画面里目标的相对位移就被抵消一截，导致喂给 predictor 的速度系统性
    // 偏小；把自身下发速率按 k 折算回像素、加回观测速度，就能拿到目标真实速度。
    // 0 = 关闭这项修正（不需要标定就能用，只是预测会偏保守）。
    double pxPerCount = 0.0;

    std::vector<ClassAimPoint> classAimPoints;

    bool requireFreshDetection = true;
    bool requireFreshCrosshair = true;
};

struct ControlInput
{
    std::vector<Candidate> candidates;
    Vec2 cross;
    double dtSec = 0.0;
    uint64_t frameIndex = 0;
    bool detectionFresh = true;
    bool crosshairFresh = true;
};

struct ControlOutput
{
    bool engaged = false;
    Counts counts;
    Vec2 anchor;
    Vec2 cross;
    Vec2 error;
    Box targetBox{};
    bool hasTarget = false;
    int targetId = -1;

    // ── 供预览窗可视化「稳定之后是什么样」用的三样东西 ────────────────────
    //
    // ★ 稳定器本身【不改变框】(它只判定"还算不算同一个目标"并放行/丢弃),
    //   真正把位置抖动压下去的是它下游的 α-β 滤波器。所以"稳定后的效果"=
    //   稳定器放行的框 (targetBox) + 滤波后的中心 (filteredCenter);
    //   把两者画在一起, 就能直接看见抖动被压掉了多少。
    Vec2 filteredCenter{};                                  // α-β 滤波后的目标中心
    StabilizerVerdict stabVerdict = StabilizerVerdict::NoHistory;  // 本拍稳定器判定
    int targetClassId = -1;                                 // 锁定目标的类别 id

    enum class IdleReason
    {
        None = 0,
        NoCandidates,
        StaleDetection,
        StaleCrosshair,
        RejectedByStabilizer,
        BadDt,
    };
    IdleReason idleReason = IdleReason::None;

    // 本拍预测的完整信息（速度、实际提前时间、推进量、是否被钳制）。
    // 供日志/面板读取；lead_ms = 0 时 applied 恒为 false。
    PredictorResult predictor;
};

class AimController
{
public:
    AimController();
    ~AimController();

    void setConfig(const ControllerConfig& cfg)
    {
        cfg_ = cfg;
        pid_.setConfig(cfg.pid);
    }
    const ControllerConfig& config() const { return cfg_; }

    void setFilter(std::unique_ptr<IFilter> filter);

    IFilter* filter() const { return filter_.get(); }

    ControlOutput update(const ControlInput& in);

    void reset();

    const ControlTelemetry& telemetry() const { return pid_.telemetry(); }
    const SelectorState& selectorState() const { return selectorState_; }
    const StabilizerState& stabilizerState() const { return stabilizerState_; }

private:
    ControllerConfig cfg_;
    std::unique_ptr<IFilter> filter_;
    SelectorState selectorState_;
    StabilizerState stabilizerState_;
    PredictorState predictorState_;
    PidController pid_;

    bool hasLastBox_ = false;
    Box lastBox_;

    // 上一拍实际下发的计数，供本拍做自身速度污染修正 (见 pxPerCount)。
    Counts lastSentCounts_{ 0, 0 };

    int targetIdCounter_ = 0;
};

}
