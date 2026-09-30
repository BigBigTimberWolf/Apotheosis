#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace mouse_async {

// 开火后切枪：等待 → 3 → 1 → 1 → 1，每次松键后间隔 20ms。
// 键盘动作由独立线程执行，不占用采集/推理拍；析构时可中断等待并收尾。
class WeaponSwitch31
{
public:
    using TapKey = std::function<bool(int hidKey, int holdMs)>;

    explicit WeaponSwitch31(TapKey tapKey) : tapKey_(std::move(tapKey))
    {
        worker_ = std::thread([this] { run(); });
    }

    ~WeaponSwitch31()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    WeaponSwitch31(const WeaponSwitch31&) = delete;
    WeaponSwitch31& operator=(const WeaponSwitch31&) = delete;

    bool request(int afterShotDelayMs)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || busy_.load(std::memory_order_acquire)) return false;
        jobDelayMs_ = std::clamp(afterShotDelayMs, 0, 2000);
        pending_ = true;
        busy_.store(true, std::memory_order_release);
        cv_.notify_one();
        return true;
    }

    bool busy() const noexcept { return busy_.load(std::memory_order_acquire); }

private:
    using Clock = std::chrono::steady_clock;

    bool waitUntil(Clock::time_point deadline)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_until(lock, deadline, [this] { return stopping_; });
        return !stopping_;
    }

    bool send(int hidKey, int holdMs)
    {
        try { return tapKey_ && tapKey_(hidKey, holdMs); }
        catch (...) { return false; }
    }

    void run()
    {
        for (;;)
        {
            int delayMs = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || pending_; });
                if (stopping_) break;
                delayMs = jobDelayMs_;
                pending_ = false;
            }

            const bool waited = waitUntil(Clock::now() + std::chrono::milliseconds(delayMs));
            const auto threeStart = Clock::now();
            if (waited && send(0x20, 20))
            {
                auto previousStart = threeStart;
                bool completed = true;
                for (int i = 0; i < 3; ++i)
                {
                    // MAKCUNEW 的 tap 交给固件异步执行；KMBOXNET 的 tap 会
                    // 等按键抬起才返回。两种后端都要留足 20ms 按住 + 20ms 间隔。
                    const auto nextAt = std::max(
                        Clock::now() + std::chrono::milliseconds(20),
                        previousStart + std::chrono::milliseconds(40));
                    if (!waitUntil(nextAt))
                    {
                        completed = false;
                        break;
                    }
                    previousStart = Clock::now();
                    if (!send(0x1E, 20))
                    {
                        completed = false;
                        break;
                    }
                }
                // The serial keyboard acknowledges the tap before the game
                // has equipped weapon 1. Keep the next scope/shot blocked
                // through the final release and a short equip settling time.
                constexpr int kEquipSettleMs = 120;
                const int settleMs = completed ? 20 + kEquipSettleMs : 20;
                waitUntil(previousStart + std::chrono::milliseconds(settleMs));
            }
            busy_.store(false, std::memory_order_release);
        }
        busy_.store(false, std::memory_order_release);
    }

    TapKey tapKey_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> busy_{ false };
    bool pending_ = false;
    bool stopping_ = false;
    int jobDelayMs_ = 50;
};

}
