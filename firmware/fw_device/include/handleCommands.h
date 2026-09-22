#pragma once

#include "InitSettings.h"
#include "USBSetup.h"
#include "usb_desc.h"
#include <Arduino.h>
#include <cstring>
#include <atomic>

// Task handles (定义在 tasks.cpp)
extern TaskHandle_t mouseMoveTaskHandle;
extern TaskHandle_t ledFlashTaskHandle;

// 描述符克隆握手用的命令序列 (由 sendNextCommand 逐个下发)
extern const char *commandQueue[];
extern int currentCommandIndex;

// 相对/绝对坐标追踪
extern int16_t mouseX;
extern int16_t mouseY;

// Serial RX 入口 (由 serial0Task/serial1Task 调用)
void serial0RX();
void serial1RX();

// km.* 命令 handler
void handleKmMoveCommand(const char *command);
void handleKmMoveto(const char *command);
void handleKmGetpos(const char *command);
// 五键 × 按下/抬起共 10 条命令统一走这一个入口(从命令串解析按键与状态)
void handleKmMouseButton(const char *command);
void handleKmWheel(const char *command);
void handleKmClick(const char *command);   // 固件内定时弹起 km.click(bits,ms)
void handleKmButtons(const char *command); // 官方 SDK 按键监控开关 km.buttons(0/1)

// 动作原语 (直写 TinyUSB, 由 kbdUsbSendMouse 承担 HID 报文发送)
void handleMove(int x, int y);
void handleMoveto(int x, int y);
void handleMouseButton(uint8_t bit, bool press);
void handleMouseWheel(int wheelMovement);
void handleGetPos();

// 位掩码 (与 usb_hid.cpp / usb_desc.cpp 描述符里的 button byte 位序一致)
#define MOUSE_BUTTON_LEFT       0x01
#define MOUSE_BUTTON_RIGHT      0x02
#define MOUSE_BUTTON_MIDDLE     0x04
#define MOUSE_BUTTON_FORWARD    0x08   // side1
#define MOUSE_BUTTON_BACKWARD   0x10   // side2

// USB 握手 (右板 -> 左板的描述符 JSON 转发流程)
void handleUsbHello(const char *command);
void handleUsbGoodbye(const char *command);
void handleNoDevice(const char *command);
void handleDebug(const char* command);
void handleSerial0Speed(const char* command);
void handleEspLog(const char* command);
void handleDebugcommand(const char *command);
void sendNextCommand();
void processCommand(const char *command);

// InitSettings 里的 JSON 接收 handler
extern void receiveDeviceInfo(const char *jsonString);
extern void receiveDescriptorDevice(const char *jsonString);
extern void receiveEndpointDescriptors(const char *jsonString);
extern void receiveInterfaceDescriptors(const char *jsonString);
extern void receiveHidDescriptors(const char *jsonString);
extern void receiveIADescriptors(const char *jsonString);
extern void receiveEndpointData(const char *jsonString);
extern void receiveUnknownDescriptors(const char *jsonString);
extern void receivedescriptorConfiguration(const char *jsonString);

// 命令表结构
struct CommandEntry {
    const char *command;
    void (*handler)(const char *);
};

// CLICK 定时弹起检查 (由 mouseMoveTask 每 10ms 顺手调一次)
void checkClickReleases();
