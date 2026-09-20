#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "capture.h"
#include "config.h"
#include "keyboard_listener.h"
#include "keycodes.h"
#include "Makcu.h"
#include "MakcuNew.h"
#include "mouse.h"
#include "runtime/active_hotkey.h"
#include "Apotheosis.h"
#include "runtime/config_snapshot.h"

extern std::atomic<bool> shouldExit;
extern std::atomic<bool> aiming;

namespace
{

bool win32_key_pressed(int vk_code)
{
    return (GetAsyncKeyState(vk_code) & 0x8000) != 0;
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
        if (key_name.empty() || key_name.find(u8"始终活跃") != std::string::npos)
            return true;

        bool pressed = false;

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

        if (pressed) return true;
    }
    return false;
}

void keyboardListener()
{
    while (!shouldExit)
    {
        int next_active = -1;
        {
            std::lock_guard<std::recursive_mutex> cfg(configMutex);
            const auto& ag = config.active_hotkey_group;
            for (size_t i = 0; i < config.hotkeys.size(); ++i)
            {
                if (config.hotkeys[i].group != ag)
                    continue;
                if (isAnyKeyPressed(config.hotkeys[i].keys))
                {
                    next_active = static_cast<int>(i);
                    break;
                }
            }
        }
        runtime::g_active_hotkey_index.store(next_active);
        aiming.store(next_active >= 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
