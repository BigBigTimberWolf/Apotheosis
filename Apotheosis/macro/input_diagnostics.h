#pragma once
#include "mouse/mouse_driver.h"
#include <string>

namespace macros {
inline std::string inputFailure(const std::string& backend, const mouse_driver::IDriver* driver,
                               bool keyboard, int code) {
    if (keyboard && code >= 0x10000 && backend != "WINDOWS")
        return u8"媒体键和 VK 通用键需要 Windows 原生输出，当前硬件不支持";
    if (!driver || !driver->isOpen())
        return backend+u8" 输入设备未连接或串口写入失败，请检查硬件连接状态";
    if (keyboard && !(driver->capabilities() & mouse_driver::kCapKeyboard)) {
        if (backend == "FERRUM")
            return u8"FE 当前为 Legacy API，仅支持鼠标；键盘宏请连接 Ferrum App 提供的 Software API 虚拟串口";
        return backend+u8" 当前连接不支持键盘输出，请检查键盘设备或固件能力";
    }
    const auto detail=driver->lastError();
    return backend+(keyboard?u8" 键盘命令失败":u8" 鼠标按钮命令失败")+
        (detail.empty()?u8"，请检查设备状态与串口响应":u8"："+detail);
}
} // namespace macros
