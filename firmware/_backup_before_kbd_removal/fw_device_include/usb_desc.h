// ============================================================================
// usb_desc.h —— 直写 TinyUSB 描述符层 (替代 Arduino USBHID)
//
// 【为什么替换】
//   MAKCUNEW 的 fw_device 原来用 Arduino 的 USBHIDMouse/USBHIDKeyboard。
//   那套封装只能产生【一个】HID 接口, 且:
//     - 接口协议固定为 HID_ITF_PROTOCOL_NONE (不是 Boot Keyboard)
//     - 报告描述符是运行时 malloc 拼接的
//     - 无法按角色只暴露一种设备
//   结果: 插键盘时被控机不认键盘。
//
//   本文件直接提供 TinyUSB 的强定义回调, 完全绕开 Arduino 的 USB 封装。
//   (Arduino 核心里这三个回调都是 __attribute__((weak)), 强定义可直接覆盖,
//    不需要 --wrap, 也不需要改框架源码。)
//
// 【按角色二选一 (方案乙)】
//   键盘马克 -> 只有键盘接口 (Boot Keyboard)
//   鼠标马克 -> 只有鼠标接口 (Boot Mouse)
//   各自只有一个 Collection, 因此【不需要 Report ID】——
//   这与真实的 boot keyboard / boot mouse 形状完全一致。
// ============================================================================

#ifndef USB_DESC_H
#define USB_DESC_H

#include <stdint.h>

// 设备角色
#define USB_ROLE_KEYBOARD   1
#define USB_ROLE_MOUSE      2

// 端点
#define EPNUM_HID_IN        0x81

// 报告描述符长度 (由 check_desc.py 逐个字节核算, 不要手改)
#define DESC_KBD_LEN        63
#define DESC_MOUSE_LEN      52

// 初始化 USB 设备栈。
//   role 决定暴露哪一种描述符。
//   必须在任何 USB 活动之前调用 (由 InitUSB() 调用)。
// 注意: 设备身份 (VID/PID/字符串) 由 usb_desc.cpp 从
//       descriptor_device / device_info 里读取, 不通过参数传。
void kbdUsbBegin(int role);

// 主机是否已就绪
bool kbdUsbReady(void);

// 本板当前是否以【键盘】角色枚举。
//
// 用途: 左板的 HID 接口是二选一的 —— 键盘角色下是 8 字节 boot 键盘, 鼠标角色下
// 是 4 字节鼠标。右板若在鼠标单板上误发来一帧 8 字节键盘报文, 直接写进 4 字节的
// 鼠标接口会让被控机读到错位数据(表现为鼠标乱动/乱按键)。发送前必须用它互锁。
bool kbdUsbIsKeyboardRole(void);

// 发送键盘报文 (8 字节: modifier + reserved + key0..key5)
bool kbdUsbSendKeyboard(const uint8_t *report8);

// 发送鼠标报文 (按键 + 相对位移 + 滚轮)
bool kbdUsbSendMouse(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel);

#endif // USB_DESC_H
