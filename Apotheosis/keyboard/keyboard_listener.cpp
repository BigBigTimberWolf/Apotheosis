#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>

#include "capture.h"
#include "config.h"
#include "keyboard_listener.h"
#include "hotkey_selection.h"
#include "keycodes.h"
#include "Makcu.h"
#include "MakcuNew.h"
#include "kmboxNetConnection.h"
#include "mouse.h"
#include "runtime/active_hotkey.h"
#include "Apotheosis.h"
#include "runtime/config_snapshot.h"
#include "runtime/ff_calibration_session.h"
#include "runtime/sched_boost.h"
#include "macro/macro_engine.h"
#include "macro/macro_config.h"
#include "mouse/windows_driver.h"
#include "mouse/cpbox_driver.h"

extern std::atomic<bool> shouldExit;
extern std::atomic<bool> aiming;

namespace
{

bool win32_key_pressed(int vk_code)
{
    return (GetAsyncKeyState(vk_code) & 0x8000) != 0;
}

// 滚轮不是一个能“按住”的键：滚一格算一次触发，用一个短窗口表示“刚滚过”。
// 只有本机低层钩子能看到滚轮（被控端盒子不上报物理滚轮），所以滚轮热键在
// 鼠标接本机或 input_method = WINDOWS 时才有效。
bool wheelKeyPulse(const std::string& key)
{
    constexpr int64_t kPulseMs = 80;
    static std::atomic<uint64_t> seenUp{0}, seenDown{0};
    static std::atomic<int64_t> lastUp{0}, lastDown{0};
    const bool up = key == "WheelUp";
    auto& seen = up ? seenUp : seenDown;
    auto& stamp = up ? lastUp : lastDown;
    const auto count = static_cast<uint64_t>(mouse_driver::windowsWheelCounter(up));
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (count != seen.load(std::memory_order_relaxed)) {
        seen.store(count, std::memory_order_relaxed);
        stamp.store(now, std::memory_order_relaxed);
    }
    const auto at = stamp.load(std::memory_order_relaxed);
    return at > 0 && now - at <= kPulseMs;
}

int keyboardHidUsage(const std::string& key)
{
    return macros::hidKey(key);
}

}

bool isAnyKeyPressed(const std::vector<std::string>& keys)
{
    const auto cfg = runtime_config::read();
    std::lock_guard<std::mutex> deviceLock(inputDeviceMutex);
    if (keys.empty())
        return true;

    for (const auto& key_name : keys)
    {
        if (key_name.empty() || key_name == "None" ||
            key_name.find(u8"始终活跃") != std::string::npos)
            return true;

        // 滚轮热键：本机钩子边沿判定，不走驱动按键回读。
        if (macros::wheelKeyId(key_name)) {
            if (wheelKeyPulse(key_name)) return true;
            continue;
        }

        bool pressed = false;
        if (cfg->input_method == "WINDOWS") {
            if (mouse_driver::windowsPhysicalKeyPressed(KeyCodes::getKeyCode(key_name))) return true;
            continue;
        }
        const int keyboardHid = keyboardHidUsage(key_name);
        if(keyboardHid>=0x10000){if(mouse_driver::windowsPhysicalKeyPressed(KeyCodes::getKeyCode(key_name)))return true;continue;}
        if (keyboardHid > 0)
        {
            int state = -1;
            if (cfg->input_method == "KMBOXNET" && kmboxNetSerial &&
                kmboxNetSerial->isOpen())
                state = kmboxNetSerial->monitorKeyboard(static_cast<short>(keyboardHid));
            if (cfg->input_method == "FERRUM" && ferrumDriver && ferrumDriver->isOpen())
                state = ferrumDriver->physicalKeyPressed(keyboardHid);
            if (cfg->input_method == "CAT" && catDriver && catDriver->isOpen())
                state = catDriver->physicalKeyPressed(keyboardHid);
            const int vk = KeyCodes::getKeyCode(key_name);
            pressed = state > 0 || (vk > 0 && mouse_driver::windowsPhysicalKeyPressed(vk));
            if (pressed) return true;
            continue;
        }

        if (cfg->input_method == "MAKCU")
        {
            if (makcuSerial && makcuSerial->isOpen())
            {
                if (key_name == "LeftMouseButton")        pressed = makcuSerial->shooting_active;
                else if (key_name == "RightMouseButton")  pressed = makcuSerial->zooming_active;
                else if (key_name == "MiddleMouseButton") pressed = makcuSerial->middle_active;
                else if (key_name == "X1MouseButton")     pressed = makcuSerial->side1_active;
                else if (key_name == "X2MouseButton")     pressed = makcuSerial->side2_active;
            }
        }
        else if (cfg->input_method == "MAKCUNEW")
        {
            if (makcuNewSerial && makcuNewSerial->isOpen())
            {
                if (key_name == "LeftMouseButton")        pressed = makcuNewSerial->physicalButtonPressed(1);
                else if (key_name == "RightMouseButton")  pressed = makcuNewSerial->physicalButtonPressed(2);
                else if (key_name == "MiddleMouseButton") pressed = makcuNewSerial->physicalButtonPressed(3);
                else if (key_name == "X1MouseButton")     pressed = makcuNewSerial->physicalButtonPressed(4);
                else if (key_name == "X2MouseButton")     pressed = makcuNewSerial->physicalButtonPressed(5);
            }
        }
        else if (cfg->input_method == "CPBOX")
        {
            if (cpboxDriver && cpboxDriver->isOpen()) {
                const int button = key_name == "LeftMouseButton" ? 1 :
                    key_name == "RightMouseButton" ? 2 : key_name == "MiddleMouseButton" ? 3 :
                    key_name == "X1MouseButton" ? 4 : key_name == "X2MouseButton" ? 5 : 0;
                if (button) pressed = cpboxDriver->physicalButtonPressed(button) > 0;
                else {
                    const int vk = KeyCodes::getKeyCode(key_name);
                    if (vk > 0) pressed = mouse_driver::windowsPhysicalKeyPressed(vk);
                }
            }
        }
        else if (cfg->input_method == "KMBOXNET")
        {
            if (kmboxNetSerial && kmboxNetSerial->isOpen())
            {
                int monitor_state = -1;
                if (key_name == "LeftMouseButton")
                    monitor_state = kmboxNetSerial->monitorMouseLeft();
                else if (key_name == "RightMouseButton")
                    monitor_state = kmboxNetSerial->monitorMouseRight();
                else if (key_name == "MiddleMouseButton")
                    monitor_state = kmboxNetSerial->monitorMouseMiddle();
                else if (key_name == "X1MouseButton")
                    monitor_state = kmboxNetSerial->monitorMouseSide1();
                else if (key_name == "X2MouseButton")
                    monitor_state = kmboxNetSerial->monitorMouseSide2();

                if (monitor_state >= 0)
                    pressed = monitor_state != 0;
                else
                {
                    const int vk = KeyCodes::getKeyCode(key_name);
                    if (vk > 0) pressed = win32_key_pressed(vk);
                }
            }
        }
        else if (cfg->input_method == "DHZBOX_MINI")
        {
            int button = 0;
            if (key_name == "LeftMouseButton") button = 1;
            else if (key_name == "RightMouseButton") button = 2;
            else if (key_name == "MiddleMouseButton") button = 3;
            else if (key_name == "X1MouseButton") button = 4;
            else if (key_name == "X2MouseButton") button = 5;
            const int state = dhzboxDriver && button ? dhzboxDriver->physicalButtonPressed(button) : -1;
            if (state >= 0) pressed = state != 0;
            else {
                const int vk = KeyCodes::getKeyCode(key_name);
                if (vk > 0) pressed = win32_key_pressed(vk);
            }
        }
        else if (cfg->input_method == "FERRUM" || cfg->input_method == "CAT")
        {
            int button = 0;
            if (key_name == "LeftMouseButton") button = 1;
            else if (key_name == "RightMouseButton") button = 2;
            else if (key_name == "MiddleMouseButton") button = 3;
            else if (key_name == "X1MouseButton") button = 4;
            else if (key_name == "X2MouseButton") button = 5;
            const auto driver=cfg->input_method == "CAT" ? catDriver : ferrumDriver;
            const int state = driver && button ? driver->physicalButtonPressed(button) : -1;
            if (state >= 0) pressed = state != 0;
            else {
                const int vk = KeyCodes::getKeyCode(key_name);
                if (vk > 0) pressed = win32_key_pressed(vk);
            }
        }

        if (pressed) return true;
    }
    return false;
}

void keyboardListener()
{
    sched_boost::LiveThreadBoost threadBoost;
    HotkeyActivationSampler activationSampler;
    bool calibrationKeyHeld = false;
    while (!shouldExit)
    {
        const auto scheduling = runtime_config::read();
        threadBoost.update(scheduling->use_mmcss,
                           scheduling->mmcss_task_name.c_str());
        int next_active = -1;
        {
            std::lock_guard<std::recursive_mutex> cfg(configMutex);
            // Read each physical selector once; repeated presses select the
            // whole profile and never toggle its nested legacy parameter set.
            std::vector<std::pair<std::string, bool>> sampled;
            auto pressed = [&](const std::string& key) {
                for (const auto& entry : sampled)
                    if (entry.first == key) return entry.second;
                // 滚轮没有 HID 码，走 isAnyKeyPressed 的滚轮边沿判定。
                const bool down = (macros::wheelKeyId(key) || KeyCodes::getKeyCode(key) > 0) &&
                    isAnyKeyPressed({key});
                sampled.emplace_back(key, down);
                return down;
            };
            if (activationSampler.sample(config.hotkeys, config.active_hotkey_group, pressed)) {
                runtime_config::publish();
                runtime::g_hotkey_activation_dirty.store(true);
            }
            next_active = selectActiveHotkeyIndex(
                config.hotkeys, config.active_hotkey_group, isAnyKeyPressed);
            // FF 标定的“标定启动键”走同一条设备读取路径：它在被控端按下，
            // 与窗口焦点无关。这里只数按下沿，界面按 100ms 轮询也不会漏掉
            // 一次短按；未设置时不产生任何设备往返。
            const auto& calibrationKey = scheduling->ff_calibration_key;
            const bool calibrationKeyDown = !calibrationKey.empty() && pressed(calibrationKey);
            if (calibrationKeyDown && !calibrationKeyHeld)
                runtime::g_ffCalibrationKeyTaps.fetch_add(1, std::memory_order_relaxed);
            calibrationKeyHeld = calibrationKeyDown;
        }
        runtime::g_secondary_aim_hotkey_index.store(-1);
        runtime::g_active_hotkey_index.store(next_active);
        aiming.store(next_active >= 0);

        macros::tick();

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    macros::shutdown();
}
