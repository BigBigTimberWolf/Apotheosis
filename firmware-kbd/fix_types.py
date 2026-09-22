# -*- coding: utf-8 -*-
"""修正: 类型定义引入 + 清理 USBSetup.h 的 Arduino USB 残留"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

root = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device'

# ---------- 1) usb_desc.cpp 加 InitSettings.h ----------
p = root + r'\src\usb_desc.cpp'
s = open(p, encoding='utf-8', errors='replace').read()
if '#include "InitSettings.h"' not in s:
    s = s.replace('#include "usb_desc.h"',
                  '#include "usb_desc.h"\n#include "InitSettings.h"   // DeviceInfo / DescriptorDevice', 1)
    open(p, 'w', encoding='utf-8', newline='').write(s)
    print("[1] usb_desc.cpp: 已引入 InitSettings.h")

# ---------- 2) 清理 USBSetup.h ----------
p2 = root + r'\include\USBSetup.h'
s2 = open(p2, encoding='utf-8', errors='replace').read()

old = """#include <Arduino.h>
#include <USB.h>
#include <USBHIDMouse.h>
#include <USBHIDKeyboard.h>
#include "InitSettings.h"
#include "ClonedHID.h"

extern USBHIDMouse Mouse;
extern USBHIDKeyboard Kbd;

"""
new = """#include <Arduino.h>
#include "InitSettings.h"
#include "ClonedHID.h"

// ★ 不再使用 Arduino 的 USBHIDMouse / USBHIDKeyboard:
//   那套封装只能产生一个 HID 接口、协议固定为 NONE, 导致插键盘时被控机不认。
//   现由 usb_desc.cpp / usb_hid.cpp 直写 TinyUSB 描述符。

"""
assert old in s2, "USBSetup.h 头部块未找到"
s2 = s2.replace(old, new, 1)
open(p2, 'w', encoding='utf-8', newline='').write(s2)
print("[2] USBSetup.h: 已移除 Arduino USB 头文件与 Mouse/Kbd 声明")

# ---------- 验证: 全工程是否还有 Mouse/Kbd 的 extern 引用 ----------
import subprocess, os
print()
print("验证: 是否还有代码引用 Arduino 的 Mouse/Kbd 对象")
found = False
for dirpath, _, files in os.walk(root + r'\src'):
    for fn in files:
        if not fn.endswith(('.cpp', '.h')): continue
        fp = os.path.join(dirpath, fn)
        txt = open(fp, encoding='utf-8', errors='replace').read()
        for i, line in enumerate(txt.split('\n'), 1):
            code = line.split('//')[0]
            if ('Mouse.' in code or 'Kbd.' in code or 'USBHIDMouse' in code or 'USBHIDKeyboard' in code):
                print(f"  {fn}:{i}: {line.strip()}")
                found = True
if not found:
    print("  无残留 ✓")
