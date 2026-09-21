# -*- coding: utf-8 -*-
"""接线: InitUSB 里注册第二接口 + platformio.ini 加 --wrap"""
import sys, re
sys.stdout.reconfigure(encoding='utf-8')

# ---------- 1) USBSetup.cpp: include + 注册调用 ----------
p = r'fw_device\src\USBSetup.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

if 'dual_hid.h' not in s:
    s = s.replace('#include "USBSetup.h"', '#include "USBSetup.h"\n#include "dual_hid.h"', 1)
    print("[1] include dual_hid.h")

# 注册: 紧跟 Kbd.begin() 之后(此时 USB 还没 begin)
OLD = """    if (wantMouse) {
        Mouse.begin();
    }
"""
NEW = """    if (wantMouse) {
        Mouse.begin();
    }

    // ===== 第二个 HID 接口 (纯键盘, 接口 1) =====
    // 目的: 让本板枚举成【多接口复合设备】, 被控机用 usbccgp 加载,
    //       结构与真实键盘接收器一致。
    // 位置: 必须在本函数内、USB.begin() 之前 —— tinyusb_enable_interface()
    //       在 tinyusb_is_initialized 之后会直接失败。
    // 说明: 只注册【描述符】。数据通路(键盘报文走 instance 1)在
    //       handleCommands 的发射路径上, 由 dualHidSendKeyboard() 承担。
    dualHidBegin();
"""
assert OLD in s, "wantMouse block not found"
if 'dualHidBegin' not in s:
    s = s.replace(OLD, NEW, 1)
    print("[2] dualHidBegin() 已插入 InitUSB")
open(p, 'w', encoding='utf-8', newline='').write(s)

# ---------- 2) platformio.ini: 加 --wrap ----------
p2 = r'fw_device\platformio.ini'
s2 = open(p2, encoding='utf-8', errors='replace').read()

if 'wrap=tud_hid_descriptor_report_cb' not in s2:
    OLD2 = """build_flags = 
  -DUSB_IS_DEBUG=false ;  true
  -DFIRMWARE_VERSION="CUSTOM_V1_0"
  -O2"""
    NEW2 = """build_flags = 
  -DUSB_IS_DEBUG=false ;  true
  -DFIRMWARE_VERSION="CUSTOM_V1_0"
  -O2
  ; ★ 方案A: 按 instance 分发 HID 报告描述符。
  ; TinyUSB 的 hid_device.c.obj 对 tud_hid_descriptor_report_cb 是未定义引用,
  ; 用 --wrap 把内部调用重定向到 __wrap_tud_hid_descriptor_report_cb,
  ; 从而让【接口1(纯键盘)】返回自己的描述符而不是 Arduino 的拼接描述符。
  -Wl,--wrap=tud_hid_descriptor_report_cb"""
    assert OLD2 in s2, "build_flags block not found"
    s2 = s2.replace(OLD2, NEW2, 1)
    print("[3] platformio.ini 已加 -Wl,--wrap=tud_hid_descriptor_report_cb")
else:
    print("[3] wrap 已存在")

open(p2, 'w', encoding='utf-8', newline='').write(s2)
print("done")
