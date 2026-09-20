#pragma once

#include "types.h"

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

    // Smith 在途自身位移补偿 (一帧拉枪)
    double kPxPerCount = 0.0;     // 0 = 关闭
    double inflightBeta = 0.8;    // 补偿阻尼 (默认 0.8)
    double deadTimeMs = 46.0;     // 死区时间 (ms)
};

struct AxisState
{
    double integral = 0.0;
    double carry = 0.0;
    double prevError = 0.0;
    double derivLp = 0.0;
    bool hasPrev = false;

    // 在途位移环形缓冲区 (记录最近发出的 counts 和 dt)
    static constexpr int kRingCap = 128;
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

    double inFlightCounts(double windowSec) const
    {
        if (windowSec <= 0.0 || ringCount == 0) return 0.0;
        double totalCounts = 0.0;
        double accumulatedTime = 0.0;
        for (int i = 0; i < ringCount; ++i)
        {
            int idx = (ringHead - 1 - i + kRingCap) % kRingCap;
            accumulatedTime += ring[idx].dt;
            totalCounts += ring[idx].counts;
            if (accumulatedTime >= windowSec) break;
        }
        return totalCounts;
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
};

struct ControlTelemetry
{
    AxisTelemetry x;
    AxisTelemetry y;
    bool unwoundX = false;
    bool unwoundY = false;
};

class PidController
{
public:
    PidController() = default;
    explicit PidController(const PidConfig& cfg) : cfg_(cfg) {}

    void setConfig(const PidConfig& cfg) { cfg_ = cfg; }
    const PidConfig& config() const { return cfg_; }

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
};

inline constexpr double kLoopDeadTimeMs = 46.0;
inline constexpr double kCriticalGain = 0.2602;
inline constexpr double kCountsPerPixel = 0.593;

double stabilityRatio(double kp, double dtSec);

}
