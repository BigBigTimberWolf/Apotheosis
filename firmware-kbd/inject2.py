# -*- coding: utf-8 -*-
"""
在 esp_usb_host.cpp 里加两个钩子:
  1) 记录键盘接口号 (供 SET_REPORT 用)
  2) 读到 USB_DEVICE_DESC 后, 把真键盘的 VID/PID 发给左板
"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'C:\Users\Administrator\Desktop\KBD_PASSTHROUGH\fw_host_kbd\src\esp_usb_host.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

# ---- 1) 声明两个外部钩子 ----
DECL = '''
// 真键盘身份上报 (实现在 main.cpp): 右板拿到描述符后告知左板
extern "C" void kbdHostSendIdentity(uint16_t vid, uint16_t pid, uint16_t bcd);
// 键盘接口号 (供 LED 回传的 SET_REPORT 使用), 定义在 led_forward.cpp
extern "C" { extern int volatile g_kbdInterface; }
'''
if 'kbdHostSendIdentity' not in s:
    idx = s.find('\n', s.find('#include'))
    s = s[:idx+1] + DECL + s[idx+1:]

# ---- 2) 在 USB_DEVICE_DESC 分支里发身份 ----
ANCHOR = """        descriptor_device.idVendor = dev_desc->idVendor;
        descriptor_device.idProduct = dev_desc->idProduct;"""
assert ANCHOR in s, "idVendor anchor not found"

INJECT = """        descriptor_device.idVendor = dev_desc->idVendor;
        descriptor_device.idProduct = dev_desc->idProduct;

        // ★ 拿到真键盘身份后立刻告知左板 —— 左板正阻塞等这一帧,
        //   收到后才会用真值去枚举, 所以这里要尽早发。
        kbdHostSendIdentity(dev_desc->idVendor,
                            dev_desc->idProduct,
                            dev_desc->bcdDevice);"""

s = s.replace(ANCHOR, INJECT, 1)

# ---- 3) 记录键盘接口号 ----
# 变量名是 intf (见 esp_usb_host.cpp:331)
IFACE_ANCHOR = "const usb_intf_desc_t *intf = (const usb_intf_desc_t *)p;"
assert IFACE_ANCHOR in s, "intf anchor not found"

if 'g_kbdInterface = ' not in s:
    add = IFACE_ANCHOR + """

        // 记录键盘接口号, 供 LED 回传的 SET_REPORT 使用
        // (bInterfaceClass=HID 且 bInterfaceProtocol=1 即 boot keyboard)
        if (intf->bInterfaceClass == TUSB_CLASS_HID &&
            intf->bInterfaceProtocol == HID_ITF_PROTOCOL_KEYBOARD) {
            g_kbdInterface = intf->bInterfaceNumber;
        }"""
    s = s.replace(IFACE_ANCHOR, add, 1)

open(p, 'w', encoding='utf-8', newline='').write(s)
print("已注入: 身份上报 + 键盘接口记录")
print()
print("  身份钩子:", 'kbdHostSendIdentity(' in s)
print("  接口记录:", 'g_kbdInterface = ' in s)
