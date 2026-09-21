#pragma once

#include "InitSettings.h"
#include <Arduino.h>
#include <USB.h>
#include <USBHIDMouse.h>
#include "USBSetup.h"
#include <esp_intr_alloc.h>
#include <cstring>
#include <atomic>

// Extern variables
extern USBHIDMouse Mouse;
extern TaskHandle_t mouseMoveTaskHandle;
extern const char *commandQueue[];
extern int currentCommandIndex;

// Mouse position tracking
extern int16_t mouseX;
extern int16_t mouseY;

// Function declarations
void handleKmMoveCommand(const char *command);
void handleDebugcommand(const char *command);
void handleMove(int x, int y);
void handleMoveto(int x, int y);
void handleMouseWheel(int wheelMovement);
void serial1RX();
void serial0RX();

void handleKmMoveto(const char *command);
void handleKmGetpos(const char *command);
// 五键 × 按下/抬起共 10 条命令统一走这一个入口(从命令串解析掩码与状态)
void handleKmMouseButton(const char *command);
void handleKmWheel(const char *command);

void handleUsbHello(const char *command);
void handleUsbGoodbye(const char *command);
void handleNoDevice(const char *command);
void handleDebug(const char* command);
void handleSerial0Speed(const char* command);
void handleEspLog(const char* command);
void sendNextCommand();
void processCommand(const char *command);

// Extern functions for JSON data handling
extern void receiveDeviceInfo(const char *jsonString);
extern void receiveDescriptorDevice(const char *jsonString);
extern void receiveEndpointDescriptors(const char *jsonString);
extern void receiveInterfaceDescriptors(const char *jsonString);
extern void receiveHidDescriptors(const char *jsonString);
extern void receiveIADescriptors(const char *jsonString);
extern void receiveEndpointData(const char *jsonString);
extern void receiveUnknownDescriptors(const char *jsonString);
extern void receivedescriptorConfiguration(const char *jsonString);

// Command table structure
struct CommandEntry {
    const char *command;
    void (*handler)(const char *);
};

// ---- 二进制协议与透传初始化 ----
#include "proto_parser.h"
extern proto::Parser g_proto;
extern proto::Parser g_proto1;   // Serial1(真实鼠标链路)的二进制解析器
void protoInit();
void clickTick();
void buttonWatchdogTick();   // 按键状态周期性重申(由 ClickTick 任务调用)
void proto1Init();           // 初始化 Serial1 二进制解析器
void proto1FeedAtLineStart(uint8_t b0, uint8_t b1, uint16_t len, uint8_t cmd,
                           const uint8_t *pl, uint16_t plen);
