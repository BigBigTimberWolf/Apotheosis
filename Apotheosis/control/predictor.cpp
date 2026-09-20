#include "predictor.h"

#include <algorithm>
#include <cmath>

namespace control {

namespace {

// 钳住速度：保留方向，只压大小。
// 这一点是刻意的 —— 速度超上限时方向通常仍然可信
// （真正方向错乱的框已经被稳定器的 Snap 判据拦掉并触发复位了），
// 所以压大小比整拍放弃预测的手感更连续。
Vec2 clampVelocity(const Vec2& v, double maxSpeed, bool& clamped)
{
    clamped = false;
    if (!(maxSpeed > 0.0))
        return v;

    const double speed = v.norm();
    if (!(speed > maxSpeed))
        return v;

    clamped = true;
    return v * (maxSpeed / speed);
}

}

PredictorResult predictAnchor(const Vec2& filteredCenter,
                              const Vec2& velocityPxPerSec,
                              const Box& targetBox,
                              const PredictorConfig& cfg,
                              PredictorState& state)
{
    (void)state;   // 当前无需跨帧状态；保留参数以统一 reset 语义

    PredictorResult out;
    out.center = filteredCenter;
    out.predictedCenter = filteredCenter;
    out.rawVelocity = velocityPxPerSec;

    if (!cfg.enabled())
    {
        out.idleReason = PredictorResult::IdleReason::Disabled;
        return out;
    }

    // ★ 提前时间完全以配置为准 —— 不做任何自动累加。
    //   用户填的就是整条链路的全部延迟，程序不再往里加东西，
    //   来源单一，避免"填了全延迟、程序又替我加了一遍"的重复计算。
    out.leadSec = cfg.leadMs * 0.001;

    // 速度为 0 时推进量恒为 0，没有意义但也不算错；
    // 直接按未生效返回，日志上更好读。
    if (!(velocityPxPerSec.normSq() > 0.0))
    {
        out.idleReason = PredictorResult::IdleReason::NotInitialized;
        return out;
    }

    // ---- ① 速度上限：钳住速度（保方向）----
    bool velClamped = false;
    const Vec2 v = clampVelocity(velocityPxPerSec, cfg.maxVelocityPxPerSec, velClamped);
    out.velocityClamped = velClamped;
    out.clampedVelocity = v;

    // ---- ② 位移 = 钳制后的速度 × 实际提前时间 ----
    Vec2 lead = v * out.leadSec;

    // ---- ③ 距离上限：× 目标框对角线 ----
    if (cfg.maxLeadRatio > 0.0)
    {
        const double diag = targetBox.diagonal();
        if (diag > 0.0 && std::isfinite(diag))
        {
            const double maxLeadPx = cfg.maxLeadRatio * diag;
            out.maxLeadPx = maxLeadPx;

            const double leadLen = lead.norm();
            if (leadLen > maxLeadPx && leadLen > 0.0)
            {
                lead = lead * (maxLeadPx / leadLen);
                out.leadClamped = true;
            }
        }
    }

    // 防御：任何非有限值都不允许进入控制回路。
    if (!std::isfinite(lead.x) || !std::isfinite(lead.y))
    {
        out.idleReason = PredictorResult::IdleReason::NotInitialized;
        out.lead = Vec2{};
        return out;
    }

    out.lead = lead;
    out.predictedCenter = filteredCenter + lead;
    out.applied = true;
    out.idleReason = PredictorResult::IdleReason::None;
    return out;
}

}
