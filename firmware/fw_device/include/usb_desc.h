// ============================================================================
// usb_desc.h —— 直写 TinyUSB 描述符层 (替代 Arduino USBHID)
//
// 【为什么替换】
//   MAKCUNEW 的 fw_device 原来用 Arduino 的 USBHIDMouse/USBHIDKeyboard。
//   那套封装只能产生【一个】HID 接口, 且:
//     - 接口协议固定为 HID_ITF_PROTOCOL_NONE (不是 Boot Mouse)
//     - 报告描述符是运行时 malloc 拼接的
//   结果: 被控机侧的身份与形状都与真设备对不上。
//
//   本文件直接提供 TinyUSB 的强定义回调, 完全绕开 Arduino 的 USB 封装。
//   (Arduino 核心里这三个回调都是 __attribute__((weak)), 强定义可直接覆盖,
//    不需要 --wrap, 也不需要改框架源码。)
//
// 【本板只有鼠标角色】
//   左板(fw_device)只做鼠标: 恒定枚举一个 Boot Mouse 接口 (4 字节报文)。
//   曾经存在过"按真设备类型在键盘/鼠标描述符之间二选一"的角色机制, 已删除;
//   键盘由独立的 KBD_PASSTHROUGH 固件负责。
// ============================================================================

#ifndef USB_DESC_H
#define USB_DESC_H

#include <stdint.h>

// 设备角色。本板只有一个角色, 保留该常量是为了让 kbdUsbBegin() 的意图明确。
#define USB_ROLE_MOUSE      2

// 端点
#define EPNUM_HID_IN        0x81

// 报告描述符长度 (逐个字节核算, 不要手改)
#define DESC_MOUSE_LEN      52

// 初始化 USB 设备栈, 恒定以【鼠标】角色枚举。
//   必须在任何 USB 活动之前调用 (由 InitUSB() 调用)。
// 注意: 设备身份 (VID/PID/字符串) 由 usb_desc.cpp 从
//       descriptor_device / device_info 里读取, 不通过参数传。
void kbdUsbBegin(void);

// 主机是否已就绪
bool kbdUsbReady(void);

// 发送鼠标报文 (按键 + 相对位移 + 滚轮)
bool kbdUsbSendMouse(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel);

// 由 USBSetup.cpp 提供: 真设备的 HID 报告描述符 (fw_host 通过
// USB_sendRawHidDescriptors 送来, receiveRealHidDescriptor 解析并存下)。
// 返回 nullptr 表示未就绪 -> 走内置 s_descMouse (52 字节 Boot Mouse)。
#ifdef __cplusplus
extern "C" {
#endif
const uint8_t *clonedMouseDesc(uint16_t *lenOut);
#ifdef __cplusplus
}
#endif

#endif // USB_DESC_H
