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

// ===== 键盘帧发送的跨任务互斥 =====
//   两个调用者: USB 回调任务(真实报文) + 1ms 任务(kbdAffirmTick 周期性重申)。
//   它们共用 s_tx_frame / s_tx_seq, 必须保证"组装 + 写出"是原子的。
static SemaphoreHandle_t s_kbd_tx_mtx = nullptr;

// 键盘链路心跳间隔。
//   必须显著小于左板的"链路失活"判据(KBD_LINK_TIMEOUT_MS), 否则左板会把
//   一次偶发丢帧误判成链路断掉。50ms vs 400ms, 8 倍余量, 容忍连续丢 7 个心跳。
static const uint32_t kKbdHeartbeatMs = 50;
static uint32_t s_kbdHeartbeatMs = 0;

// esp_usb_host.cpp 里的键盘影子副本(非 static 全局, 这里显式声明)
extern uint8_t  s_kbdRawLast[8];
extern bool     s_kbdRawLastValid;
extern uint32_t s_kbdRawLastMs;
extern int      s_kbdFwdIface;

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
    s_kbd_tx_mtx = xSemaphoreCreateMutex();   // 键盘帧"组装+写出"原子性(见 serial1SendKeyboardRaw)

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
        // 键盘链路心跳: 让左板能把"链路断了"和"用户一直按着"区分开,
        // 从而可以安全地做卡键兜底(见 kbdLinkTick)。
        instance->kbdLinkTick();
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
// 帧缓冲: 最大帧 = KB_REPORT(6 键码) -> A5 5C LEN SEQ CMD MOD k0..k5 CRC(2)
//                               = 2+1+1+1+1+6+2 = 14 字节
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

// ============================================================================
// Serial1 二进制键盘帧 (0x23 KB_REPORT)
//
// 帧格式同上: A5 5C | LEN | SEQ | CMD | PAYLOAD | CRC16
// PAYLOAD = [modifiers][key0..key5], LEN = 1 + 键码槽数(标准键盘 6) = 7。
//
// 为什么走二进制而不是 ASCII: 键盘报文是"全量快照", 每次按键变化都要发一帧,
// 打字时每秒可能十几帧。ASCII 形式(如 km.kb(4,0,4,...))每帧 30+ 字节且要
// sscanf 解析, 二进制仅 14 字节且设备侧零解析。
// ============================================================================
// ============================================================================
// 键盘原样转发: payload = [iface][端点原始字节...]
//
// 【为什么带上 iface】
//   复合键盘接收器有多个 HID 接口(键盘 + 多媒体等)。实测多媒体接口的报文
//   也会被当成键盘报文, 导致按键忽有忽无。带上接口号后左板(或诊断)能区分来源。
//
// 【为什么不做任何解析】
//   用户要求与"独立版"一致: 端点收到什么就转什么。省掉描述符查表/边界推算,
//   回调更快返回 -> 重提交更早 -> 主机下一次轮询更及时(延迟的直接来源)。
// ============================================================================
void EspUsbHost::serial1SendKeyboardRaw(uint8_t iface, const uint8_t *report, uint8_t len)
{
    if (len > 8) len = 8;
    const uint8_t plen = (uint8_t)(1 + len);

    // 串行化整帧的"组装 + 写出"。
    //
    // 本函数有两个调用者: USB 回调任务(真实键盘报文, 最高约 1000/s)和 1ms 任务
    // (周期性重申)。两者共用 s_tx_frame / s_tx_seq, 不加锁会互相撕裂 —— 拼出
    // 半新半旧的帧。左板有 CRC 校验, 坏帧会被丢掉, 但那等于白丢一帧, 而且可能
    // 破坏它的帧同步。这里用临界区保证一帧的原子性。
    // 注意: 只包住组装+写出, 不跨任何阻塞调用, 所以不会变成串行瓶颈。
    if (s_kbd_tx_mtx && xSemaphoreTake(s_kbd_tx_mtx, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;                       // 拿不到锁就丢这一帧; 50ms 后会重申, 不会卡键
    }

    s_tx_frame[0] = (char)0xA5;
    s_tx_frame[1] = (char)0x5C;
    s_tx_frame[2] = (char)plen;
    s_tx_frame[3] = (char)(s_tx_seq++);
    s_tx_frame[4] = (char)0x23;                      // CMD_KB_REPORT
    s_tx_frame[5] = (char)iface;
    for (uint8_t i = 0; i < len; ++i) {
        s_tx_frame[6 + i] = (char)report[i];
    }

    const int crcStart = 2;
    const int crcLen   = 3 + plen;
    uint16_t crc = 0xFFFF;
    for (int i = crcStart; i < crcStart + crcLen; ++i) {
        crc ^= (uint8_t)s_tx_frame[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    s_tx_frame[crcStart + crcLen]     = (char)(crc & 0xFF);
    s_tx_frame[crcStart + crcLen + 1] = (char)((crc >> 8) & 0xFF);

    Serial1.write((const uint8_t *)s_tx_frame, (size_t)(plen + 7));

    if (s_kbd_tx_mtx) xSemaphoreGive(s_kbd_tx_mtx);
}

// ---------------------------------------------------------------------------
// 键盘链路心跳
//
// 【为什么是"心跳"而不是"重发状态"】
//   一开始我把这里写成"每 50ms 重发最后一帧键盘状态", 那是【错的, 而且更危险】:
//   若抬键帧在 接收器 -> 右板端点 这一段就丢了, 右板记住的就是"a 还按着",
//   重发只会把这个错误状态【永久维持】下去 —— 比不重发更糟(不重发至少还有
//   机会靠别的帧纠正)。而左板看到帧一直在来, 兜底也永远不会触发。
//
//   所以心跳必须只表达"链路活着", 【不携带任何按键状态】。用 0x23 帧的
//   payload=[0xFF] 表示 —— 接口号合法范围是 0..kMaxIface-1, 0xFF 不可能与
//   真实报文混淆。
//
// 【它解决什么】
//   让左板能区分两种"收不到键盘报文"的原因:
//     心跳在来 -> 链路活着, 收不到帧只是因为用户一直按着(接收器不再发新帧)
//                 => 绝对不能释放, 否则长按会被误打断
//     心跳也没了 -> 链路死了 => 可以安全地把按键全部释放
//   没有心跳, 这两件事在左板看来完全一样, 左板就不敢做任何保护。
// ---------------------------------------------------------------------------
void EspUsbHost::kbdLinkTick()
{
    const uint32_t now = millis();
    if ((uint32_t)(now - s_kbdHeartbeatMs) < kKbdHeartbeatMs) return;
    s_kbdHeartbeatMs = now;

    // (1) 心跳: 只表达"链路活着", 不带按键状态。
    //     不判 maskIsActive(): 心跳不是输入事件, 屏蔽期间更需要它, 否则左板会
    //     误判链路已死而把按键强行释放。
    serial1SendKeyboardHeartbeat();

    // (2) 状态重申: 把当前键盘状态再送一遍。
    //
    // 【为什么它是安全的(与下面注释里那个被我否掉的方案的区别)】
    //   一开始我担心"重申会把一个错的状态永久维持下去"。那个担心在
    //   接收器->右板 这一段是不成立的: USB 是可靠总线, IN 端点靠 NAK/ACK 重试,
    //   设备发过的报文不会丢。所以 s_kbdRawLast 记的就是设备的【真实状态】,
    //   重申它等于把真相再说一遍, 不会维持错误。
    //   唯一会让它变"陈旧"的情况是转发接口发生切换(旧接口的状态没人再纠正),
    //   那一处已经在 _onReceive 里用"切换瞬间补一帧全空"处理掉了。
    //
    // 【它解决什么】
    //   右板 -> 左板 的 Serial1 是普通串口, 【没有重传】。抬键帧在这里丢一帧,
    //   而设备随后安静(只在变化时发帧)的话, 主机端就永久卡键。每 50ms 重申一次
    //   让丢帧在 50ms 内被自动纠正。
    //
    // 屏蔽窗口内跳过(与转发路径一致): 屏蔽期间真实键盘必须完全静默, 重申会
    // 破坏这一点。链路存活由 (1) 的心跳继续保证。
    if (s_kbdRawLastValid && s_kbdFwdIface >= 0 && !maskIsActive()) {
        serial1SendKeyboardRaw((uint8_t)s_kbdFwdIface, s_kbdRawLast, 8);
    }
}

void EspUsbHost::serial1SendKeyboardHeartbeat()
{
    if (s_kbd_tx_mtx && xSemaphoreTake(s_kbd_tx_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;

    const uint8_t plen = 1;
    s_tx_frame[0] = (char)0xA5;
    s_tx_frame[1] = (char)0x5C;
    s_tx_frame[2] = (char)plen;
    s_tx_frame[3] = (char)(s_tx_seq++);
    s_tx_frame[4] = (char)0x23;                      // 与键盘报文同类型
    s_tx_frame[5] = (char)0xFF;                      // 0xFF = 心跳哨兵(非任何接口号)

    const int crcStart = 2;
    const int crcLen   = 3 + plen;
    uint16_t crc = 0xFFFF;
    for (int i = crcStart; i < crcStart + crcLen; ++i) {
        crc ^= (uint8_t)s_tx_frame[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    s_tx_frame[crcStart + crcLen]     = (char)(crc & 0xFF);
    s_tx_frame[crcStart + crcLen + 1] = (char)((crc >> 8) & 0xFF);

    Serial1.write((const uint8_t *)s_tx_frame, (size_t)(plen + 7));

    if (s_kbd_tx_mtx) xSemaphoreGive(s_kbd_tx_mtx);
}

void EspUsbHost::serial1SendKeyboardBinary(uint8_t modifiers, const uint8_t *keys, uint8_t keyCount)
{
    if (keyCount > 6) keyCount = 6;
    const uint8_t len = (uint8_t)(1 + keyCount);

    s_tx_frame[0] = (char)0xA5;
    s_tx_frame[1] = (char)0x5C;
    s_tx_frame[2] = (char)len;
    s_tx_frame[3] = (char)(s_tx_seq++);
    s_tx_frame[4] = (char)0x23;                      // CMD_KB_REPORT
    s_tx_frame[5] = (char)modifiers;
    for (uint8_t i = 0; i < keyCount; ++i) {
        s_tx_frame[6 + i] = (char)keys[i];
    }

    // CRC over LEN..PAYLOAD: 与 MOVE 帧同一算法(MODBUS, 低字节在前)
    const int crcStart = 2;
    const int crcLen   = 3 + len;
    uint16_t crc = 0xFFFF;
    for (int i = crcStart; i < crcStart + crcLen; ++i) {
        crc ^= (uint8_t)s_tx_frame[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    s_tx_frame[crcStart + crcLen]     = (char)(crc & 0xFF);
    s_tx_frame[crcStart + crcLen + 1] = (char)((crc >> 8) & 0xFF);

    Serial1.write((const uint8_t *)s_tx_frame, (size_t)(len + 7));
}

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
