#include "handleCommands.h"
#include "InitSettings.h"
#include "tasks.h"
#include "USBSetup.h"
#include "usb_desc.h"
#include <Arduino.h>
#include <cstring>
#include <atomic>
#include <mutex>
#include <RingBuf.h>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// km.move 通过 atomic 传给 mouseMoveTask 消费。moveX/moveY 是【当前挂着未处理】的位移。
std::atomic<int> moveX(0);
std::atomic<int> moveY(0);

// 按键掩码 —— 真/注入分开维护。发到 HID 的是 merged = real | inj。
// 位序与 usb_desc.cpp 的报告描述符对齐: bit0=L bit1=R bit2=M bit3=side1 bit4=side2
// 拆开的原因: 上位机 physicalButtonPressed 需要读【真实鼠标】的按键, 不是注入。
// 二者合并后不可分, 所以在源头就分开记账。
std::atomic<uint8_t> s_realBtnState(0);   // 右板转发的真实鼠标按键
std::atomic<uint8_t> s_injBtnState(0);    // 上位机注入的按键
std::atomic<uint8_t> s_mouseBtnState(0);  // = real | inj, 冗余但供 handleMove 读取

// 命令来源标记 —— serial0RX/serial1RX 处理命令前设置, 供 handleMouseButton 判断
// 是"真实按键"还是"注入按键"。
enum : uint8_t { SRC_INJECT = 0, SRC_REAL = 1 };
std::atomic<uint8_t> g_cmd_source(SRC_INJECT);

// km.buttons(1) 使能后, 每次【真实按键】变化就推送一个单字节掩码 (0..31) 到 Serial0。
// 上位机的 MakcuNew feedAsciiByte 里 <0x20 分支收下作为按键流。
std::atomic<bool> g_buttonMonitoringEnabled(false);

// 防止 km.move 命令行在 serial0RX/serial1RX 之间重入
std::atomic<bool> kmMoveCom(false);

// Task handles: 定义在 tasks.cpp
extern TaskHandle_t mouseMoveTaskHandle;
extern TaskHandle_t ledFlashTaskHandle;

std::mutex commandMutex;

// USB 握手状态
// (deviceConnected 定义在 USBSetup.cpp, 这里只声明为 extern)
extern volatile bool deviceConnected;
bool usbReady = false;
bool processingUsbCommands = false;

// Ring buffers (每链路独立, 避免串话)
RingBuf<char, 620> serial0RingBuffer;
RingBuf<char, 8192> serial1RingBuffer;  // Serial1 装描述符 JSON, 需要 8KB
int currentCommandIndex = 0;

// 相对坐标追踪 (km.moveto / km.getpos 用)
int16_t mouseX = 0;
int16_t mouseY = 0;

const unsigned long ledFlashTime = 25;

// 描述符克隆握手序列
//
// ★ sendRawHidDescriptors: 请求真设备的【原始 HID 报告描述符字节流】。
//   这一步是描述符深度克隆的关键 —— 有了真字节流, usb_desc.cpp 的
//   tud_hid_descriptor_report_cb 才能返回真设备格式(而不是本板内置 52 字节
//   Boot Mouse), 主机的厂商专属驱动才有机会识别成对应产品。
//   历史版本里 commandQueue 没有请求它, 于是 receiveRealHidDescriptor 从不
//   被触发, s_mouseDescReady 永远为 false, 深度克隆是死代码。这里补上。
const char *commandQueue[] = {
    "sendDeviceInfo",
    "sendDescriptorDevice",
    "sendEndpointDescriptors",
    "sendInterfaceDescriptors",
    "sendHidDescriptors",
    "sendRawHidDescriptors",
    "sendIADescriptors",
    "sendEndpointData",
    "sendUnknownDescriptors",
    "sendDescriptorconfig"
};

// 接收真设备原始 HID 报告描述符 —— 格式: USB_sendRawHidDescriptors:<iface>:<hex>
// (fw_host 逐接口发一行, 见 fw_host/serialization.cpp:sendRawHidDescriptors)
static void handleReceiveRawHidDescriptor(const char *command) {
    const char *p = command + strlen("USB_sendRawHidDescriptors:");
    char *end = nullptr;
    long iface = strtol(p, &end, 10);
    if (!end || *end != ':') return;
    const char *hex = end + 1;
    receiveRealHidDescriptor((uint8_t)iface, hex);
}

// ---------------------------------------------------------------------------
// Command tables
// ---------------------------------------------------------------------------

CommandEntry serial0CommandTable[] = {
    {"DEBUG_", handleDebug},
    {"SERIAL_", handleSerial0Speed}
};

CommandEntry debugCommandTable[] = {
    {"ESPLOG_", handleEspLog},
    {"PRINT_Parsed_Descriptors", printParsedDescriptors},
    {"HID_Descriptors", [](const char* arg) { Serial1.print(arg); }}
};

// 命令表: km.buttons 是官方 SDK 的按键监控开关。km.ms1/km.ms2 是 SDK 用的
// 侧键命名 alias (跟 km.side1/km.side2 等价, 保留兼容)。
void handleKmButtons(const char *command);

// 前缀匹配, 十条按键命令 (km.left(0/1), km.right(0/1), ...) 统一走一个 handler
CommandEntry normalCommandTable[] = {
    {"km.moveto",  handleKmMoveto},
    {"km.getpos",  handleKmGetpos},
    {"km.click",   handleKmClick},
    {"km.buttons", handleKmButtons},
    {"km.left",    handleKmMouseButton},
    {"km.right",   handleKmMouseButton},
    {"km.middle",  handleKmMouseButton},
    {"km.side1",   handleKmMouseButton},
    {"km.side2",   handleKmMouseButton},
    {"km.ms1",     handleKmMouseButton},   // 官方 SDK 用的侧键命名
    {"km.ms2",     handleKmMouseButton},
    {"km.wheel",   handleKmWheel}
};

CommandEntry usbCommandTable[] = {
    {"USB_HELLO", handleUsbHello},
    {"USB_GOODBYE", handleUsbGoodbye},
    {"USB_ISNULL", handleNoDevice},
    {"USB_sendDeviceInfo:", receiveDeviceInfo},
    {"USB_sendDescriptorDevice:", receiveDescriptorDevice},
    {"USB_sendEndpointDescriptors:", receiveEndpointDescriptors},
    {"USB_sendInterfaceDescriptors:", receiveInterfaceDescriptors},
    {"USB_sendHidDescriptors:", receiveHidDescriptors},
    {"USB_sendRawHidDescriptors:", handleReceiveRawHidDescriptor},
    {"USB_sendIADescriptors:", receiveIADescriptors},
    {"USB_sendEndpointData:", receiveEndpointData},
    {"USB_sendUnknownDescriptors:", receiveUnknownDescriptors},
    {"USB_sendDescriptorconfig:", receivedescriptorConfiguration}
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void trimCommand(char* command) {
    int len = strlen(command);
    while (len > 0 && (command[len - 1] == ' ' || command[len - 1] == '\n' || command[len - 1] == '\r')) {
        command[len - 1] = '\0';
        len--;
    }
}

// ---------------------------------------------------------------------------
// Serial RX
// ---------------------------------------------------------------------------

void serial0RX() {
    while (Serial0.available() > 0) {
        char byte = Serial0.read();

        if (byte == '\r') continue;

        if (!serial0RingBuffer.isFull()) {
            serial0RingBuffer.push(byte);
        } else {
            Serial0.println("Serial0 ring buffer overflow detected.");
        }

        if (byte == '\n') {
            char commandBuffer[620];
            int commandIndex = 0;

            while (!serial0RingBuffer.isEmpty() && commandIndex < (int)sizeof(commandBuffer) - 1) {
                serial0RingBuffer.pop(commandBuffer[commandIndex++]);
            }
            commandBuffer[commandIndex] = '\0';
            trimCommand(commandBuffer);

            g_cmd_source.store(SRC_INJECT);   // Serial0 = 上位机注入
            if (strncmp(commandBuffer, "km.move", 7) == 0) {
                if (!kmMoveCom.exchange(true)) {
                    handleKmMoveCommand(commandBuffer);
                }
            } else {
                processCommand(commandBuffer);
            }
        }
    }
}

void serial1RX() {
    while (Serial1.available() > 0) {
        char byte = Serial1.read();

        if (byte == '\r') continue;

        if (!serial1RingBuffer.isFull()) {
            serial1RingBuffer.push(byte);
        } else {
            Serial0.println("Serial1 ring buffer overflow detected.");
        }

        if (byte == '\n') {
            // 装整行 JSON: 端点描述符每项约 166 字节, 最多 10 项 -> 最坏约 1700 字节
            static char commandBuffer[8192];
            int commandIndex = 0;

            while (!serial1RingBuffer.isEmpty() && commandIndex < (int)sizeof(commandBuffer) - 1) {
                serial1RingBuffer.pop(commandBuffer[commandIndex++]);
            }
            commandBuffer[commandIndex] = '\0';
            trimCommand(commandBuffer);

            g_cmd_source.store(SRC_REAL);   // Serial1 = 右板转发的真实鼠标
            if (strncmp(commandBuffer, "km.move", 7) == 0 && !kmMoveCom.exchange(true)) {
                handleKmMoveCommand(commandBuffer);
            } else {
                processCommand(commandBuffer);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// km.move (via mouseMoveTask, 官方相同结构)
// ---------------------------------------------------------------------------

void handleKmMoveCommand(const char *command) {
    int x = 0, y = 0;
    sscanf(command + strlen("km.move") + 1, "%d,%d", &x, &y);

    {
        std::lock_guard<std::mutex> lock(commandMutex);
        moveX = x;
        moveY = y;
    }

    if (mouseMoveTaskHandle != NULL) {
        xTaskNotifyGive(mouseMoveTaskHandle);
    }

    kmMoveCom = false;
}

// ---------------------------------------------------------------------------
// LED (dead-ended in this firmware, kept for parity with official)
// ---------------------------------------------------------------------------

void ledFlashTask(void *pvParameters) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        digitalWrite(9, HIGH);
        vTaskDelay(ledFlashTime / portTICK_PERIOD_MS);
        digitalWrite(9, LOW);
        vTaskDelay(ledFlashTime / portTICK_PERIOD_MS);
    }
}

// ---------------------------------------------------------------------------
// USB handshake
// ---------------------------------------------------------------------------

void handleUsbHello(const char *command) {
    deviceConnected = true;
    usbReady = true;
    processingUsbCommands = true;
    currentCommandIndex = 0;
    sendNextCommand();
}

void handleUsbGoodbye(const char *command) {
    Serial0.println("USB Device disconnected. Restarting!");
    handleMouseButton(MOUSE_BUTTON_LEFT, false);
    handleMouseButton(MOUSE_BUTTON_RIGHT, false);
    handleMouseButton(MOUSE_BUTTON_MIDDLE, false);
    handleMouseButton(MOUSE_BUTTON_FORWARD, false);
    handleMouseButton(MOUSE_BUTTON_BACKWARD, false);
    vTaskDelay(100);
    ESP.restart();
}

void sendNextCommand() {
    if (!processingUsbCommands ||
        currentCommandIndex >= (int)(sizeof(commandQueue) / sizeof(commandQueue[0]))) {
        return;
    }
    const char *command = commandQueue[currentCommandIndex];
    Serial1.println(command);
    currentCommandIndex++;
    if (currentCommandIndex >= (int)(sizeof(commandQueue) / sizeof(commandQueue[0]))) {
        usbReady = false;
        processingUsbCommands = false;
        InitUSB();
        vTaskDelay(700);
        Serial1.println("USB_INIT");
    }
}

void processCommand(const char *command) {
    for (const auto &entry : debugCommandTable) {
        if (strncmp(command, entry.command, strlen(entry.command)) == 0) {
            entry.handler(command);
            return;
        }
    }
    for (const auto &entry : serial0CommandTable) {
        if (strncmp(command, entry.command, strlen(entry.command)) == 0) {
            entry.handler(command);
            return;
        }
    }
    for (const auto &entry : usbCommandTable) {
        if (strncmp(command, entry.command, strlen(entry.command)) == 0) {
            entry.handler(command);
            return;
        }
    }
    if (!processingUsbCommands) {
        for (const auto &entry : normalCommandTable) {
            if (strncmp(command, entry.command, strlen(entry.command)) == 0) {
                entry.handler(command);
                return;
            }
        }
    }
    handleDebugcommand(command);
}

// ---------------------------------------------------------------------------
// Misc handlers
// ---------------------------------------------------------------------------

void handleEspLog(const char *command) {
    const char *message = command + strlen("ESPLOG_");
    if (strlen(message) > 0) {
        Serial0.println(message);
    }
}

void handleSerial0Speed(const char *command) {
    int speed;
    if (sscanf(command + strlen("SERIAL_"), "%d", &speed) == 1) {
        if (speed >= 115200 && speed <= 5000000) {
            Serial0.print("Setting Serial0 speed to: ");
            Serial0.println(speed);
            Serial0.end();
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            Serial0.begin(speed);
            Serial0.onReceive(serial0ISR);
            Serial0.println("Serial0 speed change successful.");
        }
    }
}

void handleDebug(const char *command) {
    if (strcmp(command, "DEBUG_ON") == 0) {
        Serial1.println("DEBUG_ON");
    } else if (strcmp(command, "DEBUG_OFF") == 0) {
        Serial1.println("DEBUG_OFF");
    } else {
        int level;
        if (sscanf(command + strlen("DEBUG_"), "%d", &level) == 1) {
            Serial1.println(command);
        }
    }
}

void handleDebugcommand(const char *command) {
    // 未知命令回显到 CH343: 官方行为一致
    Serial0.println(command);
}

void handleNoDevice(const char *command) {
    deviceConnected = false;
}

// ---------------------------------------------------------------------------
// CLICK 定时弹起
//
// 用途: km.click(bits,ms) 命令: 按下 bits 掩码所指的按键, ms 毫秒后自动松开。
// 保留原因: 上位机不必依赖后续命令来释放, 上位机断线也不会永久卡键。
// 简化: 用 5 个 slot (每个鼠标键一个), 覆盖并发按下多个键的情况。
// ---------------------------------------------------------------------------

struct ClickSlot {
    uint8_t bit;
    uint32_t releaseAt;
    bool active;
};
static ClickSlot s_clickSlots[5] = {};

void handleKmClick(const char *command) {
    int bits = 0, ms = 50;
    const char *p = strchr(command, '(');
    if (!p) return;
    if (sscanf(p + 1, "%d,%d", &bits, &ms) < 1) return;
    bits &= 0x1F;
    if (!bits) return;
    if (ms <= 0) ms = 50;

    // 找空 slot 记账
    for (auto &s : s_clickSlots) {
        if (!s.active) {
            s.bit = (uint8_t)bits;
            s.releaseAt = millis() + (uint32_t)ms;
            s.active = true;
            uint8_t cur = s_mouseBtnState.fetch_or((uint8_t)bits) | (uint8_t)bits;
            kbdUsbSendMouse(cur, 0, 0, 0);
            return;
        }
    }
    // 槽位全占用: 放弃 (安全策略 - 不下发按下如果无法保证抬起)
}

void checkClickReleases() {
    uint32_t now = millis();
    for (auto &s : s_clickSlots) {
        if (s.active && (int32_t)(now - s.releaseAt) >= 0) {
            uint8_t cur = s_mouseBtnState.fetch_and((uint8_t)~s.bit) & (uint8_t)~s.bit;
            kbdUsbSendMouse(cur, 0, 0, 0);
            s.active = false;
            s.bit = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// mouseMoveTask: 消费 km.move 位移, 顺手检查 CLICK 到期
// ---------------------------------------------------------------------------

void mouseMoveTask(void *pvParameters) {
    while (true) {
        // 10ms 超时兜底: 让 CLICK 定时弹起能被检查(即使没有位移事件)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));

        int x, y;
        {
            std::lock_guard<std::mutex> lock(commandMutex);
            x = moveX;
            y = moveY;
            moveX = 0;
            moveY = 0;
        }
        if (x != 0 || y != 0) {
            handleMove(x, y);
        }

        checkClickReleases();
    }
}

// ---------------------------------------------------------------------------
// km.moveto / km.getpos / km.wheel / km.button (统一入口)
// ---------------------------------------------------------------------------

void handleKmMoveto(const char *command) {
    int x = 0, y = 0;
    if (sscanf(command + strlen("km.moveto") + 1, "%d,%d", &x, &y) < 2) return;
    handleMoveto(x, y);
}

void handleKmGetpos(const char *command) {
    handleGetPos();
}

void handleKmMouseButton(const char *command) {
    uint8_t bit = 0;
    if      (strncmp(command, "km.left",   7) == 0) bit = MOUSE_BUTTON_LEFT;
    else if (strncmp(command, "km.right",  8) == 0) bit = MOUSE_BUTTON_RIGHT;
    else if (strncmp(command, "km.middle", 9) == 0) bit = MOUSE_BUTTON_MIDDLE;
    else if (strncmp(command, "km.side1",  8) == 0) bit = MOUSE_BUTTON_FORWARD;
    else if (strncmp(command, "km.side2",  8) == 0) bit = MOUSE_BUTTON_BACKWARD;
    else if (strncmp(command, "km.ms1",    6) == 0) bit = MOUSE_BUTTON_FORWARD;
    else if (strncmp(command, "km.ms2",    6) == 0) bit = MOUSE_BUTTON_BACKWARD;
    else return;

    const char *paren = strchr(command, '(');
    const bool down = (paren != nullptr) && (*(paren + 1) == '1');
    handleMouseButton(bit, down);
}

void handleKmWheel(const char *command) {
    int wheelMovement = 0;
    if (sscanf(command + strlen("km.wheel") + 1, "%d", &wheelMovement) != 1) return;
    handleMouseWheel(wheelMovement);
}

// ---------------------------------------------------------------------------
// Motion primitives (直写 TinyUSB via kbdUsbSendMouse)
//
// 大位移拆分: kbdUsbSendMouse 每帧只吃 int8 (-127..127); 超出部分逐帧发。
// 若 tud_hid_n_ready 返回 false, vTaskDelay(1ms) 让出 CPU 让 USB 端点恢复;
// 上限 100 次重试(≈100ms) 后放弃, 剩余位移丢弃 —— 保护任务不被无限阻塞。
// 这是与官方 Arduino Mouse.move(内部无超时) 相比的关键差别: 避免因为主机
// USB 短暂 stall 导致 Serial 任务一起被拖垮。
// ---------------------------------------------------------------------------

void handleMove(int x, int y) {
    int retries = 0;
    while ((x != 0 || y != 0) && retries < 100) {
        int8_t sx = (x > 127) ? 127 : ((x < -127) ? -127 : (int8_t)x);
        int8_t sy = (y > 127) ? 127 : ((y < -127) ? -127 : (int8_t)y);
        uint8_t buttons = s_mouseBtnState.load();
        if (!kbdUsbSendMouse(buttons, sx, sy, 0)) {
            vTaskDelay(pdMS_TO_TICKS(1));
            retries++;
            continue;
        }
        x -= sx;
        y -= sy;
        mouseX = (int16_t)(mouseX + sx);
        mouseY = (int16_t)(mouseY + sy);
        retries = 0;
    }
}

void handleMoveto(int x, int y) {
    handleMove(x - mouseX, y - mouseY);
}

void handleMouseButton(uint8_t bit, bool press) {
    // 按来源分开: real (右板真实鼠标) / inj (上位机注入)。
    // 发到 HID 的是 merged; 上位机 physicalButtonPressed 通过 km.buttons 监控
    // 读到的是 real 变化 —— 单字节 <0x20 掩码, 见下方 g_buttonMonitoringEnabled 推送。
    const bool isReal = (g_cmd_source.load() == SRC_REAL);
    if (isReal) {
        if (press) s_realBtnState.fetch_or(bit);
        else       s_realBtnState.fetch_and((uint8_t)~bit);
    } else {
        if (press) s_injBtnState.fetch_or(bit);
        else       s_injBtnState.fetch_and((uint8_t)~bit);
    }
    uint8_t merged = (uint8_t)(s_realBtnState.load() | s_injBtnState.load());
    s_mouseBtnState.store(merged);
    // 按键报文 —— 若发送失败, 下次任意移动/滚轮/按键事件都会带上最新 buttons,
    // 状态最终会同步 (与官方 Arduino Mouse 行为一致)。
    kbdUsbSendMouse(merged, 0, 0, 0);

    if (isReal && g_buttonMonitoringEnabled.load()) {
        // 单字节掩码 (< 0x20), 上位机 feedAsciiByte 的 <0x20 分支收下, 不会污染 ASCII 行解析。
        Serial0.write((uint8_t)(s_realBtnState.load() & 0x1F));
    }
}

// km.buttons(0/1) 使能/禁用真实按键推送; km.buttons() 查询当前状态。
// 对齐官方 SDK 的按键监控开关 (Makcu.cpp 的 device_.enableButtonMonitoring)。
void handleKmButtons(const char *command) {
    const char *paren = strchr(command, '(');
    if (!paren) return;
    if (*(paren + 1) == ')') {
        // 查询: 回一个 ASCII 应答
        Serial0.print("km.buttons(");
        Serial0.print(g_buttonMonitoringEnabled.load() ? 1 : 0);
        Serial0.println(")");
        return;
    }
    g_buttonMonitoringEnabled.store(*(paren + 1) == '1');
}

void handleMouseWheel(int wheelMovement) {
    int retries = 0;
    while (wheelMovement != 0 && retries < 100) {
        int8_t sw = (wheelMovement > 127) ? 127 : ((wheelMovement < -127) ? -127 : (int8_t)wheelMovement);
        uint8_t buttons = s_mouseBtnState.load();
        if (!kbdUsbSendMouse(buttons, 0, 0, sw)) {
            vTaskDelay(pdMS_TO_TICKS(1));
            retries++;
            continue;
        }
        wheelMovement -= sw;
        retries = 0;
    }
}

void handleGetPos() {
    Serial0.print("km.pos(");
    Serial0.print(mouseX);
    Serial0.print(",");
    Serial0.print(mouseY);
    Serial0.println(")");
}
