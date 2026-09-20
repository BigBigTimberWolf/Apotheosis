#include "pid_controller.h"

#include <algorithm>
#include <cmath>

namespace control {

double stabilityRatio(double kp, double dtSec)
{
    if (kCriticalGain <= 0.0 || dtSec <= 0.0)
        return 0.0;
    const double g = kp * dtSec * kCountsPerPixel;
    return g / kCriticalGain;
}

namespace {

double saturateForP(double e, double fullScale)
{
    if (fullScale <= 0.0)
        return e;
    return std::clamp(e, -fullScale, fullScale);
}

}

double PidController::stepAxis(Axis axis, double error, double dtSec,
                               AxisState& st, AxisTelemetry& tm, bool& unwound)
{
    unwound = false;

    // ── Smith 在途自身位移补偿 (一帧拉枪) ──────────────────────────────────
    // 画面是 deadTime 毫秒前拍的。在这段时间里发出的鼠标计数已经到达游戏，
    // 但画面还没显现出来。真实误差 = 看到的目标误差 - 扣除这批位移。
    double compensatedError = error;
    if (cfg_.kPxPerCount > 0.0 && cfg_.deadTimeMs > 0.0)
    {
        const double windowSec = cfg_.deadTimeMs * 0.001;
        const double pendingCounts = st.inFlightCounts(windowSec);
        const double pendingPx = pendingCounts * cfg_.kPxPerCount * cfg_.inflightBeta;
        compensatedError = error - pendingPx;
    }

    const double kp = (axis == Axis::X) ? cfg_.kpX : cfg_.kpY;
    const double ki = (axis == Axis::X) ? cfg_.kiX : cfg_.kiY;
    const double kd = (axis == Axis::X) ? cfg_.kdX : cfg_.kdY;

    if (st.hasPrev && compensatedError * st.prevError < 0.0)
    {
        const double decay = std::exp(-dtSec / std::max(cfg_.tauUnwindSec, 1e-6));
        st.integral *= decay;
        unwound = true;
    }

    if (ki != 0.0)
    {
        st.integral += ki * compensatedError * dtSec;

        const double iMax = (cfg_.iMax > 0.0)
            ? cfg_.iMax
            : std::max(1.0, static_cast<double>(cfg_.maxOutputCounts));
        st.integral = std::clamp(st.integral, -iMax, iMax);
    }
    else
    {
        st.integral = 0.0;
    }

    double deriv = 0.0;
    if (kd != 0.0)
    {
        const double de = compensatedError - st.prevError;
        if (cfg_.tauDerivSec > 0.0 && st.hasPrev)
        {
            const double a = 1.0 - std::exp(-dtSec / cfg_.tauDerivSec);
            st.derivLp += (de - st.derivLp) * a;
        }
        else
        {
            st.derivLp = de;
        }
        deriv = st.derivLp / dtSec;
    }
    else
    {
        st.derivLp = 0.0;
    }

    const double p = saturateForP(compensatedError, cfg_.pFullScalePx);
    const double u = dtSec * kp * (p + st.integral + kd * deriv);

    const double limited = std::clamp(
        u,
        -static_cast<double>(cfg_.maxOutputCounts),
        static_cast<double>(cfg_.maxOutputCounts));

    const double withCarry = limited + st.carry;
    const double rounded = std::round(withCarry);
    const double iMaxOut = static_cast<double>(cfg_.maxOutputCounts);
    const double countsClamped = std::clamp(rounded, -iMaxOut, iMaxOut);

    st.carry = withCarry - countsClamped;

    // 记录本拍发出的 counts 和 dt，供给后续拍做在途折算
    const int finalCounts = static_cast<int>(countsClamped);
    st.recordCount(finalCounts, dtSec);

    st.prevError = compensatedError;
    st.hasPrev = true;

    tm.error = compensatedError;
    tm.p = p;
    tm.i = st.integral;
    tm.d = kd * deriv;
    tm.u = u;
    tm.counts = finalCounts;
    tm.carry = st.carry;
    tm.stabilityRatio = stabilityRatio(kp, dtSec);

    return u;
}

Counts PidController::update(const Vec2& anchor, const Vec2& cross, double dtSec)
{
    Counts out;

    if (!(dtSec > 0.0))
        return out;

    const double eX = anchor.x - cross.x;
    const double eY = anchor.y - cross.y;

    bool uwX = false, uwY = false;
    stepAxis(Axis::X, eX, dtSec, stateX_, telemetry_.x, uwX);
    stepAxis(Axis::Y, eY, dtSec, stateY_, telemetry_.y, uwY);
    telemetry_.unwoundX = uwX;
    telemetry_.unwoundY = uwY;

    out.x = telemetry_.x.counts;
    out.y = telemetry_.y.counts;
    return out;
}

void PidController::reset()
{
    stateX_.reset();
    stateY_.reset();
    telemetry_ = ControlTelemetry{};
}

}
