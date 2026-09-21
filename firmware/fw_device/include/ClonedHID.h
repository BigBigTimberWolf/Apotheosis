#pragma once
// ============================================================================
// ClonedHID.h - 报告描述符克隆
//
// 背景(这是本文件存在的原因, 也是最初设计走弯路的教训):
//
//   Arduino 的 USBHID 框架【整个设备只创建一个 HID 接口】。注册多个
//   USBHIDDevice(内置鼠标/键盘, 或自己写的)并不会产生多个接口, 它们的报告
//   描述符会被【首尾拼接】成一大块, 由 tud_hid_descriptor_report_cb() 整体上报:
//
//       uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance) {
//           return tinyusb_hid_device_descriptor;   // 一整块, 鼠标段+键盘段
//       }
//
//   所以"再注册一个克隆设备"是无效的 —— 它只会让拼接缓冲多一段, 且
//   CFG_TUD_HID=1(单实例)也装不下。
//
//   正确的做法: 直接替换那块拼接缓冲【鼠标段】的内容为真设备的字节流。
//
// 缓冲区长度的硬限制:
//   tinyusb_hid_device_descriptor = malloc(各设备描述符长度之和) 在 USB.begin()
//   时一次分配, 之后【不能改变长度】。真游戏鼠标的描述符(100~200 字节)往往
//   比内置鼠标(52 字节)长, 直接覆盖会溢出。
//
//   因此采用"先量长度, 再重启"的两阶段流程:
//     第 1 次开机: 用内置描述符注册, 从 fw_host 拿到真描述符 -> 把长度存进 NVS
//                  -> 重启
//     第 2 次开机: 从 NVS 读出长度, 按该长度预留缓冲并用真描述符填充
//                  -> 被控机读到的就是真设备的描述符
//
//   重启对 MCU 只是 1 秒的事, 且只在【首次见到该设备】时发生一次。
// ============================================================================

#include <Arduino.h>
#include <USBHID.h>

// 单段描述符的最大长度。真设备的报告描述符极少超过 512 字节
// (标准鼠标 ~50, 带宏键的游戏鼠标 ~120~250, 最复杂的 ~400)。
#define CLONE_DESC_MAX 512

// 报文组装规则(从真设备描述符解析而来)
struct ClonedReportLayout {
    bool     valid = false;
    bool     hasReportId = false;
    uint8_t  reportId = 0;

    // 鼠标
    // isKeyboard: 由 parse() 按报告描述符的 Usage 判定, 用于把"真键盘接口"的
    //   描述符识别出来并【忽略】(本板只做鼠标, 见 USBSetup.cpp)。
    bool     isKeyboard = false;
    uint8_t  buttonByte = 0;
    bool     hasButtons = false;
    uint8_t  xByte = 0, yByte = 0;
    uint8_t  xBits = 8, yBits = 8;
    bool     hasX = false, hasY = false;
    bool     interleaved = false;   // 12 位半字节交织
    uint8_t  wheelByte = 0;
    bool     hasWheel = false;
    uint8_t  reportLen = 0;

    // 键盘描述符的分类信息(本板不再用它组装键盘报文, 仅用于识别并忽略该接口)
    uint8_t  modifierByte = 0;
    bool     hasModifier = false;
    uint8_t  keyArrayByte = 0;
    uint8_t  keyArrayCount = 0;
};

namespace cloned {
    // 解析真设备的 HID 报告描述符 -> 报文组装规则
    ClonedReportLayout parse(uint8_t *data, int length);

    // 按规则组装报文。outLen 应为 layout.reportLen。
    // 只有鼠标一条路: 键盘报告报文在本板上不存在(见 ClonedReportLayout::isKeyboard)。
    void buildMouseReport(const ClonedReportLayout &L, uint8_t *out, uint8_t outLen,
                          int dx, int dy, int wheel, uint8_t buttons);
}
