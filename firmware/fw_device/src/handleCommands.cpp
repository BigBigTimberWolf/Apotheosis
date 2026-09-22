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

// km.move 通过单独累加器传给 mouseMoveTask 消费。
//
// ★ 为什么真鼠标与注入必须分成两组变量, 且累加语义不同:
//
//   真鼠标 (Serial1, 右板转发): 必须【累加】。
//     透传设备的不变量是"被控机上光标的位置 == 物理鼠标的真实位置"。
//     每丢一个增量, 光标就永久偏一点 —— 表现为"鼠标变慢/跟不上手"。
//     而 xTaskNotifyGive / ulTaskNotifyTake(pdTRUE) 会把多次通知【合并成一次唤醒】,
//     所以只要 mouseMoveTask 没来得及消费(例如 handleMove 在等 USB 端点时会阻塞
//     最多 100ms), 期间到达的报告就会互相覆盖 —— 1000Hz 鼠标在 100ms 内会有
//     约 100 个增量塌缩成 1 个, 99% 的位移凭空消失, 等端点恢复后才"恢复正常"。
//     这正是"偶尔变慢几秒又恢复"的机制。
//
//   注入 (Serial0, 上位机): 保持【最新覆盖】。
//     上位机每帧都按当前误差重新计算该发多少, 旧增量已经没有意义;
//     若也累加, 一旦下发速率超过 USB 承载能力就会积压, 松手后光标还会继续漂(过冲)。
//     所以注入侧刻意保留"最新赢"的丢帧策略。
std::atomic<int> moveXReal(0);
std::atomic<int> moveYReal(0);
std::atomic<int> moveXInj(0);
std::atomic<int> moveYInj(0);

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
//
// ★ 必须显式推进握手 (sendNextCommand)。
//
// 右板是按【有原始描述符的接口数】发行的 (d.len == 0 的接口跳过), 行数可变
// (0 ~ kMaxIface), 且没有天然的结束标记 —— 所以右板在流末尾补发一行
// "USB_sendRawHidDescriptors:done" 作为终结符, 这里见到它就推进队列。
//
// 历史 bug: 本函数此前既不判终结符也不调 sendNextCommand, 于是队列永远停在
// 这一步 (currentCommandIndex 到不了 10), processingUsbCommands 永久为 true ——
// 那会把整个 km.* 命令表门控掉, 表现为"能移动但点击/滚轮全失效"。
static void handleReceiveRawHidDescriptor(const char *command) {
    const char *p = command + strlen("USB_sendRawHidDescriptors:");

    // 流结束标记: 推进握手。
    if (strncmp(p, "done", 4) == 0) {
        sendNextCommand();
        return;
    }

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
            if (strncmp(commandBuffer, "km.move", 7) == 0) {
                // ★ 真鼠标位移【不】参与 kmMoveCom 互斥。
                //
                // 原来这里带了 && !kmMoveCom.exchange(true): 一旦 serial0Task 正持着
                // 这个标志, 真鼠标的这一拍就竞争失败 -> 既丢位移, 又被甩进
                // processCommand, 而 km.move 不在 normalCommandTable 里, 最终落到
                // handleDebugcommand 被【原样回显到 Serial0】—— 白白污染日志还丢输入。
                // handleKmMoveCommand 内部只用原子量与互斥锁, 从两个 RX 任务并发调用安全。
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

    // 按来源决定累加还是覆盖 —— 见 moveXReal 处的说明。
    //   SRC_REAL   = 右板转发的真实鼠标 -> 累加(一个增量都不能丢)
    //   SRC_INJECT = 上位机注入         -> 覆盖(最新赢, 避免积压过冲)
    const bool isReal = (g_cmd_source.load() == SRC_REAL);

    if (isReal) {
        moveXReal.fetch_add(x, std::memory_order_relaxed);
        moveYReal.fetch_add(y, std::memory_order_relaxed);
    } else {
        std::lock_guard<std::mutex> lock(commandMutex);
        moveXInj = x;
        moveYInj = y;
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
    // ★ 不再用 processingUsbCommands 门控 km.* 命令。
    //
    // 原实现把 normalCommandTable 压在 !processingUsbCommands 之后, 名义理由是
    // "握手期间不要处理普通命令"。但那个理由站不住:
    //   · USB_* 应答走 usbCommandTable, 它在这段【之前】且顺序优先, 不受影响;
    //   · Serial0(上位机) 与 Serial1(板间) 是两条独立 UART, 处理一条来自上位机的
    //     km.* 命令不会干扰板间的握手时序。
    //
    // 而这道门控的代价是灾难性的: 只要握手因为任何原因没走完 10 步,
    // processingUsbCommands 就【永久停在 true】, 于是所有 km.* 命令全部失效 ——
    // 唯一还能用的只剩 km.move (它在 serial0RX 里被特判, 绕过了 processCommand)。
    // 表现为"鼠标能移动, 但点击/滚轮/侧键全部没反应", 且固件不报任何错
    // (未识别的命令会落到 handleDebugcommand 被原样回显, 看起来像"命令发出去了")。
    //
    // 触发实例: commandQueue 的 sendRawHidDescriptors 一步, 右板回包后左板的
    // 接收函数没有调用 sendNextCommand(), 队列永远停在索引 6, 永远到不了 10。
    for (const auto &entry : normalCommandTable) {
        if (strncmp(command, entry.command, strlen(entry.command)) == 0) {
            entry.handler(command);
            return;
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

        // 两级位移合并成一拍下发: 真鼠标(累加, 一个不丢) + 注入(最新值)。
        // 真鼠标用 exchange(0) 取走并清零, 保证这期间新到的增量不会丢。
        int x, y;
        {
            const int rx = moveXReal.exchange(0, std::memory_order_relaxed);
            const int ry = moveYReal.exchange(0, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(commandMutex);
            x = rx + moveXInj.exchange(0, std::memory_order_relaxed);
            y = ry + moveYInj.exchange(0, std::memory_order_relaxed);
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
