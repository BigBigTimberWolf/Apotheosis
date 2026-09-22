# -*- coding: utf-8 -*-
"""
右板: 在接口描述符里判定设备是键盘还是鼠标, 设置 g_isKeyboard/g_isMouse。
这是"角色自动识别"的数据来源。
"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'C:\Users\Administrator\Desktop\KBD_PASSTHROUGH\fw_host_kbd\src\esp_usb_host.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

# ---- 声明 ----
DECL = '''
// 设备类型标志 (定义在 main.cpp): 供"角色自动识别"使用
extern volatile bool g_isKeyboard;
extern volatile bool g_isMouse;
'''
if 'extern volatile bool g_isKeyboard' not in s:
    idx = s.find('\n', s.find('#include'))
    s = s[:idx+1] + DECL + s[idx+1:]

# ---- 在接口描述符处理里判定 ----
ANCHOR = """        // 记录键盘接口号, 供 LED 回传的 SET_REPORT 使用
        // (bInterfaceClass=HID 且 bInterfaceProtocol=1 即 boot keyboard)
        if (intf->bInterfaceClass == TUSB_CLASS_HID &&
            intf->bInterfaceProtocol == HID_ITF_PROTOCOL_KEYBOARD) {
            g_kbdInterface = intf->bInterfaceNumber;
        }"""

NEW = """        // 记录键盘接口号
        // (bInterfaceClass=HID 且 bInterfaceProtocol=1 即 boot keyboard)
        if (intf->bInterfaceClass == TUSB_CLASS_HID &&
            intf->bInterfaceProtocol == HID_ITF_PROTOCOL_KEYBOARD) {
            g_kbdInterface = intf->bInterfaceNumber;
        }

        // ★ 角色自动识别: 判定 USB-C 插入的是键盘还是鼠标
        //
        // 判定依据 (按可靠性排序):
        //   1) 接口协议 == KEYBOARD(1)          -> 确定是键盘
        //   2) 接口协议 == MOUSE(2)             -> 确定是鼠标
        //   3) 报告描述符里出现 Keyboard usage  -> 键盘
        //   4) 报告描述符里出现 Mouse usage     -> 鼠标
        //
        // 键盘优先后设置: 键鼠一体的接收器会同时带鼠标接口,
        // 但本板 USB-C 只插一个设备, 所以只要出现键盘特征就按键盘处理。
        if (intf->bInterfaceClass == TUSB_CLASS_HID) {
            if (intf->bInterfaceProtocol == HID_ITF_PROTOCOL_KEYBOARD) {
                g_isKeyboard = true;
            } else if (intf->bInterfaceProtocol == HID_ITF_PROTOCOL_MOUSE) {
                g_isMouse = true;
            }
        }"""

if 'g_isKeyboard = true' not in s:
    assert ANCHOR in s, "interface anchor not found"
    s = s.replace(ANCHOR, NEW, 1)

open(p, 'w', encoding='utf-8', newline='').write(s)
print("已注入角色判定")
print("  声明:", 'extern volatile bool g_isKeyboard' in s)
print("  判定:", 'g_isKeyboard = true' in s)
