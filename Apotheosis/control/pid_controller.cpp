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

    const double kp = (axis == Axis::X) ? cfg_.kpX : cfg_.kpY;
    const double ki = (axis == Axis::X) ? cfg_.kiX : cfg_.kiY;
    const double kd = (axis == Axis::X) ? cfg_.kdX : cfg_.kdY;

    if (st.hasPrev && error * st.prevError < 0.0)
    {
        const double decay = std::exp(-dtSec / std::max(cfg_.tauUnwindSec, 1e-6));
        st.integral *= decay;
        unwound = true;
    }

    if (ki != 0.0)
    {
        st.integral += ki * error * dtSec;

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
        if (!st.hasPrev)
        {
            // 首拍没有前一帧误差，不能把当前误差当作速度，否则 Kd 会突跳。
            st.derivLp = 0.0;
        }
        else if (cfg_.tauDerivSec > 0.0)
        {
            const double de = error - st.prevError;
            const double a = 1.0 - std::exp(-dtSec / cfg_.tauDerivSec);
            st.derivLp += (de - st.derivLp) * a;
        }
        else
        {
            st.derivLp = error - st.prevError;
        }
        deriv = st.derivLp / dtSec;
    }
    else
    {
        st.derivLp = 0.0;
    }

    const double p = saturateForP(error, cfg_.pFullScalePx);
    const double uRaw = dtSec * kp * (p + st.integral + kd * deriv);

    // ── Smith 在途自身位移补偿：只作用在【输出】上，不碰上面的误差/积分/微分 ──
    // ★ 这是与历史 bug 版本的关键区别：积分/微分/P 项必须吃原始 error，不能吃
    //   补偿后的值——否则积分一旦被压小就再也不累积，回路会停在一个随 beta
    //   线性增长的假平衡点上（稳态瞄偏）。
    double inflight = 0.0;
    if (cfg_.inflightBeta > 0.0 && cfg_.deadTimeMs > 0.0)
    {
        const double windowSec = cfg_.deadTimeMs * 0.001;
        inflight = cfg_.inflightBeta * st.inFlightCountsPerTick(windowSec, dtSec);
    }
    const double u = uRaw - inflight;

    const double limited = std::clamp(
        u,
        -static_cast<double>(cfg_.maxOutputCounts),
        static_cast<double>(cfg_.maxOutputCounts));

    const double withCarry = limited + st.carry;
    const double rounded = std::round(withCarry);
    const double iMaxOut = static_cast<double>(cfg_.maxOutputCounts);
    const double countsClamped = std::clamp(rounded, -iMaxOut, iMaxOut);

    st.carry = withCarry - countsClamped;

    const int finalCounts = static_cast<int>(countsClamped);

    st.prevError = error;
    st.hasPrev = true;

    tm.error = error;
    tm.p = p;
    tm.i = st.integral;
    tm.d = kd * deriv;
    tm.u = u;
    tm.counts = finalCounts;
    tm.carry = st.carry;
    tm.stabilityRatio = stabilityRatio(kp, dtSec);
    tm.inflight = inflight;

    return u;
}

void PidController::observeSentCounts(Counts sentCounts, double dtSec)
{
    if (!(dtSec > 0.0)) return;
    stateX_.recordCount(sentCounts.x, dtSec);
    stateY_.recordCount(sentCounts.y, dtSec);
}

Counts PidController::update(const Vec2& anchor, const Vec2& cross, double dtSec)
{
    Counts out;

    if (!(dtSec > 0.0))
        return out;

    const double eX = anchor.x - cross.x;
    const double eY = anchor.y - cross.y;

    if (cfg_.settleEnterPx > 0.0)
    {
        const double distance = std::hypot(eX, eY);
        const double exitPx = std::max(cfg_.settleExitPx, cfg_.settleEnterPx);
        if (settled_)
        {
            if (distance >= exitPx)
            {
                settled_ = false;
                nearTimeSec_ = 0.0;
            }
        }
        else if (distance <= cfg_.settleEnterPx)
        {
            // 单次卡顿不能冒充连续观察；50ms 是独立的停稳消抖时间。
            nearTimeSec_ += std::min(dtSec, 0.020);
            if (nearTimeSec_ >= cfg_.settleDwellSec)
                settled_ = true;
        }
        else if (distance < exitPx)
        {
            // 短暂测量抖动只回退部分观察时间，不让一帧噪声反复清零。
            nearTimeSec_ = std::max(0.0, nearTimeSec_ - std::min(dtSec, 0.020) * 0.5);
        }
        else
        {
            nearTimeSec_ = 0.0;
        }
    }
    else
    {
        settled_ = false;
        nearTimeSec_ = 0.0;
    }

    if (settled_)
    {
        stateX_.hold(eX);
        stateY_.hold(eY);
        telemetry_ = ControlTelemetry{};
        telemetry_.x.error = eX;
        telemetry_.y.error = eY;
        telemetry_.settled = true;
        return out;
    }

    bool uwX = false, uwY = false;
    stepAxis(Axis::X, eX, dtSec, stateX_, telemetry_.x, uwX);
    stepAxis(Axis::Y, eY, dtSec, stateY_, telemetry_.y, uwY);
    telemetry_.unwoundX = uwX;
    telemetry_.unwoundY = uwY;
    telemetry_.settled = false;

    out.x = telemetry_.x.counts;
    out.y = telemetry_.y.counts;
    return out;
}

void PidController::reset()
{
    stateX_.reset();
    stateY_.reset();
    telemetry_ = ControlTelemetry{};
    settled_ = false;
    nearTimeSec_ = 0.0;
}

}
