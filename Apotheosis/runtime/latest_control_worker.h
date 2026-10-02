#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace runtime {
// One pending update, not a queue of outdated aiming commands. The callback
// reads a complete detection batch under DetectionBuffer::mutex.
class LatestControlWorker {
public:
    explicit LatestControlWorker(std::function<void()> update)
        : update_(std::move(update)), thread_([this] { run(); }) {}
    LatestControlWorker(const LatestControlWorker&) = delete;
    LatestControlWorker& operator=(const LatestControlWorker&) = delete;
    ~LatestControlWorker() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }
    void notify() {
        {
            std::lock_guard lock(mutex_);
            pending_ = true;
        }
        cv_.notify_one();
    }
private:
    void run() {
        for (;;) {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || pending_; });
            if (stopping_) return;
            pending_ = false;
            lock.unlock();
            update_();
        }
    }
    std::function<void()> update_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool pending_ = false, stopping_ = false;
    std::thread thread_;
};
}
