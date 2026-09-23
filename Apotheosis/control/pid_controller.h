#pragma once

#include "types.h"

#include <algorithm>

namespace control {

struct PidConfig
{
    double kpX = 35.0;
    double kpY = 35.0;
    double kiX = 0.0;
    double kiY = 0.0;
    double kdX = 0.0;
    double kdY = 0.0;

    double tauUnwindSec = 0.030;

    double tauDerivSec = 0.020;

    double iMax = 0.0;

    int maxOutputCounts = 200;

    double pFullScalePx = 0.0;

    // 锚点附近停稳：连续接近后停止亚像素计数的往返结转；超过退出半径即恢复。
    // 50ms 是本策略的初版观察时间，不是实测链路延迟，也不读取下面的 Smith 窗口。
    // enter=0 可关闭，供对照测试与后续实机调参使用。
    double settleEnterPx = 0.75;
    double settleExitPx = 1.5;
    double settleDwellSec = 0.050;

    // ── 在途自身位移补偿 (Smith) ────────────────────────────────────────────
    // 把"已经发出去、游戏里已生效、只是画面还没回来"的自身位移从【输出】里扣掉，
    // 避免死区期间控制器反复对同一批位移重复下令导致的过冲/振荡。
    //
    // ★ 纯计数域运算：u -= inflightBeta * (窗口内下发 counts 之和 / 窗口拍数)。
    //   不需要任何计数<->像素换算（不引入 kp 或每计数像素 k̂）——两者相除后
    //   本就与像素无关，强行换算反而会隐含"kp = 1/k̂"这个错误假设。
    // ★ 必须除以窗口拍数：否则同一批位移会在窗口内被跨帧重复扣除，导致强度
    //   随检测帧率剧烈漂移（帧率越高，重复扣的次数越多）。
    // ★ inflightBeta = 0 时整段直接跳过，与没有这个功能逐位相同（安全默认）。
    double inflightBeta = 0.0;
    // 补偿窗口(ms)。默认 46ms 来自旧实机日志的模型反推，当前硬件需重新测量。
    // ★ 启用 Smith 时必须按真实链路死区设置，不能超过——超过会把已经生效的
    // 位移当成还在途、重复扣除、变成正反馈发散。
    double deadTimeMs = 46.0;
};

struct AxisState
{
    double integral = 0.0;
    double carry = 0.0;
    double prevError = 0.0;
    double derivLp = 0.0;
    bool hasPrev = false;

    // 在途位移环形缓冲区。运行时最短有效拍长为 1ms；1024 拍可覆盖界面允许的
    // 最长 1000ms 窗口，并留出少量帧率抖动余量。
    static constexpr int kRingCap = 1024;
    struct StepSample {
        int counts = 0;
        double dt = 0.0;
    };
    StepSample ring[kRingCap]{};
    int ringHead = 0;
    int ringCount = 0;

    void recordCount(int counts, double dt)
    {
        ring[ringHead] = { counts, dt };
        ringHead = (ringHead + 1) % kRingCap;
        if (ringCount < kRingCap) ringCount++;
    }

    // 窗口(windowSec 秒)内"已发出、尚未生效"的位移，按窗口的【理论拍数】
    // (windowSec / currentDtSec)归一化为"平均每拍在途多少 counts"。
    //
    // ★ 为什么除以"理论拍数"而不是"实际扫到的样本数": 历史不够长时(刚启动/
    //   刚复位/刚换目标), 用极少的样本去算均值会冒充"整窗平均", 导致第二拍
    //   就按满窗强度补偿, 造成过补偿的瞬态尖峰。除以理论拍数让窗口不够满时
    //   缺的那部分按 0 计入, 随历史积累逐渐爬升到满强度, 更保守也更符合"这
    //   段时间内平均每拍发了多少"的物理含义。
    // ★ 为什么还要按时间戳累加(而不是固定拍数): 检测帧率会抖动, 用真实记录
    //   的 dt 累加能让"扫到多少样本才算覆盖了这个窗口"不受个别拍长短影响。
    // ★ 这两者结合才是帧率无关的关键: 同一个 beta 在 60/120/240fps 下算出的
    //   在途量应保持一致比例, 不会随检测帧率线性放大或缩小。
    double inFlightCountsPerTick(double windowSec, double currentDtSec) const
    {
        if (windowSec <= 0.0 || ringCount == 0 || !(currentDtSec > 0.0)) return 0.0;
        double totalCounts = 0.0;
        double accumulatedTime = 0.0;
        for (int i = 0; i < ringCount; ++i)
        {
            int idx = (ringHead - 1 - i + kRingCap) % kRingCap;
            accumulatedTime += ring[idx].dt;
            totalCounts += ring[idx].counts;
            if (accumulatedTime >= windowSec) break;
        }
        const double windowTicks = std::max(1.0, windowSec / currentDtSec);
        return totalCounts / windowTicks;
    }

    void reset()
    {
        integral = 0.0;
        carry = 0.0;
        prevError = 0.0;
        derivLp = 0.0;
        hasPrev = false;
        ringHead = 0;
        ringCount = 0;
    }

    void hold(double error)
    {
        integral = 0.0;
        carry = 0.0;
        derivLp = 0.0;
        prevError = error;
        hasPrev = true;
        // 在途历史继续由每拍成功发送计数（停稳时通常是 0）推进时间，
        // 不能清空：进入停稳前的动作可能此刻才真正发送成功。
    }
};

struct AxisTelemetry
{
    double error = 0.0;
    double p = 0.0;
    double i = 0.0;
    double d = 0.0;
    double u = 0.0;
    int counts = 0;
    double carry = 0.0;
    double stabilityRatio = 0.0;
    double inflight = 0.0;  // 本拍从输出里扣掉的在途补偿量 (counts)，遥测用
};

struct ControlTelemetry
{
    AxisTelemetry x;
    AxisTelemetry y;
    bool unwoundX = false;
    bool unwoundY = false;
    bool settled = false;
};

class PidController
{
public:
    PidController() = default;
    explicit PidController(const PidConfig& cfg) : cfg_(cfg) {}

    void setConfig(const PidConfig& cfg) { cfg_ = cfg; }
    const PidConfig& config() const { return cfg_; }

    // 每个控制拍先记录自上拍以来驱动确认发送的计数，随后再计算本拍输出。
    void observeSentCounts(Counts sentCounts, double dtSec);
    Counts update(const Vec2& anchor, const Vec2& cross, double dtSec);

    void reset();

    const ControlTelemetry& telemetry() const { return telemetry_; }

private:
    double stepAxis(Axis axis, double error, double dtSec,
                    AxisState& st, AxisTelemetry& tm, bool& unwound);

    PidConfig cfg_;
    AxisState stateX_;
    AxisState stateY_;
    ControlTelemetry telemetry_;
    bool settled_ = false;
    double nearTimeSec_ = 0.0;
};

inline constexpr double kCriticalGain = 0.2602;
inline constexpr double kCountsPerPixel = 0.593;

double stabilityRatio(double kp, double dtSec);

}
