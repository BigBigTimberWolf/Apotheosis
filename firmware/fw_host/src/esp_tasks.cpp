#include "EspUsbHost.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// 每链路独立的行缓冲。
//
// 原实现是单个全局 `RingBuf<char, 512> rxBuffer`, 被 RxTaskSerial0 与
// RxTaskSerial1 两条任务共用 —— handleSerialInput 虽然在形参上区分了
// HardwareSerial, 但读写的是同一个全局对象, 于是:
//   1) 两条链路(4Mbps 与 5Mbps)同时活跃时数据互相串话, 两个协议的行交错污染;
//   2) RingBuf 的 push/pop 是非加锁版, mSize 的读改写无临界区, 并发下会丢更新,
//      `while (!isEmpty())` 可能把从未写入的陈旧字节当成命令解析。
// 现在改为按链路分开, 顺带解决串话与竞态。
//
// 容量从 512 提到 4096: 单行最坏长度由 sendRawHidDescriptors() 决定 ——
// 报告描述符上限 512 字节, 转 hex 后是 1024 字符, 加前缀约 1050 字节;
// 而 512 的缓冲装不下, 溢出后残留字节会让之后每一行都错位, 表现为
// "Unknown command received" 刷屏、握手永久卡死(注释里早写明了这个风险,
// 但当时只放大了 commandBuffer, 没有同步放大本缓冲)。
RingBuf<char, 4096> rxBuffer;      // Serial0 专用(保留原名, 兼容其它引用)
RingBuf<char, 4096> rxBuffer1;     // Serial1 专用
// 说明: 原先这里还有一个 `RingBuf<char, 512> txBuffer`, 全工程零引用
// (发送恒走 Serial1.write), 已删除。
SemaphoreHandle_t ledSemaphore;
TaskHandle_t cleanupTaskHandle = NULL;
TaskHandle_t rxSerialTaskHandle;
#define USB_TASK_PRIORITY 1
#define CLIENT_TASK_PRIORITY 2

// 说明: 这里原先还有键盘帧发送的跨任务互斥量(s_kbd_tx_mtx)、心跳间隔常量
// (kKbdHeartbeatMs / s_kbdHeartbeatMs), 以及对 esp_usb_host.cpp 里"键盘影子
// 副本"(s_kbdRawLast 等)的 extern 声明。键盘转发已整体删除, 故一并删除。

void EspUsbHost::begin(void)
{
    usbTransferSize = 0;
    deviceSuspended = false;
    last_activity_time = millis();

    const usb_host_config_t host_config = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    usb_host_install(&host_config);

    ledSemaphore = xSemaphoreCreateBinary();
    s_mask_mtx   = xSemaphoreCreateMutex();   // 屏蔽状态跨任务互斥(见 EspUsbHost.h)

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->receiveSerial0(arg); 
    }, "RxTaskSerial0", 4096, this, 5, &rxSerialTaskHandle) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create RxTaskSerial0.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->receiveSerial1(arg); 
    }, "RxTaskSerial1", 4096, this, 5, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create RxTaskSerial1.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->usbLibraryTask(arg); 
    }, "usbLibTask", 4096, this, USB_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create usbLibTask.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->usbClientTask(arg); 
    }, "usbClientTask", 4096, this, CLIENT_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create usbClientTask.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->cleanupTask(arg); 
    }, "CleanupTask", 4096, this, 5, &cleanupTaskHandle) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create CleanupTask.");
    }

    // 说明: 原来这里还会创建一个 monitorInactivity 任务(3200 字节栈),
    // 它的函数体只有 `vTaskDelete(NULL)` —— 建完立刻自杀。自动挂起功能已
    // 永久禁用(见该函数注释), 因此这个任务纯粹是浪费 3200 字节栈 + TCB,
    // 已不再创建。

    if (xTaskCreate(flashLEDToggleTask, "LED Flash Task", 1500, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create LED Flash Task.");
    }

    ESP_LOGI("EspUsbHost", "EspUsbHost::begin() completed. Tasks created.");
}



// combine both maybe mutex?

void handleSerialInput(HardwareSerial &serial, EspUsbHost *instance, RingBuf<char, 4096> &rb) {
    while (serial.available() > 0) {
        char byte = serial.read();

        if (byte == '\r') continue;

        if (!rb.isFull()) {
            rb.push(byte);
        } else {
            ESP_LOGW("EspUsbHost", "RX buffer overflow detected.");
            break;
        }

        if (byte == '\n') {
            // 必须容纳整行 JSON: 端点描述符每项约 166 字符, 最多 10 项 -> 最坏约 1700 字节。
            // 设备侧为此已把 serial1RingBuffer 放大到 16KB、commandBuffer 放到 8192,
            // 主机侧原来只有 620 —— 一旦超长, 这里 while 循环到 619 就 break,
            // 残留字节留在 rxBuffer 里错位, 之后每一行都是残帧, 握手就此永久卡死。
            char commandBuffer[2048];
            int commandIndex = 0;

            while (!rb.isEmpty() && commandIndex < sizeof(commandBuffer) - 1) {
                rb.pop(commandBuffer[commandIndex++]);
                if (commandBuffer[commandIndex - 1] == '\n') break;
            }

            commandBuffer[commandIndex] = '\0';

            if (commandIndex > 0 && commandBuffer[commandIndex - 1] == '\n') {
                commandBuffer[commandIndex - 1] = '\0';
            }

            instance->handleIncomingCommands(commandBuffer);
            // 顺带把这一行喂给格式协商状态机(识别 km.movefmt 的 OK 应答)
            instance->onSerial1Line(commandBuffer);
        }
    }
}

void EspUsbHost::receiveSerial0(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1));                                                               // Set 1ms, be lazy be happy
        handleSerialInput(Serial0, instance, rxBuffer);
    }
}

void EspUsbHost::receiveSerial1(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1));                                                              // Set 1ms, be lazy be happy
        handleSerialInput(Serial1, instance, rxBuffer1);
        instance->moveFmtTick();          // 推进位移帧格式协商(超时/重试)
        // 屏蔽窗口的硬超时兜底: 这是解除屏蔽的唯一保证路径, 必须每毫秒都跑到。
        // 上位机崩溃/忘记发解除命令时, 靠这里把真实输入恢复回来。
        instance->maskTick();
    }
}

bool EspUsbHost::serial1Send(const char *format, ...)
{
    char logMsg[256];

    va_list args;
    va_start(args, format);
    int len = vsnprintf(logMsg, sizeof(logMsg), format, args);
    va_end(args);

    if (len > 0) {
        if (len >= sizeof(logMsg)) len = sizeof(logMsg) - 1;
        Serial1.write((const uint8_t*)logMsg, len);
        return true;
    }

    return false;
}

// ============================================================================
// Serial1 二进制位移帧 (0x01 MOVE)
//
// 帧格式与 docs/proto.md §1 完全一致: A5 5C | LEN | SEQ | CMD | PAYLOAD | CRC16
// CRC 范围 = LEN + SEQ + CMD + PAYLOAD (MODBUS, 低字节在前), 与本设备 g_proto 的校验一致。
//
// 为什么值得换: docs/proto.md §4.1 —— 二进制 0x01 走内联旁路, ASCII km.move 原来恒定走
// 任务通知。设备侧现已为两条路径都补上旁路, 二进制进一步省掉 ASCII 解析(sscanf)与
// 每帧 18 字节的文本开销。
//
// 安全: 只有在设备对 "km.movefmt(1)" 回了 OK 之后 binaryMovesEnabled 才会置位。
// 旧固件不认识该命令 -> 命令落到 handleDebugcommand() 静默返回 -> 这里超时 -> 保持 ASCII。
// 因此新主机 + 旧设备不会把链路打断; 也已经支持被显式关掉 (moveFmtNegotiationMs = 0)。
// ============================================================================
static uint8_t  s_tx_seq = 0;
// 帧缓冲: 现在唯一的发送者是 0x01 MOVE 帧 ->
//   A5 5C | LEN | SEQ | CMD | dx(2) | dy(2) | CRC(2) = 2+1+1+1+4+2 = 11 字节
static char     s_tx_frame[24];

void EspUsbHost::serial1SendMoveBinary(int8_t x, int8_t y)
{
    // PAYLOAD = int16 dx, int16 dy (小端), 与 device 侧 rd_i16 读取顺序一致
    const uint8_t len = 4;
    s_tx_frame[0] = (char)0xA5;
    s_tx_frame[1] = (char)0x5C;
    s_tx_frame[2] = (char)len;
    s_tx_frame[3] = (char)(s_tx_seq++);
    s_tx_frame[4] = (char)0x01;                      // CMD_MOVE
    s_tx_frame[5] = (char)((uint8_t)(int8_t)x);
    s_tx_frame[6] = (char)((uint8_t)((int8_t)x < 0 ? 0xFF : 0x00));
    s_tx_frame[7] = (char)((uint8_t)(int8_t)y);
    s_tx_frame[8] = (char)((uint8_t)((int8_t)y < 0 ? 0xFF : 0x00));

    // CRC over LEN..PAYLOAD: s_tx_frame[2] .. s_tx_frame[8]  (共 3 + len = 7 字节)
    uint16_t crc = 0xFFFF;
    for (int i = 2; i < 2 + 3 + len; ++i) {
        crc ^= (uint8_t)s_tx_frame[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    s_tx_frame[9]  = (char)(crc & 0xFF);
    s_tx_frame[10] = (char)((crc >> 8) & 0xFF);

    Serial1.write((const uint8_t *)s_tx_frame, 11);   // 总帧长 = LEN + 7
}

// 说明: 这里原先还有四个键盘函数 —— serial1SendKeyboardRaw() /
// serial1SendKeyboardHeartbeat() / serial1SendKeyboardBinary() / kbdLinkTick()
// (0x23 KB_REPORT 帧、心跳帧、50ms 状态重申), 以及它们依赖的 s_kbd_tx_mtx。
// 键盘转发已整体删除(键盘由独立固件负责), 故一并删除; Serial1 上现在只有
// 鼠标相关的 ASCII km.* 命令与 0x01 MOVE 二进制帧。

void EspUsbHost::monitorInactivity(void *arg)
{
    // 定制固件: 自动挂起已禁用 —— 根治"游戏PC开机后鼠标必须重新插拔"bug。
    // 原厂逻辑: 空闲10s -> suspend_device(); 挂起后 _onReceive 不再resubmit传输,
    // 而恢复逻辑恰恰写在 _onReceive 里等数据触发 => 死锁, 只有重新插拔能救。
    (void)arg;
    vTaskDelete(NULL);
}

// 位移帧格式协商(非阻塞状态机, 不占用 RX 任务)
//
// 流程: 设备就绪 -> 发 "km.movefmt(1)" -> 等设备回加时间戳应答(含 "OK") -> 切二进制。
// 最多重试 moveFmtMaxAttempts 次, 每次 150ms; 全部失败则永久保持 ASCII。
// 这条路径保证: 新主机 + 旧设备(不认识 km.movefmt)只会退化为原来的 ASCII 行为, 不会断链。
void EspUsbHost::moveFmtTick()
{
    if (!moveFmtPending) return;
    if (!deviceMouseReady) return;

    const uint32_t now = millis();
    if ((int32_t)(now - moveFmtDeadline) < 0) return;

    if (moveFmtAttempts >= moveFmtMaxAttempts) {
        moveFmtPending = false;
        ESP_LOGW("EspUsbHost", "movefmt: no OK after %u tries, staying on ASCII km.move",
                 (unsigned)moveFmtAttempts);
        return;
    }

    moveFmtAttempts++;
    moveFmtDeadline = now + 150;
    // 带 track id 便于识别正是这次协商的应答
    serial1Send("km.movefmt(1)#%u\n", (unsigned)(900 + moveFmtAttempts));
}

// 请求协商(由 commands.cpp 在设备握手完成后调用一次), 随后由 moveFmtTick() 非阻塞推进
void EspUsbHost::onDeviceReady()
{
    if (moveFmtMaxAttempts == 0) return;         // 显式关闭: 永远用 ASCII
    if (binaryMovesEnabled || moveFmtPending) return;
    moveFmtPending  = true;
    moveFmtAttempts = 0;
    // 延后 250ms 再发: 设备收到 "USB_INIT" 时 processingUsbCommands 刚被清零,
    // 而 normalCommandTable(km.movefmt 所在表)只在 !processingUsbCommands 时才被查。
    // 留一点余量, 避免协商命令恰好落在握手尾部被忽略。
    moveFmtDeadline = millis() + 250;
}

// 收到设备 ASCII 应答时调用: 识别 km.movefmt 的 OK 应答
void EspUsbHost::onSerial1Line(const char *line)
{
    if (!moveFmtPending) return;
    // 形如 ">>> #901:OK"
    if (strstr(line, "OK") == nullptr) return;

    moveFmtPending = false;
    binaryMovesEnabled = true;
    ESP_LOGI("EspUsbHost", "movefmt: device confirmed binary 0x01 MOVE");
}

void EspUsbHost::usbLibraryTask(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

    while (true) {
        uint32_t event_flags;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (err != ESP_OK) {
            ESP_LOGE("EspUsbHost", "usb_host_lib_handle_events() failed with error: %x", err);
            continue;
        }

        if (instance->clientHandle == NULL || !instance->isClientRegistering) {
            ESP_LOGI("EspUsbHost", "Registering client...");
            const usb_host_client_config_t client_config = {
                .max_num_event_msg = 10,
                .async = {
                    .client_event_callback = instance->_clientEventCallback,
                    .callback_arg = instance,
                }
            };

            err = usb_host_client_register(&client_config, &instance->clientHandle);
            if (err != ESP_OK) {
                ESP_LOGW("EspUsbHost", "Failed to register client, retrying...");
                vTaskDelay(pdMS_TO_TICKS(100));
            } else {
                ESP_LOGI("EspUsbHost", "Client registered successfully.");
                instance->isClientRegistering = true;
            }
        }
    }
}


void EspUsbHost::usbClientTask(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

    while (true) {
        if (!instance->isClientRegistering) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        usb_host_client_handle_events(instance->clientHandle, portMAX_DELAY);
    }
}

void flashLEDToggleTask(void *parameter)
{
    pinMode(9, OUTPUT);

    while (true) {
        if (xSemaphoreTake(ledSemaphore, portMAX_DELAY) == pdTRUE) {
            digitalWrite(9, HIGH);
            vTaskDelay(pdMS_TO_TICKS(25));
            digitalWrite(9, LOW);
        }
    }
}

void flashLED()
{
    if (ledSemaphore != NULL) {
        xSemaphoreGive(ledSemaphore);
    }
}
