# -*- coding: utf-8 -*-
"""
fw_device/USBSetup.cpp 手术:
  把 Arduino USBHID 描述符层换成直写 TinyUSB 层。
  保留: 角色判定(wantKbd/wantMouse)、动态身份数据、其余全部逻辑。
"""
import re, sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\src\USBSetup.cpp'
s = open(p, encoding='utf-8', errors='replace').read()
orig_len = len(s)

# ---------- 1) include 替换 ----------
s = s.replace('#include "USBSetup.h"\n#include "dual_hid.h"\n#include "ClonedHID.h"\n#include <USBHIDMouse.h>\n#include <USBHIDKeyboard.h>\n#include <USB.h>\n#include "tusb.h"\n',
              '#include "USBSetup.h"\n#include "ClonedHID.h"\n#include "usb_desc.h"   // 直写 TinyUSB 描述符层\n#include "tusb.h"\n', 1)
assert 'dual_hid.h' not in s, "include 替换失败"
assert '#include "usb_desc.h"' in s

# ---------- 2) 删除全局 Mouse/Kbd/USB ----------
old_globals = """// 全局 HID 对象。两者只在【一个】HID 实例内, 由 Report ID 区分:
//   鼠标 = HID_REPORT_ID_MOUSE, 键盘 = HID_REPORT_ID_KEYBOARD
// 二者都经 USBHID::SendReport() -> tud_hid_n_report(0, report_id, ...),
// 所以实例号恒为 0 是正确的, 不是限制。
USBHIDMouse Mouse;
USBHIDKeyboard Kbd;
extern ESPUSB USB;
"""
assert old_globals in s, "全局对象块未找到"
s = s.replace(old_globals, """// ★ 不再使用 Arduino 的 USBHIDMouse / USBHIDKeyboard:
//   那套封装只能产生一个 HID 接口, 且接口协议固定为 NONE,
//   导致插键盘时被控机不认。现由 usb_desc.cpp / usb_hid.cpp 直写 TinyUSB 描述符。
""", 1)

# ---------- 3) 删除 MAKCUNEW 自己的 tud_descriptor_string_cb ----------
i = s.find('uint16_t const* tud_descriptor_string_cb')
assert i > 0, "字符串回调未找到"
# 找到该函数结尾: 从 i 起找第一个 "\n}\n"
j = s.find('\n}\n', i)
assert j > i, "字符串回调结尾未找到"
j += len('\n}\n')
s = s[:i] + ('// ★ tud_descriptor_string_cb 已移入 usb_desc.cpp\n'
             '//   (那里的版本同样从 device_info 取真设备的字符串)\n') + s[j:]

# ---------- 4) 删除 InitUSB 里的 USB.* 赋值块 ----------
start = s.find('    // 设置与真实鼠标一致的 VID / PID')
end   = s.find('    // ===== 只暴露真设备【实际拥有】的那一类 HID =====')
assert start > 0 and end > start, "USB.* 赋值块未找到"
s = s[:start] + """    // ★ VID/PID/类/字符串 不再通过 Arduino 的 USB.xxx() 设置。
    //   它们由 usb_desc.cpp 直接从 descriptor_device / device_info 读取并填进
    //   我们自己的描述符, 效果与原来一致(身份克隆得以保留)。

""" + s[end:]

# ---------- 5) 替换 begin/注册块 ----------
start = s.find('    // ===== 注册顺序至关重要 (Arduino 框架的硬限制) =====')
end   = s.find('    s_usb_initialized = true;\n}')
assert start > 0 and end > start, "begin/注册块未找到"
s = s[:start] + """    // ===== 启动直写 TinyUSB 描述符层 =====
    //
    // 按角色只暴露一种设备 (方案乙):
    //   键盘马克 -> 只有键盘接口 (Boot Keyboard, 无 Report ID)
    //   鼠标马克 -> 只有鼠标接口 (Boot Mouse,    无 Report ID)
    //
    // 这取代了原来经 Arduino USBHID 的那套 begin() 与注册流程。
    // Arduino 的 USB 封装不再参与枚举 —— 我们对 tud_descriptor_* 与
    // tud_hid_descriptor_report_cb 给出的强定义会直接生效。
    const int role = wantKbd ? USB_ROLE_KEYBOARD : USB_ROLE_MOUSE;
    kbdUsbBegin(role);

    // 克隆实例的注册保留(其内容由 ClonedHID.cpp 维护, 不依赖 Arduino USB)
    initClonedDevices();

""" + s[end:]

# 只检查真实【代码语句】, 注释里提到这些名字是正常的
assert '    USB.begin();' not in s, "USB.begin() 语句仍残留"
assert 'dualHidBegin' not in s, "dualHidBegin 仍残留"
assert 'dual_hid' not in s, "dual_hid 仍残留"
assert '#include <USBHIDMouse.h>' not in s, "USBHIDMouse include 仍残留"
assert '#include <USBHIDKeyboard.h>' not in s, "USBHIDKeyboard include 仍残留"
assert '#include <USB.h>' not in s, "USB.h include 仍残留"
assert 'USBHIDMouse Mouse;' not in s, "USBHIDMouse 对象仍残留"
assert 'USBHIDKeyboard Kbd;' not in s, "USBHIDKeyboard 对象仍残留"
assert 'kbdUsbBegin(role)' in s, "kbdUsbBegin 未被调用"

open(p, 'w', encoding='utf-8', newline='').write(s)
print(f"USBSetup.cpp 手术完成: {orig_len} -> {len(s)} 字节")
print()
print("验证:")
print("  dual_hid 已移除 :", 'dual_hid' not in s)
print("  Arduino USBHID 已移除:", 'USBHIDMouse' not in s and 'USBHIDKeyboard' not in s)
print("  usb_desc.h 已引入:", '#include "usb_desc.h"' in s)
print("  kbdUsbBegin 已调用:", 'kbdUsbBegin(role)' in s)
print("  角色判定已保留:", 'wantKbd' in s and 'iface_isKbd' in s)
