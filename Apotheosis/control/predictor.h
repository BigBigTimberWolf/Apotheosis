#pragma once

#include "types.h"

#include <cstdint>

namespace control {

// 在途补偿（预测提前量）。
//
// 目的：整条链路（采集 → 推理 → 瞄准 → 鼠标下发 → 游戏渲染）有几十毫秒延迟，
// 等这一拍算完时目标已经跑掉了。预测就是按目标速度把瞄准点往前推一段，
// 抵消这段"在途"时间。
//
// 生效规则（三颗旋钮，各管一段，职责不重叠）：
//   leadMs       ── 总开关 + 唯一的提前时间来源。== 0 时预测整体不生效，
//                    另外两个参数不读取。
//   maxVelocity  ── 速度上限。超出则【钳住速度】（保留方向，只压大小）。
//                    0 = 不限制。
//   maxLeadRatio ── 预测距离上限，单位是【目标框对角线倍数】。
//                    1.0 = 最多提前一个对角线；0 = 不限制。
//
// ★ leadMs 填的是【整条链路的全部延迟】，不是"程序测不到的那部分"。
//   预测只用这一个来源，程序不再自动往里加任何东西 ——
//   来源单一，用户填多少就是多少，不会出现重复计算。
//
// 为什么会从"速度上限 + 距离上限"两道闸：速度上限管"速度估计可不可信"，
// 距离上限管"最终能推多远"，两者管的是不同的东西，不允许互相顶替。
struct PredictorConfig
{
    // 预测提前时间（毫秒）= 整条链路的全部延迟。
    // 用户自己测量后填入（包含采集、推理、瞄准、下发、以及游戏渲染内部延迟）。
    // ★ 0 = 关闭（默认）。
    double leadMs = 0.0;

    // 速度上限（像素/秒）。0 = 不限制。
    double maxVelocityPxPerSec = 0.0;

    // 预测距离上限，单位是目标框对角线倍数。0 = 不限制。
    double maxLeadRatio = 0.0;

    bool enabled() const { return leadMs != 0.0; }
};

// 一拍预测的完整产物，同时供控制层与日志使用。
struct PredictorResult
{
    // 本拍是否真的做了预测。false 时 predictedCenter 等于传入的 filteredCenter，
    // 也就是行为退化成"没有这个功能"。
    bool applied = false;

    // 预测前的中心（滤波器输出）与预测后的中心。
    Vec2 center{};
    Vec2 predictedCenter{};

    // 实际推进的位移（predictedCenter - center）。
    Vec2 lead{};

    // 本拍实际使用的提前时间（秒）= leadMs / 1000。
    double leadSec = 0.0;

    // 本拍滤波器的原始速度估计（像素/秒），以及钳制后的速度。
    Vec2 rawVelocity{};
    Vec2 clampedVelocity{};

    // 是否触发了速度钳制 / 距离钳制。
    bool velocityClamped = false;
    bool leadClamped = false;

    // 本拍距离上限（像素）。0 表示不限制。仅在 applied 时有意义。
    double maxLeadPx = 0.0;

    // 为什么这一拍没有预测（便于日志与排查）。
    enum class IdleReason
    {
        None = 0,
        Disabled,        // leadMs == 0，功能关闭
        NotInitialized,  // 滤波器还没建立速度估计
    };
    IdleReason idleReason = IdleReason::None;
};

// 预测器内部状态。目前无需跨帧保存任何东西 ——
// 提前时间完全来自配置，没有"上一帧延迟"这类需要沿用的数据。
// 保留该类型是为了让调用方（AimController）的 reset 语义保持统一，
// 将来若要加平滑/自适应也有落点。
struct PredictorState
{
    void reset() {}
};

// 预测瞄点。
// ★ 提前时间完全以 cfg.leadMs 为准，不做任何自动累加 ——
//   用户填的就是整条链路的全部延迟。
PredictorResult predictAnchor(const Vec2& filteredCenter,
                              const Vec2& velocityPxPerSec,
                              const Box& targetBox,
                              const PredictorConfig& cfg,
                              PredictorState& state);

}
