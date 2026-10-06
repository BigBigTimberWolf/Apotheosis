#pragma once

#include "types.h"

#include <algorithm>
#include <cmath>

namespace control {

// Companion of the Smith Predictor that sits in the PID error.
//
// Smith removes every pixel of motion that was already sent but has not shown up
// in the image yet, so the PID sees the estimated current error instead of the
// delayed one. That is exact for a still target. A target that is itself moving
// covers about `velocity x delay` during the same delay, though; removing all of
// the in-flight motion then leaves the crosshair trailing by that much and
// cancels the visible extrapolation.
//
// This hands back only the part of the in-flight motion that the target's own
// movement explains. Per axis:
//     lead = min(|slow in-flight|, |in-flight now|, |filtered velocity| x delay)
// signed like the in-flight motion, and only while the filtered velocity, the
// slowly filtered in-flight motion and the in-flight motion now all point the
// same way. A still target, a noisy velocity reading, an acquisition burst
// (in-flight motion that has not persisted) and a target moving away from the
// motion we send all get no lead, so Smith keeps its full correction there.
//
// The controller adds the lead to the control anchor, so it is also the
// extrapolated aim point the preview draws. Nothing here is user-tunable.
class SmithLead
{
public:
    // velocity: tracked target velocity (px/s, self motion removed).
    // pending: motion sent but not yet visible in the image (px).
    // delaySec: calibrated send-to-image lag; <= 0 means Smith is not active.
    Vec2 update(Vec2 velocity, Vec2 pending, double delaySec, double dtSec)
    {
        if (!(delaySec > 0.0) || !(dtSec > 0.0) || !std::isfinite(delaySec) ||
            !std::isfinite(dtSec) || !finite(velocity) || !finite(pending)) {
            reset();
            return {};
        }
        if (!ready_) {
            // A new target (or a restart) must not inherit earlier motion: the
            // velocity starts at the current reading, the in-flight history at 0.
            velocity_ = velocity;
            pending_ = {};
            subtract_ = pending; // 首次直接取当前值，避免启动时凭空多一个台阶
            ready_ = true;
        }
        const double velocityAlpha = -std::expm1(-dtSec / kVelocityTauSec);
        const double pendingAlpha = -std::expm1(-dtSec / kPendingTauSec);
        const double subtractAlpha = -std::expm1(-dtSec / kSubtractTauSec);
        velocity_ += (velocity - velocity_) * velocityAlpha;
        pending_ += (pending - pending_) * pendingAlpha;
        subtract_ += (pending - subtract_) * subtractAlpha;
        return {axis(velocity_.x, pending_.x, pending.x, delaySec),
                axis(velocity_.y, pending_.y, pending.y, delaySec)};
    }

    // 给 PID 误差减掉的那份"在飞位移"，按发送事件在时间上摊开。
    //
    // ★ 原始 pending 是按事件整块跳变的：一次成功发送就是一个事件、最多
    //   smoothMaxPixel（默认 50 px），它进出延迟窗口时 PID 误差就整块跳一下 ——
    //   而准星死区只有 5 px，这一跳是死区的十倍。标定出来的延迟是常量，实际的
    //   发送→画面延迟却在抖（帧间隔、采集、推理），窗口边界来回扫，事件就整笔进出
    //   → 这就是"延迟一波动就不稳"。近距离目标要瞄的点落在大框里、离准星天然就远，
    //   误差常打到限幅、每次发送都是最大笔，所以只有近目标看得见抖。
    //
    //   这里用与延迟同量级的时间常数把台阶摊平；直流增益仍是 1，平均修正量不变，
    //   所以不会因为这一改变得更容易过冲。未标定（delaySec <= 0）时整体 reset 并
    //   返回 0，与 pendingCorrection() 在 delayMs < 0 时不产生值的契约一致。
    Vec2 smoothedPending() const { return subtract_; }

    void reset() {
        velocity_ = {};
        pending_ = {};
        subtract_ = {};
        ready_ = false;
    }

private:
    // Velocity smoothing is short (the tracker already filters it); the in-flight
    // history is longer so a one-off burst is not mistaken for target motion.
    static constexpr double kVelocityTauSec = 0.05;
    static constexpr double kPendingTauSec = 0.12;
    // 减掉用的那份跟延迟同量级：既摊平事件台阶，又不会比真实在飞量滞后太多。
    static constexpr double kSubtractTauSec = 0.04;

    static bool finite(const Vec2& v) { return std::isfinite(v.x) && std::isfinite(v.y); }

    static double axis(double velocity, double slow, double now, double delaySec) {
        if (slow * now <= 0.0 || velocity * slow <= 0.0) return 0.0;
        const double magnitude = std::min({std::abs(slow), std::abs(now),
                                           std::abs(velocity) * delaySec});
        return slow > 0.0 ? magnitude : -magnitude;
    }

    Vec2 velocity_{}, pending_{}, subtract_{};
    bool ready_ = false;
};

} // namespace control
