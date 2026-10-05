#pragma once

#include "trigger_fsm.h"
#include <limits>

namespace boss {

enum class AmFireMode { SmartClick = 0, Burst = 1, Continuous = 2, SmartContinuous = 3 };

inline int amDelay(int base, int random)
{
    base = std::max(0, base);
    random = std::max(0, random);
    // AM 1.0.30, 140079980: zero base bypasses the random generator.
    if (!base || !random) return base;
    const int high = static_cast<int>(std::min<int64_t>(
        static_cast<int64_t>(base) + random, std::numeric_limits<int>::max()));
    thread_local std::mt19937 rng{std::random_device{}()};
    return std::uniform_int_distribution<int>(std::max(0, base - random), high)(rng);
}

inline AmFireMode amFireMode(int configured, int legacyDuration)
{
    return static_cast<AmFireMode>(configured < 0
        ? (legacyDuration > 0 ? 0 : 3) : std::clamp(configured, 0, 3));
}

// Recovered from AM 1.0.30 140088130 and its frame-loop caller.
// Timing advances only on observations; no sleeping or repeated stale-frame firing.
//
// ★ 连点/智能连点额外带一轮“打点宽限”：这套状态机只在检测帧上推进（约
//   8–33 ms 一拍），一帧离区或者一帧丢框就 reset 的话，瞄准微调、检测框抖动
//   或短暂遮挡都会把整轮打点清空，然后首发等待重来 —— 实机表现就是“不开枪、
//   打两枪就断”，并且怎么改按下/冷却时间都感觉一样。
class AmTriggerFsm {
public:
    using Input = TriggerFsm::Input;
    using Action = TriggerFsm::Action;
    // 连点/智能连点：至少保留这么多毫秒的离区宽限（用户设置更大时按用户的）。
    static constexpr int kClickCycleGraceMs = 60;

    bool configure(AmFireMode mode, int grace)
    {
        const bool release = mode != mode_ && reset();
        mode_ = mode;
        grace_ = std::clamp(grace, 0, 1000);
        return release;
    }

    Action tick(const Input& in, bool /*legacyHold*/, int start, int duration,
                int interval, int /*switchCooldown*/, int random,
                int /*durationRandom*/, int /*intervalRandom*/,
                int preFire = 0, int /*prearmElapsed*/ = 0)
    {
        Action act;
        if (!in.prerequisite_ready) { act.release_left = reset(); return act; }
        if (!in.in_zone && !continuousActive()) {
            if (holding()) {
                // 持续/智能持续：宽限内保持按住，超时才松开。
                if (keepHoldingOnTargetLoss(in.now_ms, grace_)) return act;
                act.release_left = reset();
                return act;
            }
            if (in.now_ms - clickLostAt(in.now_ms) < clickGrace()) {
                if (phase_ == TriggerPhase::Pressed) {
                    // 离区就按下这一枪按时松开，但保留冷却与 started_。
                    act.release_left = true;
                    phase_ = TriggerPhase::Cooldown;
                    due_ = in.now_ms + std::max(minPulse(), amDelay(interval, random));
                }
                return act;
            }
            act.release_left = reset();
            return act;
        }
        lostAt_ = -1;
        clickLostAtMs_ = -1;
        if (in.now_ms < due_) return act;
        if (phase_ == TriggerPhase::Pressed) {
            if (holding()) return act;
            act.release_left = true;
            phase_ = TriggerPhase::Cooldown;
            due_ = in.now_ms + std::max(minPulse(), amDelay(interval, random));
            return act;
        }
        if (phase_ == TriggerPhase::Cooldown) {
            // AM clears its post-release flag in a separate observation.
            phase_ = TriggerPhase::Idle;
            return act;
        }
        if (!started_) {
            started_ = true;
            const int delay = mode_ == AmFireMode::SmartClick ? 0 : amDelay(start, random);
            // Explicit keyboard-stop timing is an extension of the host project.
            if (preFire > 0) act.prepare_fire = true;
            due_ = in.now_ms + std::max(delay, preFire);
            if (due_ > in.now_ms) { phase_ = TriggerPhase::Delay; return act; }
        }
        phase_ = TriggerPhase::Pressed;
        act.press_left = act.fired = true;
        due_ = holding() ? 0 : in.now_ms + std::max(minPulse(), amDelay(duration, random));
        return act;
    }

    bool keepHoldingOnTargetLoss(int64_t now, int grace)
    {
        if (!pressed() || !holding()) return false;
        if (mode_ == AmFireMode::Continuous) return true;
        if (grace <= 0) return false;
        if (lostAt_ < 0) lostAt_ = now;
        return now - lostAt_ < std::clamp(grace, 0, 1000);
    }

    // 连点/智能连点：目标整体消失时保留本轮打点状态（左键另走 releasePress），
    // 回来了不用重新等首发延迟。
    bool keepCycleOnTargetLoss(int64_t now, int /*grace*/)
    {
        if (holding() || !started_) return false;
        return now - clickLostAt(now) < clickGrace();
    }

    // 松开当前这一枪并进入冷却，但保留 started_。
    bool releasePress(int64_t now = 0, int interval = 0, int random = 0)
    {
        if (!pressed()) return false;
        phase_ = TriggerPhase::Cooldown;
        due_ = now + std::max(minPulse(), amDelay(interval, random));
        return true;
    }

    bool holdZoneOnBriefMiss(bool hit, int /*track*/, int64_t now, int grace)
    {
        if (hit) { lostAt_ = -1; clickLostAtMs_ = -1; return true; }
        // 连点：宽限内的离区仍算在区内（自动急停屏蔽与开镜不跟着抖），
        // 超过宽限就交回原逻辑。
        if (!holding()) return keepCycleOnTargetLoss(now, grace);
        return keepHoldingOnTargetLoss(now, grace);
    }

    bool reset()
    {
        const bool release = pressed();
        phase_ = TriggerPhase::Idle;
        started_ = false;
        due_ = 0;
        lostAt_ = -1;
        clickLostAtMs_ = -1;
        return release;
    }
    TriggerPhase phase() const { return phase_; }
    bool pressed() const { return phase_ == TriggerPhase::Pressed; }
    bool holding() const { return mode_ == AmFireMode::Continuous || mode_ == AmFireMode::SmartContinuous; }
    bool continuousActive() const { return mode_ == AmFireMode::Continuous && started_; }
    bool started() const { return started_; }

private:
    // Do not collapse configured durations/intervals through the old 10ms floor.
    int minPulse() const { return mode_ == AmFireMode::Burst ? 1 : 0; }
    int clickGrace() const { return std::max(grace_, kClickCycleGraceMs); }
    int64_t clickLostAt(int64_t now)
    {
        if (clickLostAtMs_ < 0) clickLostAtMs_ = now;
        return clickLostAtMs_;
    }
    AmFireMode mode_ = AmFireMode::SmartContinuous;
    TriggerPhase phase_ = TriggerPhase::Idle;
    bool started_ = false;
    int grace_ = 20;
    int64_t due_ = 0;
    int64_t lostAt_ = -1;
    int64_t clickLostAtMs_ = -1;
};

}

