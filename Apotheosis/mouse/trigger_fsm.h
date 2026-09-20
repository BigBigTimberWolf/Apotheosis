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
        int    track_id = -1;
        int64_t now_ms = 0;
    };

    struct Action
    {
        bool press_left = false;
        bool release_left = false;
        bool fired = false;
    };

    Action tick(const Input& in, bool hold_mode,
                int fire_delay, int duration, int interval, int switch_cd,
                int delay_jitter, int duration_jitter, int interval_jitter)
    {
        Action act;

        if (in.track_id != last_fire_track_id_ &&
            last_fire_track_id_ != -1 &&
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
        }
        last_fire_track_id_ = in.track_id;

        switch (phase_)
        {
        case TriggerPhase::Idle:
            if (in.in_zone)
            {
                const int target_delay = jitter(fire_delay, delay_jitter);
                if (target_delay <= 0)
                {
                    beginFire(act, in.now_ms, hold_mode, duration, duration_jitter);
                }
                else
                {
                    if (in_zone_since_ms_ < 0)
                    {
                        in_zone_since_ms_ = in.now_ms;
                        phase_target_ms_ = target_delay;
                    }
                    if (in.now_ms - in_zone_since_ms_ >= phase_target_ms_)
                        beginFire(act, in.now_ms, hold_mode, duration, duration_jitter);
                    else
                    {
                        phase_ = TriggerPhase::Delay;
                        phase_time_ms_ = in_zone_since_ms_;
                    }
                }
            }
            else
            {
                in_zone_since_ms_ = -1;
            }
            break;

        case TriggerPhase::Delay:
            if (!in.in_zone)
            {
                phase_ = TriggerPhase::Idle;
                in_zone_since_ms_ = -1;
                break;
            }
            if (in.now_ms - phase_time_ms_ >= phase_target_ms_)
                beginFire(act, in.now_ms, hold_mode, duration, duration_jitter);
            break;

        case TriggerPhase::Pressed:
            if (hold_mode)
            {
                if (!in.in_zone)
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
                phase_time_ms_ = in.now_ms;
                phase_target_ms_ = jitter(interval, interval_jitter);
            }
            break;

        case TriggerPhase::Cooldown:
            if (in.now_ms - phase_time_ms_ >= phase_target_ms_)
            {
                phase_ = TriggerPhase::Idle;
                in_zone_since_ms_ = -1;
                if (in.in_zone && fire_delay <= 0)
                    beginFire(act, in.now_ms, hold_mode, duration, duration_jitter);
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

    bool reset()
    {
        const bool was_pressed = (phase_ == TriggerPhase::Pressed);
        phase_ = TriggerPhase::Idle;
        phase_time_ms_ = 0;
        phase_target_ms_ = 0;
        in_zone_since_ms_ = -1;
        last_fire_track_id_ = -1;
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
    void beginFire(Action& act, int64_t now_ms, bool hold_mode,
                   int duration, int duration_jitter)
    {
        act.press_left = true;
        act.fired = true;
        phase_ = TriggerPhase::Pressed;
        phase_time_ms_ = now_ms;
        in_zone_since_ms_ = -1;
        phase_target_ms_ = hold_mode ? 0 : jitter(duration, duration_jitter);
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
};

}

#endif // MOUSE_TRIGGER_FSM_H
