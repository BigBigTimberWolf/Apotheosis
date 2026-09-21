# -*- coding: utf-8 -*-
"""
方案 A 实现 —— 左板暴露真正的多接口 USB 复合设备

原理:
  Arduino 的 USBHID 只能产生【一个】HID 接口(TUD_HID_INOUT_DESCRIPTOR)。
  真键盘接收器是【两个接口】的复合设备(usbccgp 加载)。

  框架提供了一个空槽: USB_INTERFACE_CUSTOM
    - tinyusb_enable_interface(USB_INTERFACE_CUSTOM, LEN, my_cb)
    - 描述符装载时会回调 my_cb(dst, itf)
  => 我们可以用它写入【第二个 HID 接口】的描述符

  报告描述符的分发:
    TinyUSB 的 hid_device.c.obj 对 tud_hid_descriptor_report_cb 是【未定义引用(U)】
    => 可以用链接器 -Wl,--wrap=... 拦截
    => 在 __wrap_... 里按 instance 返回不同描述符

  注意 instance 的分配:
    接口号 由描述符顺序决定 (HID=2 先于 CUSTOM=6)
      -> Arduino HID  = 接口 0 = instance 0
      -> 我们的接口  = 接口 1 = instance 1
"""

import io, sys
sys.stdout.reconfigure(encoding='utf-8')

HEADER = r'''#ifndef DUAL_HID_H
#define DUAL_HID_H

#include <stdint.h>

// ============================================================================
// 双 HID 接口支持 (方案 A: 多接口复合设备)
//
// 目的: 让本板以【两个独立 HID 接口】的形式枚举, 使被控机用 usbccgp 加载,
//       结构上与真·键盘接收器一致(MI_00 键盘接口 + MI_01 扩展接口)。
//
// 背景: Arduino 的 USBHID 封装只能产生一个 HID 接口(见 USBHID.cpp 的
//       tusb_hid_load_descriptor, 内有 tinyusb_hid_is_initialized 单次保护),
//       鼠标与键盘的报告描述符被首尾拼接塞进同一个接口。
//       真设备是复合设备, 结构不符 -> 被控机/驱动认不出键盘。
//
// 做法: 框架的 USB_INTERFACE_CUSTOM 槽位是空的, tinyusb_enable_interface()
//       接受自定义描述符回调 -> 用它注册第二个 HID 接口。
//       报告描述符按 instance 分发, 用 -Wl,--wrap=tud_hid_descriptor_report_cb
//       拦截 TinyUSB 内部的调用(hid_device.c.obj 对该符号是未定义引用)。
// ============================================================================

// 该接口作为"接口 1"呈现(接口 0 是 Arduino 内建的 HID)。
#define DUALHID_IFACE_INDEX   1u

// 报告描述符: 第二个接口 = 纯键盘 (与真键盘的 MI_00 对齐)
// 用 TinyUSB 的模板展开, 不带 Report ID —— 单报告接口最接近真设备,
// 且主机读到的就是标准 boot keyboard 布局。
#define DUALHID_KBD_REPORT_DESC()                     \
    HID_USAGE_PAGE ( HID_USAGE_PAGE_DESKTOP     )   , \
    HID_USAGE      ( HID_USAGE_DESKTOP_KEYBOARD )   , \
    HID_COLLECTION ( HID_COLLECTION_APPLICATION )   , \
      HID_USAGE_PAGE ( HID_USAGE_PAGE_KEYBOARD )    , \
        HID_USAGE_MIN    ( 224                  )   , \
        HID_USAGE_MAX    ( 231                  )   , \
        HID_LOGICAL_MIN  ( 0                    )   , \
        HID_LOGICAL_MAX  ( 1                    )   , \
        HID_REPORT_COUNT ( 8                    )   , \
        HID_REPORT_SIZE  ( 1                    )   , \
        HID_INPUT        ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ), \
        HID_REPORT_COUNT ( 1                    )   , \
        HID_REPORT_SIZE  ( 8                    )   , \
        HID_INPUT        ( HID_CONSTANT         )   , \
      HID_USAGE_PAGE  ( HID_USAGE_PAGE_LED      )   , \
        HID_USAGE_MIN    ( 1                    )   , \
        HID_USAGE_MAX    ( 5                    )   , \
        HID_REPORT_COUNT ( 5                    )   , \
        HID_REPORT_SIZE  ( 1                    )   , \
        HID_OUTPUT       ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ), \
        HID_REPORT_COUNT ( 1                    )   , \
        HID_REPORT_SIZE  ( 3                    )   , \
        HID_OUTPUT       ( HID_CONSTANT         )   , \
      HID_USAGE_PAGE ( HID_USAGE_PAGE_KEYBOARD )    , \
        HID_USAGE_MIN    ( 0                    )   , \
        HID_USAGE_MAX    ( 255                  )   , \
        HID_LOGICAL_MIN  ( 0                    )   , \
        HID_LOGICAL_MAX  ( 255                  )   , \
        HID_REPORT_COUNT ( 6                    )   , \
        HID_REPORT_SIZE  ( 8                    )   , \
        HID_INPUT        ( HID_DATA | HID_ARRAY | HID_ABSOLUTE ), \
    HID_COLLECTION_END

// 注册第二个 HID 接口。必须在 USB.begin() 之前调用。
// 返回 true 表示注册成功。
bool dualHidBegin(void);

// 键盘报文走第二个接口(instance 1)。
// modifier + 6 键码 = 8 字节, 与标准 boot keyboard 一致。
bool dualHidSendKeyboard(uint8_t modifier, const uint8_t keys6[6]);

// 该接口是否已就绪(可用于发送)
bool dualHidKeyboardReady(void);

#endif // DUAL_HID_H
'''

open(r'fw_device\include\dual_hid.h', 'w', encoding='utf-8', newline='\n').write(HEADER)
print("written: fw_device/include/dual_hid.h")
