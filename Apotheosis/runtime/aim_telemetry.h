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
    double pivot_x = 0.0;
    double pivot_y = 0.0;
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

    void push(const ReplayFrame& frame);
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
// ★ 这里面装的是【稳定之后】的结果: 稳定器放行的目标框 + α-β 滤波后的中心 +
//   控制器最终瞄点。全部用【检测分辨率坐标系】, 与检测框、准星找色同一套坐标。
// ★ 由 aim_loop 每拍写一次, 预览线程读一次。不参与任何控制计算。
struct AimOverlayState
{
    std::chrono::steady_clock::time_point ts{};   // 写入时刻 (预览据此判过期)
    uint64_t seq = 0;                             // 每写一次 +1
    bool  valid = false;                          // 本拍有没有锁定目标
    bool  engaged = false;                        // 控制器本拍是否真的在输出
    cv::Rect box{};                               // 稳定器放行后的目标框
    double filtered_cx = 0.0;                     // 滤波后的中心 (稳定后的位置)
    double filtered_cy = 0.0;
    double anchor_x = 0.0;                        // 控制器最终瞄点
    double anchor_y = 0.0;
    int   target_id = -1;
    int   target_class_id = -1;
    int   verdict = 0;                            // control::StabilizerVerdict
    int   idle_reason = 0;                        // control::ControlOutput::IdleReason
    bool  scope_params = false;                   // 本拍是否在用「开镜档」参数
};

void publishAimOverlay(const AimOverlayState& state);
AimOverlayState readAimOverlay();

// 预览用: 距今超过这个毫秒数的叠加状态算过期 (瞄准停下来了就别再画旧框)。
inline constexpr int kAimOverlayStaleMs = 250;

}

#include <atomic>
extern std::atomic<bool> g_replay_playback_active;
extern std::atomic<int>  g_replay_playback_frame;
extern std::atomic<float> g_mouse_queue_latency_ms;
extern std::atomic<int> g_mouse_queue_backlog;
extern std::atomic<unsigned long long> g_mouse_send_failures;

#endif // RUNTIME_AIM_TELEMETRY_H
