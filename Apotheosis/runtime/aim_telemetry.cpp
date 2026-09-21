#include "aim_telemetry.h"

#include <algorithm>
#include <cmath>

std::atomic<float> g_mouse_queue_latency_ms{0.0f};
std::atomic<int> g_mouse_queue_backlog{0};
std::atomic<unsigned long long> g_mouse_send_failures{0};

namespace runtime
{

ReplayBuffer& ReplayBuffer::instance()
{
    static ReplayBuffer b;
    return b;
}

void ReplayBuffer::setEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lk(mu_);
    enabled_ = enabled;
    if (!enabled_)
        frames_.clear();
}

bool ReplayBuffer::enabled() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return enabled_;
}

void ReplayBuffer::setRetentionSeconds(int seconds)
{
    std::lock_guard<std::mutex> lk(mu_);
    retention_seconds_ = std::clamp(seconds, 1, 60);
}

void ReplayBuffer::push(const ReplayFrame& frame)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_)
        return;

    frames_.push_back(frame);

    const auto cutoff = frame.ts - std::chrono::seconds(retention_seconds_);
    while (!frames_.empty() && frames_.front().ts < cutoff)
        frames_.pop_front();
}

void ReplayBuffer::clear()
{
    std::lock_guard<std::mutex> lk(mu_);
    frames_.clear();
}

std::vector<ReplayFrame> ReplayBuffer::snapshot() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return std::vector<ReplayFrame>(frames_.begin(), frames_.end());
}

size_t ReplayBuffer::size() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return frames_.size();
}

// ── 预览叠加状态 ────────────────────────────────────────────────────────────
//
// ★ 用一把小锁而不是无锁双缓冲: 写方是瞄准线程(每拍一次)、读方是预览线程
//   (~60Hz), 两边临界区都只是几十字节的拷贝 —— 锁的开销远小于每拍本来就要做的
//   检测框搬运。无锁在这里是没必要的复杂度。
namespace
{
std::mutex g_overlay_mutex;
AimOverlayState g_overlay;
}

void publishAimOverlay(const AimOverlayState& state)
{
    std::lock_guard<std::mutex> lk(g_overlay_mutex);
    const uint64_t seq = g_overlay.seq;
    g_overlay = state;
    g_overlay.seq = seq + 1;
    g_overlay.ts = std::chrono::steady_clock::now();
}

AimOverlayState readAimOverlay()
{
    std::lock_guard<std::mutex> lk(g_overlay_mutex);
    return g_overlay;
}

}
