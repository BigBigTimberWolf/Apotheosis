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
        {
            std::lock_guard<std::mutex> lock(mutex);
            keys.push_back(key);
            holds.push_back(holdMs);
            times.push_back(std::chrono::steady_clock::now());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
        return true;
    });

    const auto requestedAt = std::chrono::steady_clock::now();
    if (!worker.request(10) || worker.request(10) || !waitDone(worker))
    {
        std::puts("switch request or busy gate failed");
        return 1;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (keys != std::vector<int>{ 0x20, 0x1E, 0x1E, 0x1E } ||
            holds != std::vector<int>{ 20, 20, 20, 20 })
        {
            std::puts("switch must tap 3 then 1 three times");
            return 1;
        }
        if (times[0] - requestedAt < std::chrono::milliseconds(8))
        {
            std::puts("post-shot delay was skipped");
            return 1;
        }
        for (size_t i = 1; i < times.size(); ++i)
            if (times[i] - times[i - 1] < std::chrono::milliseconds(38))
            {
                std::puts("20ms inter-key spacing was skipped");
                return 1;
            }
        if (std::chrono::steady_clock::now() - times.back() <
            std::chrono::milliseconds(135))
        {
            std::puts("new scope/shot resumed before weapon 1 settled");
            return 1;
        }
    }

    std::vector<std::chrono::steady_clock::time_point> asyncTimes;
    mouse_async::WeaponSwitch31 asyncWorker([&](int, int) {
        asyncTimes.push_back(std::chrono::steady_clock::now());
        return true; // 模拟固件接受 tap 后立即返回，按键仍会持续 20ms。
    });
    if (!asyncWorker.request(0) || !waitDone(asyncWorker) || asyncTimes.size() != 4)
    {
        std::puts("asynchronous tap sequence did not finish");
        return 1;
    }
    for (size_t i = 1; i < asyncTimes.size(); ++i)
        if (asyncTimes[i] - asyncTimes[i - 1] < std::chrono::milliseconds(38))
        {
            std::puts("asynchronous tap must allow hold plus 20ms gap");
            return 1;
        }

    int failedCalls = 0;
    mouse_async::WeaponSwitch31 failed([&](int, int) {
        ++failedCalls;
        return false;
    });
    if (!failed.request(0) || !waitDone(failed) || failedCalls != 1)
    {
        std::puts("failed 3 tap must not continue to 1");
        return 1;
    }

    std::vector<int> partialKeys;
    mouse_async::WeaponSwitch31 partial([&](int key, int) {
        partialKeys.push_back(key);
        return partialKeys.size() != 3;
    });
    if (!partial.request(0) || !waitDone(partial) ||
        partialKeys != std::vector<int>{ 0x20, 0x1E, 0x1E })
    {
        std::puts("failed 1 tap must stop the remaining sequence");
        return 1;
    }

    int cancelledCalls = 0;
    {
        mouse_async::WeaponSwitch31 cancelled([&](int, int) {
            ++cancelledCalls;
            return true;
        });
        if (!cancelled.request(500)) return 1;
    }
    if (cancelledCalls != 0)
    {
        std::puts("destruction must cancel delayed switch");
        return 1;
    }
    return 0;
}
