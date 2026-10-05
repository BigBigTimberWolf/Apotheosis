#ifndef MOUSE_TRIGGER_FSM_H
#define MOUSE_TRIGGER_FSM_H

#include <algorithm>
#include <cstdint>
#include <random>

namespace boss
{

enum class TriggerPhase { Idle, Delay, Pressed, Cooldown, SwitchCooldown };

class TriggerFsm
{
public:
    struct Input
    {
        bool   in_zone = false;
        bool   prerequisite_ready = true;
        int    track_id = -1;
        int64_t now_ms = 0;
    };

    struct Action
    {
        bool prepare_fire = false;
        bool press_left = false;
        bool release_left = false;
        bool fired = false;
    };

    Action tick(const Input& in, bool hold_mode,
                int fire_delay, int duration, int interval, int switch_cd,
                int delay_jitter, int duration_jitter, int interval_jitter,
                int pre_fire_ms = 0, int prearm_elapsed_ms = 0)
    {
        Action act;
        target_loss_since_ms_ = -1;
        const bool eligible = in.in_zone && in.prerequisite_ready;

        const bool changedTarget = in.track_id >= 0 &&
            last_fire_track_id_ >= 0 && in.track_id != last_fire_track_id_;
        if (changedTarget)
        {
            first_shot_pending_ = true;
            // Restart once for a real target change, then keep the pending
            // shot's timer. Repeated tracker-ID changes must not starve it.
            if (phase_ == TriggerPhase::Delay &&
                (!in.in_zone || !delay_retargeted_))
            {
                phase_ = TriggerPhase::Idle;
                in_zone_since_ms_ = -1;
                prefire_sent_ = false;
                delay_retargeted_ = in.in_zone;
            }
        }

        if (changedTarget &&
            shot_since_switch_ &&
            switch_cd > 0 &&
            phase_ != TriggerPhase::SwitchCooldown &&
            !in.in_zone)
        {
            if (phase_ == TriggerPhase::Pressed)
                act.release_left = true;
            phase_ = TriggerPhase::SwitchCooldown;
            phase_time_ms_ = in.now_ms;
            phase_target_ms_ = jitter(switch_cd, delay_jitter);
            in_zone_since_ms_ = -1;
            prefire_sent_ = false;
            shot_since_switch_ = false;
        }
        if (in.track_id >= 0) last_fire_track_id_ = in.track_id;

        switch (phase_)
        {
        case TriggerPhase::Idle:
            if (eligible)
                startShot(act, in.now_ms, hold_mode, fire_delay, duration,
                          delay_jitter, duration_jitter, pre_fire_ms, prearm_elapsed_ms);
            else
            {
                in_zone_since_ms_ = -1;
                prefire_sent_ = false;
            }
            break;

        case TriggerPhase::Delay:
            if (!eligible)
            {
                phase_ = TriggerPhase::Idle;
                in_zone_since_ms_ = -1;
                prefire_sent_ = false;
                delay_retargeted_ = false;
                break;
            }
            advanceDelay(act, in.now_ms, hold_mode, duration,
                         duration_jitter, pre_fire_ms);
            break;

        case TriggerPhase::Pressed:
            if (hold_mode)
            {
                if (!eligible)
                {
                    act.release_left = true;
                    phase_ = TriggerPhase::Cooldown;
                    phase_time_ms_ = in.now_ms;
                    phase_target_ms_ = jitter(interval, interval_jitter);
                }
                break;
            }
            if (in.now_ms - phase_time_ms_ >= phase_target_ms_)
            {
                act.release_left = true;
                phase_ = TriggerPhase::Cooldown;
                // Lua's interval is measured from last_trigger_time (shot
                // start), not from the end of the mouse click.
                phase_target_ms_ = jitter(interval, interval_jitter);
            }
            break;

        case TriggerPhase::Cooldown:
            if (in.now_ms - phase_time_ms_ >= phase_target_ms_)
            {
                phase_ = TriggerPhase::Idle;
                in_zone_since_ms_ = -1;
                delay_retargeted_ = false;
                if (eligible)
                    startShot(act, in.now_ms, hold_mode, fire_delay, duration,
                              delay_jitter, duration_jitter, pre_fire_ms, prearm_elapsed_ms);
            }
            break;

        case TriggerPhase::SwitchCooldown:
            if (in.now_ms - phase_time_ms_ >= phase_target_ms_)
            {
                phase_ = TriggerPhase::Idle;
                in_zone_since_ms_ = -1;
            }
            break;
        }

        return act;
    }

    // Only extend an existing sustained press. Repeated missing frames share
    // one deadline; normal target processing or reset clears it.
    bool keepHoldingOnTargetLoss(int64_t now_ms, int grace_ms)
    {
        if (!pressed() || !pressed_hold_ || grace_ms <= 0) return false;
        if (target_loss_since_ms_ < 0)
            target_loss_since_ms_ = hit_zone_loss_since_ms_ >= 0
                ? hit_zone_loss_since_ms_ : now_ms;
        return now_ms - target_loss_since_ms_ < std::clamp(grace_ms, 0, 2000);
    }

    // Keep a sustained press through a brief hit-zone miss on the same track.
    // The caller feeds the resulting zone to both scope and trigger state
    // machines, so a temporary miss cannot close the scope first.
    bool holdZoneOnBriefMiss(bool raw_in_zone, int track_id,
                             int64_t now_ms, int grace_ms)
    {
        if (raw_in_zone)
        {
            hit_zone_loss_since_ms_ = -1;
            return true;
        }
        if (!pressed() || !pressed_hold_ || grace_ms <= 0 ||
            track_id < 0 || track_id != last_fire_track_id_)
        {
            hit_zone_loss_since_ms_ = -1;
            return false;
        }
        if (hit_zone_loss_since_ms_ < 0)
            hit_zone_loss_since_ms_ = target_loss_since_ms_ >= 0
                ? target_loss_since_ms_ : now_ms;
        return now_ms - hit_zone_loss_since_ms_ < std::clamp(grace_ms, 0, 2000);
    }

    // 这套旧状态机没有连点宽限：丢目标就清空（AmTriggerFsm 才有）。
    // 两个方法只为满足 releaseOnTargetLoss 的模板接口而存在。
    bool keepCycleOnTargetLoss(int64_t, int) { return false; }
    bool releasePress(int64_t = 0, int = 0, int = 0) { return reset(); }

    bool reset()
    {
        const bool was_pressed = (phase_ == TriggerPhase::Pressed);
        phase_ = TriggerPhase::Idle;
        phase_time_ms_ = 0;
        phase_target_ms_ = 0;
        in_zone_since_ms_ = -1;
        prefire_sent_ = false;
        last_fire_track_id_ = -1;
        first_shot_pending_ = true;
        shot_since_switch_ = false;
        delay_retargeted_ = false;
        target_loss_since_ms_ = -1;
        hit_zone_loss_since_ms_ = -1;
        pressed_hold_ = false;
        return was_pressed;
    }

    TriggerPhase phase() const { return phase_; }
    bool pressed() const { return phase_ == TriggerPhase::Pressed; }

    static bool inHitZone(double judge_x, double judge_y,
                          double box_x, double box_y, double box_w, double box_h,
                          int y_percent,
                          double* half_x_out = nullptr, double* half_y_out = nullptr)
    {
        const double s = std::max(0.1, y_percent / 100.0);
        const double half_x = box_w * s * 0.5;
        const double half_y = box_h * s * 0.5;
        const double box_cx = box_x + box_w * 0.5;
        const double box_cy = box_y + box_h * 0.5;
        if (half_x_out) *half_x_out = half_x;
        if (half_y_out) *half_y_out = half_y;
        return std::abs(judge_x - box_cx) <= half_x &&
               std::abs(judge_y - box_cy) <= half_y;
    }

private:
    void startShot(Action& act, int64_t now_ms, bool hold_mode,
                   int fire_delay, int duration, int delay_jitter,
                   int duration_jitter, int pre_fire_ms, int prearm_elapsed_ms)
    {
        const int target_delay = first_shot_pending_
            ? std::max(0, jitter(fire_delay, delay_jitter) -
                          std::max(0, prearm_elapsed_ms)) : 0;
        if (target_delay <= 0 && pre_fire_ms <= 0)
        {
            beginFire(act, now_ms, hold_mode, duration, duration_jitter);
            return;
        }
        in_zone_since_ms_ = now_ms;
        phase_ = TriggerPhase::Delay;
        phase_time_ms_ = now_ms;
        phase_target_ms_ = std::max(target_delay, pre_fire_ms);
        prefire_sent_ = false;
        advanceDelay(act, now_ms, hold_mode, duration,
                     duration_jitter, pre_fire_ms);
    }

    void advanceDelay(Action& act, int64_t now_ms, bool hold_mode,
                      int duration, int duration_jitter, int pre_fire_ms)
    {
        const int64_t due_ms = phase_time_ms_ + phase_target_ms_;
        if (pre_fire_ms > 0 && !prefire_sent_ && now_ms >= due_ms - pre_fire_ms)
        {
            act.prepare_fire = true;
            prefire_sent_ = true;
            // A coarse frame may reach the preparation window late. Give the
            // keyboard mask the full requested lead time before pressing left.
            phase_target_ms_ = static_cast<int>(
                std::max<int64_t>(due_ms, now_ms + pre_fire_ms) - phase_time_ms_);
        }
        if (now_ms - phase_time_ms_ >= phase_target_ms_)
            beginFire(act, now_ms, hold_mode, duration, duration_jitter);
    }

    void beginFire(Action& act, int64_t now_ms, bool hold_mode,
                   int duration, int duration_jitter)
    {
        act.press_left = true;
        act.fired = true;
        phase_ = TriggerPhase::Pressed;
        pressed_hold_ = hold_mode;
        first_shot_pending_ = false;
        shot_since_switch_ = true;
        delay_retargeted_ = false;
        phase_time_ms_ = now_ms;
        in_zone_since_ms_ = -1;
        prefire_sent_ = false;
        phase_target_ms_ = hold_mode ? 0 : jitter(duration, duration_jitter);
        hit_zone_loss_since_ms_ = -1;
    }

    static int jitter(int base, int j)
    {
        if (j <= 0) return std::max(0, base);
        thread_local std::mt19937 rng{ std::random_device{}() };
        std::uniform_int_distribution<int> d(-j, j);
        return std::max(0, base + d(rng));
    }

    TriggerPhase phase_ = TriggerPhase::Idle;
    int64_t phase_time_ms_ = 0;
    int64_t in_zone_since_ms_ = -1;
    int last_fire_track_id_ = -1;
    int phase_target_ms_ = 0;
    bool prefire_sent_ = false;
    bool first_shot_pending_ = true;
    bool shot_since_switch_ = false;
    bool delay_retargeted_ = false;
    bool pressed_hold_ = false;
    int64_t target_loss_since_ms_ = -1;
    int64_t hit_zone_loss_since_ms_ = -1;
};

}

#endif // MOUSE_TRIGGER_FSM_H
