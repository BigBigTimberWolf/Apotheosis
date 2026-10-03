#pragma once
#include "control/types.h"
#include "runtime/frame_context.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace AutoCapture {
struct Policy {
    bool enabled=false, triggerOnly=false, anyDetection=false, useHigh=true, useLow=false;
    float high=.85f, low=.3f;
    bool accepts(bool fresh, bool force, bool confirmedTrigger,
                 size_t detectionCount, const std::vector<float>& confidences) const {
        if (!enabled) return false;
        if (triggerOnly) return confirmedTrigger;
        if (force) return true;
        if (!fresh || detectionCount == 0) return false;
        if (anyDetection) return true;
        for (float c : confidences)
            if ((useHigh && c >= high) || (useLow && c > 0 && c <= low)) return true;
        return false;
    }
};
struct TriggerSample {
    runtime::FrameContext context;
    std::vector<control::Candidate> detections;
};
// This mailbox preserves the firing frame and its labels across the race between
// inference publication, the aim worker and the asynchronous image writer.
class TriggerSamples {
public:
    bool pending() const { return pending_.load(std::memory_order_acquire); }
    void push(TriggerSample sample) {
        if (!sample.context.sequence && sample.context.captured_ns <= 0) return;
        std::lock_guard lock(mutex_);
        if (samples_.size() >= 16) samples_.pop_front();
        samples_.push_back(std::move(sample)); pending_.store(true, std::memory_order_release);
    }
    std::optional<TriggerSample> pop() {
        std::lock_guard lock(mutex_);
        if (samples_.empty()) return {};
        auto result=std::move(samples_.front()); samples_.pop_front();
        pending_.store(!samples_.empty(), std::memory_order_release); return result;
    }
    void clear() {
        std::lock_guard lock(mutex_); samples_.clear(); pending_.store(false, std::memory_order_release);
    }
private:
    mutable std::mutex mutex_;
    std::deque<TriggerSample> samples_;
    std::atomic<bool> pending_{false};
};
} // namespace AutoCapture
