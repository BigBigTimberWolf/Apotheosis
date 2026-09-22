// ============================================================================
// kbd_usb.h —— 左板 USB Device: TinyUSB 原生复合 HID 键盘
//
// 【为什么不用 Arduino 的 USBHID】
//   Arduino 的 USBHID 封装只能产生【一个】HID 接口, 而且它的报告描述符是把
//   各设备的描述符首尾拼接塞进同一个接口, 接口协议固定为 NONE。
//   真键盘接收器是【多接口复合设备】(Windows 用 usbccgp 加载), 接口协议是
//   Boot Keyboard (Class_03/SubClass_01/Prot_01)。结构不符 -> 被控机不认键盘。
//
//   而且 Arduino 的 tusb_hid_load_descriptor 只能注册一次(USB_INTERFACE_HID
//   槽被占用), 无法再加接口。
//
// 【本实现的做法】
//   完全绕开 Arduino 的 USB 库, 直接使用 TinyUSB 原生 API:
//     - 自己注册两个 HID 接口 (键盘 + 鼠标), 各自独立的报告描述符
//     - 自己提供 tud_hid_descriptor_report_cb / get_report / set_report
//     - 设备描述符 bDeviceClass = 0x00 (由接口定义) -> Windows 用 usbccgp
//
// 【重要前提】本板不使用 Arduino 的 USBHID / USB 对象, 因此必须确保
//   Arduino 的 USB 库不被链接进来(否则重复定义)。见 usb_desc.cpp 的说明。
// ============================================================================

#ifndef KBD_USB_H
#define KBD_USB_H

#include <stdint.h>

// 初始化 USB 设备栈 (在 setup() 里调用一次)
void kbdUsbBegin(void);

// 由主循环调用, 处理 USB 事件
void kbdUsbTask(void);

// 把真键盘的原始报文发到【键盘接口】
// payload/len 就是真键盘的原始 Report 字节, 原样发送, 不做任何解析。
// 返回 true 表示已交给 USB 栈。
bool kbdUsbSendReport(const uint8_t *payload, uint8_t len);

// 主机是否已就绪(可发送)
bool kbdUsbReady(void);

#endif // KBD_USB_H
