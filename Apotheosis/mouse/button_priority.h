#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace mouse_async {

// Announce buttons before taking the shared device lock. Movements yield that
// lock while a button is pending, including moves on direct-send backends.
class ButtonPriority {
public:
    class Pending {
    public:
        Pending(ButtonPriority& owner, std::recursive_mutex& mutex)
            : owner_(owner), mutex_(mutex) { ++owner_.pending_; }
        ~Pending() {
            // Use the waiter's mutex to prevent a notification being lost
            // between its predicate check and going to sleep.
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            --owner_.pending_;
            owner_.cv_.notify_all();
        }
        Pending(const Pending&) = delete;
        Pending& operator=(const Pending&) = delete;
    private:
        ButtonPriority& owner_;
        std::recursive_mutex& mutex_;
    };
    template<class Mutex>
    void wait(std::unique_lock<Mutex>& lock) {
        cv_.wait(lock, [this] { return pending_.load() == 0; });
    }
    int pending() const { return pending_.load(); }
private:
    std::atomic<int> pending_{0};
    std::condition_variable_any cv_;
};

} // namespace mouse_async
