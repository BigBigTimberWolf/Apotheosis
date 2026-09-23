#include "mouse/weapon_switch31.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {

bool waitDone(mouse_async::WeaponSwitch31& worker)
{
    for (int i = 0; i < 1000 && worker.busy(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return !worker.busy();
}

}

int main()
{
    std::mutex mutex;
    std::vector<int> keys;
    std::vector<int> holds;
    std::vector<std::chrono::steady_clock::time_point> times;
    mouse_async::WeaponSwitch31 worker([&](int key, int holdMs) {
        std::lock_guard<std::mutex> lock(mutex);
        keys.push_back(key);
        holds.push_back(holdMs);
        times.push_back(std::chrono::steady_clock::now());
        return true;
    });

    const auto requestedAt = std::chrono::steady_clock::now();
    if (!worker.request(10, 5) || worker.request(10, 5) || !waitDone(worker))
    {
        std::puts("switch request or busy gate failed");
        return 1;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (keys != std::vector<int>{ 0x20, 0x1E } ||
            holds != std::vector<int>{ 5, 5 })
        {
            std::puts("switch must tap 3 then 1 exactly once");
            return 1;
        }
        if (times[0] - requestedAt < std::chrono::milliseconds(8) ||
            times[1] - times[0] < std::chrono::milliseconds(8))
        {
            std::puts("post-shot delay or 3-to-1 spacing was skipped");
            return 1;
        }
    }

    int failedCalls = 0;
    mouse_async::WeaponSwitch31 failed([&](int, int) {
        ++failedCalls;
        return false;
    });
    if (!failed.request(0, 5) || !waitDone(failed) || failedCalls != 1)
    {
        std::puts("failed 3 tap must not continue to 1");
        return 1;
    }

    int cancelledCalls = 0;
    {
        mouse_async::WeaponSwitch31 cancelled([&](int, int) {
            ++cancelledCalls;
            return true;
        });
        if (!cancelled.request(500, 20)) return 1;
    }
    if (cancelledCalls != 0)
    {
        std::puts("destruction must cancel delayed switch");
        return 1;
    }
    return 0;
}
