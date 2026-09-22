# -*- coding: utf-8 -*-
"""更新 fw_device 构建配置: 切到直写 TinyUSB 模式"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

# ---------- 1) platformio.ini ----------
p = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\platformio.ini'
s = open(p, encoding='utf-8', errors='replace').read()

old = """  ; ★ 方案A: 按 instance 分发 HID 报告描述符。
  ; TinyUSB 的 hid_device.c.obj 对 tud_hid_descriptor_report_cb 是未定义引用,
  ; 用 --wrap 把内部调用重定向到 __wrap_tud_hid_descriptor_report_cb,
  ; 从而让【接口1(纯键盘)】返回自己的描述符而不是 Arduino 的拼接描述符。
  ; 双HID接口开关: 0=关闭(回到单接口基线), 1=启用第二个HID接口。
  ; 出问题时先设 0 确认基线, 再逐步开启定位。
  -DFW_DUALHID_ENABLE=0
  -Wl,--wrap=tud_hid_descriptor_report_cb"""

new = """  ; ★ 直写 TinyUSB 描述符层 (取代 Arduino USBHID)
  ;
  ; 为什么要关掉 Arduino 的 USB:
  ;   ARDUINO_USB_CDC_ON_BOOT=1(板子默认) 会让 arduino-esp32 自建 USB 栈,
  ;   用【它自己的】描述符和 HID 回调完成枚举, 我们 usb_desc.cpp / usb_hid.cpp
  ;   的强定义会被绕过 —— 设备仍以单接口 HidUsb 出现, 键盘不被识别。
  ;   置 0 后 Arduino 完全不碰 USB, 完全交给我们的 tinyusb_init()。
  ;   (调试仍走 USB-B 的 CH343 = UART0, 不受影响)
  -DARDUINO_USB_CDC_ON_BOOT=0
  ; 关掉 CDC_ON_BOOT 后 UART0 的对象名从 Serial0 变为 Serial。
  ; MAKCUNEW 有 300+ 处 Serial0, 用宏统一映射, 不改动业务代码。
  ; (含 Serial0 的标识符如 handleSerial0Speed 会成对改名, 语义一致)
  -DSerial0=Serial"""

assert old in s, "旧 build_flags 块未找到"
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print("[1] platformio.ini 已更新")

# ---------- 2) tusb_config.h ----------
p2 = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\include\tusb_config.h'
s2 = open(p2, encoding='utf-8', errors='replace').read()
s2 = s2.replace('#define CFG_TUD_HID                 2   // 鼠标 + 键盘 复合HID',
                '#define CFG_TUD_HID                 1   // 单接口(键盘或鼠标, 按角色二选一)')
open(p2, 'w', encoding='utf-8', newline='').write(s2)
print("[2] tusb_config.h: CFG_TUD_HID = 1")

# ---------- 验证 ----------
s = open(p, encoding='utf-8', errors='replace').read()
print()
print("验证:")
print("  旧的 --wrap 已移除  :", 'wrap=tud_hid_descriptor_report_cb' not in s)
print("  FW_DUALHID 已移除   :", 'FW_DUALHID' not in s)
print("  CDC_ON_BOOT=0 已加  :", 'ARDUINO_USB_CDC_ON_BOOT=0' in s)
print("  Serial0 宏已加      :", '-DSerial0=Serial' in s)
