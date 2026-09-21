# -*- coding: utf-8 -*-
"""生成 left2 / dual_hid.cpp —— 第二个 HID 接口的实现"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

CPP = r'''// ============================================================================
// dual_hid.cpp —— 第二个 HID 接口 (方案 A: 多接口复合设备)
//
// 见 include/dual_hid.h 的总体说明。这里给出关键实现细节:
//
// 【1】为什么用 USB_INTERFACE_CUSTOM
//   tinyusb_enable_interface() 对每种 interface_t 只允许注册一次:
//       if (tinyusb_loaded_interfaces_mask & (1U << interface)) return ESP_FAIL;
//   USB_INTERFACE_HID 已被 Arduino 的 USBHID 构造函数占用, 无法再注册。
//   而 USB_INTERFACE_CUSTOM 是空槽, 且描述符装载器会回调我们传入的函数:
//       tinyusb_load_descriptor(interface, dst, itf)
//         -> tinyusb_loaded_interfaces_callbacks[interface](dst, itf)
//
// 【2】接口编号
//   tinyusb_load_enabled_interfaces() 按 interface_t 枚举值升序装载:
//       MSC=0 DFU=1 HID=2 VENDOR=3 CDC=4 MIDI=5 CUSTOM=6
//   HID(2) 先于 CUSTOM(6), 所以 Arduino HID 拿到接口 0;
//   我们在回调里用递增的 *itf 拿到接口 1。
//
// 【3】报告描述符按 instance 分发
//   TinyUSB hid_device.c.obj 对 tud_hid_descriptor_report_cb 是未定义引用,
//   因此可以用 -Wl,--wrap 拦截。Arduino 的原实现忽略 instance 参数, 两个接口
//   都会拿到同一份拼接描述符 —— 那会让第二个接口的 collection 与声明长度不符。
//   这里按 instance 返回各自真正的描述符。
//
// 【4】端点
//   该接口只需要一个 IN 端点(键盘是纯输入)。用 tinyusb_get_free_in_endpoint()
//   申请, 与 Arduino HID 已占用的端点不会冲突(框架维护占用位图)。
// ============================================================================

#include "dual_hid.h"

#include <Arduino.h>
#include <string.h>

#include "tusb.h"
#include "class/hid/hid_device.h"
#include "esp32-hal-tinyusb.h"

// ---- 第二个接口的报告描述符 (纯键盘, 无 Report ID) ----
static const uint8_t s_kbd_report_desc[] = {
    DUALHID_KBD_REPORT_DESC()
};

// ---- 运行状态 ----
static uint8_t s_iface_num   = 0xFF;   // 本接口的接口号(由 itf 递增得到)
static uint8_t s_ep_in       = 0;      // 本接口的 IN 端点号
static bool    s_registered  = false;  // 描述符回调是否已执行
static bool    s_ready       = false;  // 接口是否已就绪(主机已配置)

// ============================================================================
// 描述符回调: 由框架在 USB.begin() 时调用, 写入我们的接口描述符
// ============================================================================
extern "C" uint16_t dualHidLoadDescriptor(uint8_t *dst, uint8_t *itf)
{
    // 只允许装载一次(框架本身也只调用一次, 这里防御性检查)
    if (s_registered) {
        return 0;
    }

    // 申请一个 IN 端点。键盘是纯输入设备, 不需要 OUT 端点。
    // 注意: 必须在 USB 初始化【之前】申请, 否则位图已冻结。
    uint8_t ep_in = tinyusb_get_free_in_endpoint();
    if (ep_in == 0) {
        Serial0.println("[DUALHID] no free IN endpoint!");
        return 0;
    }

    const uint8_t str_index = tinyusb_add_string_descriptor("MAKCU Keyboard");

    // TUD_HID_DESCRIPTOR 字段顺序:
    //   itf, str_idx, protocol, report_desc_len, ep_in, ep_size, polling_interval
    //
    // protocol 用 HID_ITF_PROTOCOL_KEYBOARD —— 与真键盘的 MI_00 一致
    // (Class_03 / SubClass_01 / Prot_01), 让主机按 boot keyboard 处理。
    //
    // bInterval = 1 (1ms) —— 与真接收器一致, 保证 1000Hz 上报。
    const uint8_t descriptor[TUD_HID_DESC_LEN] = {
        TUD_HID_DESCRIPTOR(*itf, str_index, HID_ITF_PROTOCOL_KEYBOARD,
                           (uint16_t)sizeof(s_kbd_report_desc),
                           ep_in, 64, 1)
    };

    memcpy(dst, descriptor, TUD_HID_DESC_LEN);

    s_iface_num  = *itf;
    s_ep_in      = ep_in;
    s_registered = true;

    *itf += 1;      // 占用一个接口号

    return TUD_HID_DESC_LEN;
}

// ============================================================================
// 按 instance 分发报告描述符
//
// 用 -Wl,--wrap=tud_hid_descriptor_report_cb 拦截。
// instance 0 = Arduino 的拼接 HID(鼠标+键盘), instance 1 = 本接口(纯键盘)。
//
// 注意: Arduino 的原实现是 __real_..., 我们不能直接用它的返回值来决定
// instance 1, 因为那会返回同一份拼接描述符。这里显式区分。
// ============================================================================
extern "C" uint8_t const *__real_tud_hid_descriptor_report_cb(uint8_t instance);

extern "C" uint8_t const *__wrap_tud_hid_descriptor_report_cb(uint8_t instance)
{
    // 接口 1 = 我们的纯键盘接口
    if (s_registered && instance == 1) {
        return s_kbd_report_desc;
    }
    // 其余(接口 0)交回 Arduino 原实现
    return __real_tud_hid_descriptor_report_cb(instance);
}

// ============================================================================
// 就绪判定
// ============================================================================
bool dualHidKeyboardReady(void)
{
    if (!s_registered) return false;
    return tud_ready() && tud_hid_n_ready(1);
}

// ============================================================================
// 键盘报文发送 (走 instance 1)
// ============================================================================
bool dualHidSendKeyboard(uint8_t modifier, const uint8_t keys6[6])
{
    if (!s_registered) return false;
    if (!tud_ready())  return false;

    // 标准 boot keyboard 输入报文: modifier + reserved + 6 键码 = 8 字节
    // 用 hid_keyboard_report_t 保证布局与描述符一致。
    hid_keyboard_report_t report;
    report.modifier = modifier;
    report.reserved = 0;
    memcpy(report.keycode, keys6, 6);

    // 无 Report ID -> 传 0
    return tud_hid_n_report(1, 0, &report, sizeof(report));
}

// ============================================================================
// 注册 (必须在 USB.begin() 之前调用)
// ============================================================================
bool dualHidBegin(void)
{
    if (s_registered) return true;

    // 描述符长度用 TUD_HID_DESC_LEN —— 框架按此累加配置描述符总长。
    esp_err_t err = tinyusb_enable_interface(USB_INTERFACE_CUSTOM,
                                             TUD_HID_DESC_LEN,
                                             dualHidLoadDescriptor);
    if (err != ESP_OK) {
        Serial0.printf("[DUALHID] enable_interface failed: %d\n", (int)err);
        return false;
    }

    Serial0.println("[DUALHID] custom interface registered (kbd as iface 1)");
    return true;
}
'''

open(r'fw_device\src\dual_hid.cpp', 'w', encoding='utf-8', newline='\n').write(CPP)
print("written: fw_device/src/dual_hid.cpp")
