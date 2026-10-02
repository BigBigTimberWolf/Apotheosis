#include "mouse/button_priority.h"
#include <future>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <thread>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

int main() {
    for (int attempt = 0; attempt < 100; ++attempt) {
        mouse_async::ButtonPriority priority;
        std::recursive_mutex device;
        std::vector<int> order;
        std::promise<void> allowButton, moveAcquired;
        auto buttonGate = allowButton.get_future();
        auto moveGate = moveAcquired.get_future();
        std::unique_lock lock(device); // already in-flight device command
        auto button = std::async(std::launch::async, [&] {
            mouse_async::ButtonPriority::Pending pending(priority, device);
            buttonGate.wait();
            std::lock_guard send(device);
            order.push_back(1);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!priority.pending() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        CHECK(priority.pending() == 1);
        auto move = std::async(std::launch::async, [&] {
            std::unique_lock send(device);
            moveAcquired.set_value();
            priority.wait(send);
            order.push_back(2);
        });
        lock.unlock();
        // Force movement to acquire the device first. It must yield to the
        // announced button, rather than merely winning a favourable schedule.
        CHECK(moveGate.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        allowButton.set_value();
        CHECK(button.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        CHECK(move.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        button.get(); move.get();
        CHECK((order == std::vector<int>{1, 2}));
        CHECK(priority.pending() == 0);
        try {
            mouse_async::ButtonPriority::Pending failed(priority, device);
            throw 1;
        } catch (int) {}
        CHECK(priority.pending() == 0); // failed send cannot strand movement
    }
    std::cout << "Button priority ordering and failure cleanup passed (100 schedules)\n";
}
