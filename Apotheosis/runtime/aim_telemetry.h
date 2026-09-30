#ifndef RUNTIME_AIM_TELEMETRY_H
#define RUNTIME_AIM_TELEMETRY_H

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/core/types.hpp>

namespace runtime
{

struct ReplayFrame
{
    std::chrono::steady_clock::time_point ts;
    std::vector<cv::Rect> boxes;
    std::vector<int> class_ids;
    std::vector<int> track_ids;
    int   locked_track_id = -1;
    int   resolution = 320;
    double cross_x = 0.0;
    double cross_y = 0.0;
    double fov_radius_x = 0.0, fov_radius_y = 0.0;
    bool mask_x = false, mask_y = false; // Requested physical input mask.
    bool unlock_x = false, unlock_y = false;
    double pivot_x = 0.0;
    double pivot_y = 0.0;
    double base_anchor_x = 0.0, base_anchor_y = 0.0;
    double follow_strength_x = 0.0, follow_strength_y = 0.0;
    double follow_motion_x = 0.0, follow_motion_y = 0.0;
    double follow_preset_x = 0.0, follow_preset_y = 0.0;
    int follow_state_x = 0, follow_state_y = 0;
    bool scope_params = false, secondary_params = false;
    double error_x = 0.0;
    double error_y = 0.0;
    double derivative_raw_x = 0.0;
    double derivative_raw_y = 0.0;
    int   requested_dx = 0;
    int   requested_dy = 0;
    int   mouse_dx = 0;
    int   mouse_dy = 0;
    bool  hotkey_active = false;
};

class ReplayBuffer
{
public:
    static ReplayBuffer& instance();

    void setEnabled(bool enabled);
    bool enabled() const;

    void setRetentionSeconds(int seconds);

    void push(ReplayFrame frame);
    void clear();

    std::vector<ReplayFrame> snapshot() const;
    size_t size() const;

private:
    ReplayBuffer() = default;

    mutable std::mutex mu_;
    std::deque<ReplayFrame> frames_;
    bool   enabled_ = false;
    int    retention_seconds_ = 10;
};

// ── 预览窗的瞄准叠加状态 ────────────────────────────────────────────────────
//
// 目的: 让用户【看得见稳定器到底做了什么】。原来预览只画检测框(detectionBuffer
// 里的原始框), 那是稳定【之前】的东西 —— 稳定器判定、目标锁定、滤波压掉的抖动
// 全都不在画面上, 调参只能靠猜。
//
// ★ 跟踪后的目标框、原始瞄点和补偿后的最终瞄点。全部用【检测分辨率坐标系】,
//   与检测框、准星找色同一套坐标。
// ★ 由 aim_loop 每拍写一次, 预览线程读一次。不参与任何控制计算。
enum class TriggerOverlayReason {
    Disabled, NoTarget, OutsideZone, Ready, ScopeWait, FirstShotDelay,
    Cooldown, AutoStopWait, SwitchBusy, DriverRejected, Pressed, NoDriver
};

struct AimOverlayState
{
    std::chrono::steady_clock::time_point ts{};   // 写入时刻 (预览据此判过期)
    uint64_t seq = 0;                             // 每写一次 +1
    bool  valid = false;                          // 本拍有没有锁定目标
    bool  engaged = false;                        // 控制器本拍是否真的在输出
    cv::Rect box{};                               // 跟踪器输出的目标框
    double filtered_cx = 0.0;                     // 跟踪后的中心
    double filtered_cy = 0.0;
    double anchor_x = 0.0;                        // 控制器最终瞄点
    double anchor_y = 0.0;
    double base_anchor_x = 0.0, base_anchor_y = 0.0;
    double base_error_x = 0.0, base_error_y = 0.0;
    double cross_x = 0.0, cross_y = 0.0;
    double fov_radius_x = 0.0, fov_radius_y = 0.0;
    bool mask_x = false, mask_y = false; // Requested physical input mask.
    bool unlock_x = false, unlock_y = false;
    double follow_strength_x = 0.0, follow_strength_y = 0.0;
    double follow_motion_x = 0.0, follow_motion_y = 0.0;
    double follow_preset_x = 0.0, follow_preset_y = 0.0;
    int follow_state_x = 0, follow_state_y = 0;
    int   target_id = -1;
    int   target_class_id = -1;
    int   verdict = 0;                            // control::TrackLockState
    int   idle_reason = 0;                        // control::ControlOutput::IdleReason
    bool  scope_params = false;                   // 本拍是否在用「开镜档」参数
    bool  secondary_params = false;
    bool  trigger_valid = false;
    bool  trigger_in_zone = false;
    TriggerOverlayReason trigger_reason = TriggerOverlayReason::Disabled;
    int   trigger_class_id = -1;
    double trigger_point_x = 0.0, trigger_point_y = 0.0;
    double trigger_half_width = 0.0, trigger_half_height = 0.0;
    double control_dt_ms = 0.0;
    double capture_age_ms = -1.0;
    double derivative_raw_x = 0.0, derivative_raw_y = 0.0;
};

void publishAimOverlay(const AimOverlayState& state);
AimOverlayState readAimOverlay();

// 预览用: 距今超过这个毫秒数的叠加状态算过期 (瞄准停下来了就别再画旧框)。
inline constexpr int kAimOverlayStaleMs = 250;

}

#include <atomic>
extern std::atomic<bool> g_replay_playback_active;
extern std::atomic<int>  g_replay_playback_frame;
extern std::atomic<int>  g_replay_playback_total;
extern std::atomic<unsigned int> g_replay_playback_request;
extern std::atomic<float> g_mouse_queue_latency_ms;
extern std::atomic<int> g_mouse_queue_backlog;
extern std::atomic<unsigned long long> g_mouse_send_failures;

#endif // RUNTIME_AIM_TELEMETRY_H
