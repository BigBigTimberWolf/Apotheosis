// Smith lead: Smith keeps damping a still target, but a moving target is no longer
// trailed by velocity x delay and the extrapolated aim point (controlAnchor - anchor)
// is visible again. Part 1 pins the SmithLead rule itself, part 2 closes the loop with
// the real controller and the real send-feedback window against a delayed toy plant.
#include "control/recovered_aim_controller.h"
#include "control/smith_lead.h"
#include "runtime/motion_feedback_window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using namespace control;

namespace {

int failures = 0;
void check(bool pass, const char* name)
{
    if (!pass) { ++failures; std::printf("FAIL: %s\n", name); }
}

// ---- part 1: the rule ----------------------------------------------------------------

constexpr double kDelaySec = 0.06;

Vec2 settle(SmithLead& lead, Vec2 velocity, Vec2 pending, int frames, double dt = 0.005)
{
    Vec2 out;
    for (int n = 0; n < frames; ++n) out = lead.update(velocity, pending, kDelaySec, dt);
    return out;
}

void ruleTests()
{
    {
        SmithLead lead;
        const Vec2 out = settle(lead, {0, 0}, {20, -8}, 200);
        check(out.x == 0.0 && out.y == 0.0, "still target gets no lead however much is in flight");
    }
    {
        SmithLead lead;
        const Vec2 out = settle(lead, {240, -120}, {14.4, -7.2}, 400);
        check(std::abs(out.x - 14.4) < 0.05 && std::abs(out.y + 7.2) < 0.05,
              "steady tracking hands back velocity x delay per axis");
        const Vec2 drained = lead.update({240, -120}, {3, -1}, kDelaySec, 0.005);
        check(drained.x > 0.0 && drained.x <= 3.0 && drained.y < 0.0 && drained.y >= -1.0,
              "the lead never exceeds the motion that is in flight right now");
    }
    // 在飞位移按发送事件整块跳变：一次成功发送最多 smoothMaxPixel（默认 50 px），
    // 标定延迟与实际延迟之间的抖动会让窗口边界扫过事件，于是它整笔进/出。减掉的那
    // 份必须摊平，否则 PID 误差跟着跳台阶（准星死区 5 px，这一跳 50 px）。
    {
        SmithLead lead;
        lead.update({0, 0}, {0, 0}, kDelaySec, 0.008);
        double previous = lead.smoothedPending().x;
        double maxStep = 0.0, firstStep = 0.0;
        for (int n = 0; n < 400; ++n) {
            lead.update({0, 0}, {50, 0}, kDelaySec, 0.008);
            const double now = lead.smoothedPending().x;
            const double step = std::abs(now - previous);
            if (n == 0) firstStep = step;
            maxStep = std::max(maxStep, step);
            previous = now;
        }
        check(firstStep > 0.0 && maxStep < 50.0 * 0.35,
              "a whole-event step in flight is spread over time, not applied in one frame");
        check(std::abs(previous - 50.0) < 0.5,
              "spreading keeps unity DC gain: the average correction is unchanged");
    }
    {
        SmithLead lead;
        const Vec2 first = lead.update({240, 0}, {30, 0}, kDelaySec, 0.005);
        check(first.x > 0.0 && first.x < 2.0,
              "an acquisition burst is not mistaken for target motion on the first frame");
        const Vec2 early = settle(lead, {240, 0}, {30, 0}, 19);
        check(early.x > first.x && early.x < 14.4,
              "the lead builds up gradually while the in-flight motion persists");
        const Vec2 late = settle(lead, {240, 0}, {30, 0}, 400);
        check(std::abs(late.x - 14.4) < 0.05, "a long burst is capped by what the target's speed explains");
    }
    {
        SmithLead lead;
        const Vec2 out = settle(lead, {-240, 240}, {14.4, -14.4}, 400);
        check(out.x == 0.0 && out.y == 0.0,
              "a target moving against the motion we send gets no lead");
    }
    {
        SmithLead lead;
        const Vec2 out = settle(lead, {100, 0}, {40, 0}, 400);
        check(std::abs(out.x - 6.0) < 0.01, "a slow target caps the lead at velocity x delay");
    }
    {
        SmithLead lead;
        const Vec2 out = settle(lead, {240, 0}, {14.4, 14.4}, 400);
        check(std::abs(out.x - 14.4) < 0.05 && out.y == 0.0, "axes are judged independently");
    }
    {
        SmithLead lead;
        settle(lead, {240, 0}, {14.4, 0}, 400);
        const double nan = std::nan("");
        check(lead.update({240, 0}, {14.4, 0}, 0.0, 0.005).x == 0.0, "no delay calibration, no lead");
        settle(lead, {240, 0}, {14.4, 0}, 400);
        check(lead.update({240, 0}, {14.4, 0}, -1.0, 0.005).x == 0.0, "negative delay, no lead");
        settle(lead, {240, 0}, {14.4, 0}, 400);
        check(lead.update({nan, 0}, {14.4, 0}, kDelaySec, 0.005).x == 0.0, "non-finite velocity, no lead");
        settle(lead, {240, 0}, {14.4, 0}, 400);
        check(lead.update({240, 0}, {nan, 0}, kDelaySec, 0.005).x == 0.0, "non-finite in-flight, no lead");
        settle(lead, {240, 0}, {14.4, 0}, 400);
        check(lead.update({240, 0}, {14.4, 0}, kDelaySec, 0.0).x == 0.0, "non-positive dt, no lead");
        const Vec2 restarted = lead.update({240, 0}, {14.4, 0}, kDelaySec, 0.005);
        check(restarted.x > 0.0 && restarted.x < 1.0,
              "after an invalid frame the in-flight history restarts from zero");
    }
    {
        SmithLead lead;
        settle(lead, {240, 0}, {14.4, 0}, 400);
        lead.reset();
        const Vec2 restarted = lead.update({240, 0}, {14.4, 0}, kDelaySec, 0.005);
        check(restarted.x > 0.0 && restarted.x < 1.0, "reset forgets the earlier target's history");
    }
}

// ---- part 2: closed loop ---------------------------------------------------------------

constexpr double kPpc = 0.91;
constexpr double kSendToEffectSec = 0.0015;

// Image error seen at time t = target(t) - everything sent before t (pure delay plant).
class Plant
{
public:
    void add(double timeSec, double px)
    {
        times_.push_back(timeSec);
        totals_.push_back((totals_.empty() ? 0.0 : totals_.back()) + px);
    }
    double view(double timeSec) const
    {
        const auto it = std::upper_bound(times_.begin(), times_.end(), timeSec);
        return it == times_.begin() ? 0.0 : totals_[static_cast<size_t>(it - times_.begin()) - 1];
    }

private:
    std::vector<double> times_, totals_;
};

struct SimConfig
{
    bool smith = true;                // feed pendingMotionPx
    double lagMs = 60.0;              // true send-to-image lag
    double configuredDelayMs = 60.0;  // what the PID is told; -1 = not calibrated
    float kp = 0.2f;
    double fps = 144.0;
    double predictionMs = 0.0;        // macro extrapolation
    double staleAtSec = -1.0;         // one frame without a fresh detection
};

struct Trace
{
    std::vector<double> time;
    std::vector<double> error;  // true target - crosshair right now (positive = trailing)
    std::vector<double> lead;   // controlAnchor - anchor (follow compensation is off)
};

Trace simulate(const std::function<double(double)>& target, double seconds, const SimConfig& cfg)
{
    ControllerConfig config;
    config.frameWidth = config.frameHeight = 512;
    config.fovWidth = config.fovHeight = 512;
    config.buckets.byClassId = {Bucket::Aim};
    RecoveredPidConfig pid;
    pid.kpX = pid.kpY = cfg.kp;
    pid.kdX = pid.kdY = 0.0f;
    pid.feedforwardX = pid.feedforwardY = static_cast<float>(3.0 / (kPpc * cfg.fps));
    pid.motionPixelsPerCountX = pid.motionPixelsPerCountY = static_cast<float>(kPpc);
    pid.motionDelayMs = static_cast<float>(cfg.configuredDelayMs);
    RecoveredAimController controller;
    controller.setConfig(config, pid);

    runtime::MotionFeedbackWindow feedback;
    Plant plant;
    const double dt = 1.0 / cfg.fps;
    const Vec2 cross{256, 293};
    Trace trace;
    for (int k = 0; k < static_cast<int>(seconds * cfg.fps); ++k) {
        const double now = (k + 1) * dt;
        const double seen = now - cfg.lagMs * 1e-3;
        const Vec2 center = cross + Vec2{target(seen) - plant.view(seen), 0.0};
        ControlInput in;
        in.cross = cross;
        in.dtSec = in.trackingDtSec = dt;
        in.candidates = {Candidate{{center.x - 11, center.y - 60, 22, 123}, 0, 0.9}};
        in.detectionFresh = in.crosshairFresh = true;
        in.frameIndex = static_cast<uint64_t>(k + 1);
        in.observationTimeUs = static_cast<int64_t>(now * 1e6);
        in.macro.predictionMs = cfg.predictionMs;
        in.motionEventSum = feedback.sample(in.observationTimeUs, true, cfg.configuredDelayMs);
        if (cfg.smith)
            in.pendingMotionPx = feedback.pendingCorrection(in.observationTimeUs,
                                                             cfg.configuredDelayMs, kPpc, kPpc);
        if (cfg.staleAtSec >= 0.0 && std::abs(now - cfg.staleAtSec) < 0.5 * dt)
            in.detectionFresh = false;
        const ControlOutput out = controller.update(in);
        if (out.engaged && (out.counts.x != 0 || out.counts.y != 0)) {
            const double effective = now + kSendToEffectSec;
            plant.add(effective, out.counts.x * kPpc);
            feedback.add({out.counts.x, out.counts.y, static_cast<int64_t>(effective * 1e6), 0});
        }
        trace.time.push_back(now);
        trace.error.push_back(target(now) - plant.view(now));
        trace.lead.push_back(out.engaged ? out.controlAnchor.x - out.anchor.x : 0.0);
    }
    return trace;
}

double meanOver(const std::vector<double>& values, const Trace& trace, double from, double to)
{
    double sum = 0.0;
    int count = 0;
    for (size_t i = 0; i < values.size(); ++i)
        if (trace.time[i] >= from && trace.time[i] <= to) { sum += values[i]; ++count; }
    return count ? sum / count : 0.0;
}

double maxAbs(const std::vector<double>& values)
{
    double result = 0.0;
    for (double value : values) result = std::max(result, std::abs(value));
    return result;
}

// How far the crosshair went past the target (error < 0 means it is ahead of it).
double overshoot(const Trace& trace, double from = 0.0)
{
    double worst = 0.0;
    for (size_t i = 0; i < trace.error.size(); ++i)
        if (trace.time[i] >= from) worst = std::max(worst, -trace.error[i]);
    return worst;
}

double ramp(double t) { return 20.0 + 240.0 * t; }
double still(double) { return 40.0; }

void closedLoopTests()
{
    // A target moving at 240 px/s: Smith used to leave the crosshair about 17 px behind
    // (velocity x delay) and the extrapolated point on top of the aim point.
    {
        const Trace with = simulate(ramp, 6.0, {});
        SimConfig off;
        off.smith = false;
        const Trace without = simulate(ramp, 6.0, off);
        const double lag = meanOver(with.error, with, 3.0, 6.0);
        const double lagOff = meanOver(without.error, without, 3.0, 6.0);
        const double lead = meanOver(with.lead, with, 3.0, 6.0);
        std::printf("smith lead: ramp 240 px/s lag %.1f px (smith off %.1f), lead %.1f px\n",
                    lag, lagOff, lead);
        check(lag < 7.0, "a moving target is no longer trailed by velocity x delay");
        check(std::abs(lag - lagOff) < 4.0, "tracking a moving target is as close as without Smith");
        check(lead > 9.0 && lead < 16.0, "the extrapolated aim point is visible again (about velocity x delay)");
    }
    // A still target keeps Smith's damping: no lead, and a high Kp still does not run past it.
    {
        SimConfig high;
        high.kp = 0.4f;
        const Trace with = simulate(still, 3.0, high);
        high.smith = false;
        const Trace without = simulate(still, 3.0, high);
        std::printf("smith lead: still target overshoot %.1f px (smith off %.1f), max lead %.2f px\n",
                    overshoot(with), overshoot(without), maxAbs(with.lead));
        check(maxAbs(with.lead) < 2.0, "a still target gets no extrapolation");
        check(overshoot(with) < 8.0, "Smith still stops a high-gain loop from running past a still target");
        check(overshoot(with) * 2.0 < overshoot(without), "...clearly better than without Smith");
    }
    // Without a calibrated delay Smith is inactive and so is the lead.
    {
        SimConfig legacy;
        legacy.smith = false;
        legacy.configuredDelayMs = -1.0;
        const Trace trace = simulate(ramp, 4.0, legacy);
        check(maxAbs(trace.lead) == 0.0, "uncalibrated delay: the control anchor is untouched");
    }
    // The macro prediction keeps its meaning: it moves the aim point by the same amount
    // with Smith on as with Smith off (the lead cancels what Smith subtracts).
    {
        SimConfig macro;
        macro.predictionMs = 50.0;
        const Trace with = simulate(ramp, 6.0, macro);
        macro.smith = false;
        const Trace without = simulate(ramp, 6.0, macro);
        const double ahead = meanOver(with.error, with, 3.0, 6.0);
        const double aheadOff = meanOver(without.error, without, 3.0, 6.0);
        std::printf("smith lead: macro 50 ms -> error %.1f px (smith off %.1f)\n", ahead, aheadOff);
        check(ahead < -8.0 && ahead > -16.0, "macro prediction still puts the crosshair ahead by about velocity x time");
        check(std::abs(ahead - aheadOff) < 4.0, "macro prediction lands in the same place with Smith on or off");
    }
    // The lead does not linger after the target stops. Tracking without lag means a sudden
    // stop is overshot by about velocity x delay, which is what the loop did before Smith.
    {
        const auto stops = [](double t) { return 20.0 + 240.0 * std::min(t, 2.0); };
        const Trace with = simulate(stops, 4.0, {});
        SimConfig off;
        off.smith = false;
        const Trace without = simulate(stops, 4.0, off);
        std::printf("smith lead: stop overshoot %.1f px (smith off %.1f), lead after stop %.2f px\n",
                    overshoot(with, 2.0), overshoot(without, 2.0), meanOver(with.lead, with, 3.0, 4.0));
        check(std::abs(meanOver(with.lead, with, 3.0, 4.0)) < 1.0, "the lead is gone once the target stands still");
        check(overshoot(with, 2.0) < overshoot(without, 2.0) + 6.0,
              "a sudden stop costs no more than it did before Smith");
    }
    // Losing the target restarts the in-flight history instead of resuming the old lead.
    {
        SimConfig lost;
        lost.staleAtSec = 4.0;
        const Trace trace = simulate(ramp, 5.0, lost);
        const double before = meanOver(trace.lead, trace, 3.9, 3.99);
        double justAfter = 1e9;
        for (size_t i = 0; i < trace.time.size(); ++i)
            if (trace.time[i] > 4.0 && trace.time[i] < 4.0 + 1.5 / 144.0)
                justAfter = std::min(justAfter, std::abs(trace.lead[i]));
        check(before > 9.0 && justAfter < 1.0, "a lost detection restarts the lead from zero");
    }
}

} // namespace

int main()
{
    ruleTests();
    closedLoopTests();
    std::printf("smith lead: %d failures\n", failures);
    return failures ? 1 : 0;
}
