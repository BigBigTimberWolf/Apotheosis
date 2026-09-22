#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <windows.h>
#include <iostream>

#include "Makcu.h"

namespace
{
makcu::MouseButton mouseButtonFromChannel(int button)
{
    switch (button)
    {
    case 2: return makcu::MouseButton::RIGHT;
    case 3: return makcu::MouseButton::MIDDLE;
    case 4: return makcu::MouseButton::SIDE1;
    case 5: return makcu::MouseButton::SIDE2;
    case 1:
    default:return makcu::MouseButton::LEFT;
    }
}
}

MakcuConnection::MakcuConnection(const std::string& port, unsigned int baud_rate)
    : is_open_(false)
    , aiming_active(false)
    , shooting_active(false)
    , zooming_active(false)
    , side1_active(false)
    , side2_active(false)
    , middle_active(false)
{
    try
    {
        device_.setMouseButtonCallback([this](makcu::MouseButton button, bool pressed) {
            onButtonCallback(button, pressed);
        });

        if (device_.connect(port))
        {
            if (baud_rate > 0)
            {
                if (!device_.setBaudRate(baud_rate, true))
                {
                    std::cerr << "[Makcu] Failed to set baud rate to " << baud_rate
                        << ", continuing with current baud rate." << std::endl;
                }
            }

            // ★ 必须在 connect() 之后才启用按键监控.
            //
            // Device::enableButtonMonitoring 内部一开始就检查 connected 标志, 未连接
            // 就 early-return false 什么都不发. 老实现在 connect 之前调用它, 相当于
            // no-op —— 固件从没收到 km.buttons(1), g_buttonMonitoringEnabled 保持
            // false, 真按键掩码不推送 -> 上位机 shooting_active 等永远是 false ->
            // 鼠标热键 (LeftMouseButton / RightMouseButton / ...) 判定不到按下 ->
            // g_active_hotkey_index 永远是 -1 -> aim_loop 直接 return, 按热键完全
            // 不移动.
            //
            // setBaudRate 之后调是因为切波特率会 close+reopen 串口, 期间任何命令
            // 都是白发; 波特率稳定下来再开监控才可靠.
            if (!device_.enableButtonMonitoring(true))
            {
                std::cerr << "[Makcu] Failed to enable button monitoring; mouse "
                             "hotkeys (Left/Right/Middle/Side) will not work."
                          << std::endl;
            }

            is_open_ = true;
            std::cout << "[Makcu] Connected! PORT: " << port << std::endl;
        }
        else
        {
            std::cerr << "[Makcu] Unable to connect to the port: " << port << std::endl;
        }
    }
    catch (const makcu::MakcuException& e)
    {
        std::cerr << "[Makcu] Error: " << e.what() << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Makcu] Error: " << e.what() << std::endl;
    }
}

MakcuConnection::~MakcuConnection()
{
    try
    {
        device_.disconnect();
    }
    catch (...)
    {
    }
    is_open_ = false;
}

bool MakcuConnection::isOpen() const
{
    return is_open_ && device_.isConnected();
}

void MakcuConnection::move(int x, int y)
{
    if (!is_open_)
        return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        device_.mouseMove(x, y);
    }
    catch (...)
    {
        is_open_ = false;
    }
}

void MakcuConnection::click(int button)
{
    if (!is_open_)
        return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        device_.click(mouseButtonFromChannel(button));
    }
    catch (...)
    {
        is_open_ = false;
    }
}

void MakcuConnection::press(int button)
{
    if (!is_open_)
        return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        device_.mouseDown(mouseButtonFromChannel(button));
    }
    catch (...)
    {
        is_open_ = false;
    }
}

void MakcuConnection::release(int button)
{
    if (!is_open_)
        return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        device_.mouseUp(mouseButtonFromChannel(button));
    }
    catch (...)
    {
        is_open_ = false;
    }
}

void MakcuConnection::onButtonCallback(makcu::MouseButton button, bool pressed)
{
    switch (button)
    {
    case makcu::MouseButton::LEFT:
        shooting_active = pressed;
        break;

    case makcu::MouseButton::RIGHT:
        zooming_active = pressed;
        break;

    case makcu::MouseButton::MIDDLE:
        middle_active = pressed;
        break;

    case makcu::MouseButton::SIDE1:
        side1_active = pressed;
        break;

    case makcu::MouseButton::SIDE2:
        side2_active = pressed;
        break;
    }
}

void MakcuConnection::wheel(int delta)
{
    if (!is_open_)
        return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        device_.mouseWheel(delta);
    }
    catch (...)
    {
        is_open_ = false;
    }
}
