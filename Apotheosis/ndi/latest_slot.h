#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace ndi {
// One pending item, independent from the item currently in use by NDI.
template<class T> class LatestSlot {
public:
    void publish(T value) {
        { std::lock_guard lock(mutex_); if (closed_) return; pending_ = std::move(value); }
        cv_.notify_one();
    }
    std::optional<T> take() {
        std::lock_guard lock(mutex_);
        auto result = std::move(pending_); pending_.reset(); return result;
    }
    bool wait(int ms) {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return closed_ || pending_.has_value(); });
        return pending_.has_value();
    }
    void close() { { std::lock_guard lock(mutex_); closed_ = true; pending_.reset(); } cv_.notify_all(); }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<T> pending_;
    bool closed_ = false;
};
}
