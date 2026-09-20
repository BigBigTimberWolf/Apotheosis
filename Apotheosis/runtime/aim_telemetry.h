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

}

#include <atomic>
extern std::atomic<bool> g_replay_playback_active;
extern std::atomic<int>  g_replay_playback_frame;
extern std::atomic<float> g_mouse_queue_latency_ms;
extern std::atomic<int> g_mouse_queue_backlog;
extern std::atomic<unsigned long long> g_mouse_send_failures;

#endif // RUNTIME_AIM_TELEMETRY_H
