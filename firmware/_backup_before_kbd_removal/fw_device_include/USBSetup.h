#pragma once

#include <Arduino.h>
#include "InitSettings.h"
#include "ClonedHID.h"

// ★ 不再使用 Arduino 的 USBHIDMouse / USBHIDKeyboard:
//   那套封装只能产生一个 HID 接口、协议固定为 NONE, 导致插键盘时被控机不认。
//   现由 usb_desc.cpp / usb_hid.cpp 直写 TinyUSB 描述符。

extern DeviceInfo device_info;
extern DescriptorDevice descriptor_device;
extern usb_endpoint_descriptor_t endpoint_descriptors[MAX_ENDPOINT_DESCRIPTORS];
extern uint8_t endpointCounter;
extern usb_interface_descriptor_t interface_descriptors[MAX_INTERFACE_DESCRIPTORS];
extern uint8_t interfaceCounter;
extern usb_hid_descriptor_t hid_descriptors[MAX_HID_DESCRIPTORS];
extern uint8_t hidDescriptorCounter;
extern usb_iad_desc_t descriptor_interface_association;
extern endpoint_data_t endpoint_data_list[17];
extern usb_unknown_descriptor_t unknown_descriptors[MAX_UNKNOWN_DESCRIPTORS];
extern uint8_t unknownDescriptorCounter;
extern DescriptorConfiguration configuration_descriptor;

extern volatile bool deviceConnected;

void requestUSBDescriptors();
void InitUSB();
bool isUsbReadyToTransfer();

// ---- 真设备描述符的接收(诊断 + 报文格式记录) ----
//
// 注意: 这些函数【不会】改变被控机看到的描述符。原因见 USBSetup.cpp 里的
// 详细说明 —— Arduino 框架不允许替换 HID 报告描述符。
// 本固件的克隆范围是身份层(VID/PID/字符串), 不是报文格式层。
void receiveRealHidDescriptor(uint8_t iface, const char *hex);
void initClonedDevices();
void cloneReenumerate();
bool isCloneMouseActive();
bool isCloneKbdActive();
const ClonedReportLayout *mouseLayout();
const ClonedReportLayout *kbdLayout();

// 按真设备格式发报文。当前恒返回 false(未实现), 调用方应回落内置路径。
bool sendClonedMouseReport(int dx, int dy, int wheel, uint8_t buttons);
bool sendClonedKbdReport(uint8_t modifiers, const uint8_t *keys);
