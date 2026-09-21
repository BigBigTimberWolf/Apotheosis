// ============================================================
// diag.cpp - 诊断固件（仅 -DFW_DIAG=1 时编译）
//
// 生产固件只在 setup() 打印一次横幅，串口错过上电瞬间就再也看不到信息。
// 本文件提供两条独立的观测通道：
//
//   通道 A: Serial0 (UART0, 115200 → CH343) 每 2 秒输出完整状态行
//   通道 B: GPIO9 LED 闪码（串口读不到时靠数闪灯定位）
//
// 并在启动时先做一次 LED 极性自检，因为高/低电平点亮无法从代码确定。
//
// 同时生产固件里两处高风险动作在本构建中被禁用：
//   - burn_usb_phy_sel_efuse()  一次性不可逆 eFuse
//   - oc::bootSelfTest()        260MHz 超频（已确认是空操作，但仍不执行）
// ============================================================
#include "diag.h"

// ============================================================================
// ★ 计数器【无条件定义】—— 不再包在 FW_DIAG 里
//
// 原因: s1bytes(Serial1 收到的原始字节数) 是排查"右板到底有没有发数据"的
// 唯一依据。生产固件也必须能看, 否则一旦 rx=0 就无法区分
//   (a) 右板根本没发    (b) 有字节但帧解析不出来
// 开销只是几个自增, 可以忽略。
// ============================================================================
volatile uint32_t g_diag_rx1_bytes = 0;
volatile uint32_t g_diag_rx1_lines = 0;
volatile uint32_t g_diag_max_line = 0;

// 右板 "#HBT" 心跳的行数。用于判定板间链路是否物理连通。
// 它由 handleCommands.cpp 的 serial1RX() 在识别到 "#HBT" 行时自增。
volatile uint32_t g_diag_hbt_count = 0;

// Serial1 前 64 个字节原样存下来，用于判断右板到底发了什么
// (结构化帧 / 原始报告 / 乱码 / 完全没数据)
#define DIAG_SNAP 64
volatile uint32_t g_diag_snap_len = 0;
uint8_t g_diag_snap[DIAG_SNAP];

#if FW_DIAG

#include <Arduino.h>
#include <esp32-hal-cpu.h>
#include "USBSetup.h"
#include "handleCommands.h"

extern bool processingUsbCommands;

static const int LED_PIN = 9;

// 右板固件用 digitalWrite(9, HIGH) 点亮，故这里默认按高电平点亮；
// 开机自检会把真实极性告诉用户。
static inline void led(bool on) { digitalWrite(LED_PIN, on ? HIGH : LOW); }

static void pulse(int ms) {
    led(true);
    vTaskDelay(pdMS_TO_TICKS(ms));
    led(false);
}

static void printSnap() {
    uint32_t n = g_diag_snap_len;
    if (n > DIAG_SNAP) n = DIAG_SNAP;
    Serial0.printf("[DIAG] s1_first_bytes(%lu): ", (unsigned long)g_diag_snap_len);
    for (uint32_t i = 0; i < n; i++) Serial0.printf("%02X ", g_diag_snap[i]);
    Serial0.print(" | ascii: ");
    for (uint32_t i = 0; i < n; i++) {
        char c = (char)g_diag_snap[i];
        Serial0.print((c >= 32 && c < 127) ? c : '.');
    }
    Serial0.println();
}

static void diagTask(void *) {
    // ---- LED 极性自检：4 段，每段 2.5 秒，依次 HIGH / LOW / HIGH / LOW ----
    Serial0.println("[DIAG] LED polarity check: 4 phases x 2.5s = HIGH,LOW,HIGH,LOW");
    for (int i = 0; i < 2; i++) {
        digitalWrite(LED_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(2500));
        digitalWrite(LED_PIN, LOW);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
    led(false);
    Serial0.println("[DIAG] polarity check done -> status loop");
    printSnap();

    int snap_reported = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        int      idx   = currentCommandIndex;
        int      conn  = deviceConnected ? 1 : 0;
        int      ready = isUsbReadyToTransfer() ? 1 : 0;
        uint32_t rx    = g_diag_rx1_bytes;
        uint32_t lines = g_diag_rx1_lines;

        // hbt 是判定板间链路是否【物理连通】的决定性指标。
        // 右板诊断固件每 2 秒发一行 "#HBT"(见 fw_host/src/diag.cpp) —— 那是一个
        // 左板必然不认识的命令, 会被 handleDebugcommand 静默丢弃, 不干扰任何
        // 握手状态机, 但只要它到了, 就证明这两块板之间的 UART 是真的通的。
        //
        //   链路通   -> hbt 每 2 秒 +1, 而 s1_rx 同时增长
        //   链路不通 -> hbt 恒 0 且 s1_rx 恒 0 (此时右板 LED 仍在按闪码跑,
        //               说明右板固件本身是活的, 问题纯粹在这条线上)
        Serial0.printf("[DIAG] up=%lus cpu=%uMHz s1_rx=%lu s1_lines=%lu s1_maxline=%lu hbt=%lu conn=%d proc=%d qidx=%d usb_ready=%d heap=%u\n",
                       (unsigned long)(millis() / 1000),
                       (unsigned)getCpuFrequencyMhz(),
                       (unsigned long)rx,
                       (unsigned long)lines,
                       (unsigned long)g_diag_max_line,
                       (unsigned long)g_diag_hbt_count,
                       conn,
                       processingUsbCommands ? 1 : 0,
                       idx,
                       ready,
                       (unsigned)ESP.getFreeHeap());

        // 右板一旦开始说话就把原始字节抓出来看（只印一次，避免刷屏）
        if (!snap_reported && g_diag_snap_len > 0) {
            printSnap();
            snap_reported = 1;
        }

        // ---- LED 闪码（每 2 秒一轮，高电平点亮）----
        //   第 1 组: 1 次长闪(500ms)      = 固件在跑
        //            之后 N 次短闪(110ms) = qidx，握手进行到第几步 (0~9)
        //   超长闪(1200ms) = 两组之间的分隔符
        //   第 2 组: M 次短闪(110ms)     = 右板发来的最长行长 / 256 字节
        //             (用于验证"JSON 超长被截断"这个判断: 若 M>=3 即 >=768 字节)
        //   最后 3 次急闪(80ms)          = USB 已就绪
        pulse(500);
        vTaskDelay(pdMS_TO_TICKS(350));

        for (int i = 0; i < idx && i < 12; i++) {
            pulse(110);
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if (ready) {
            vTaskDelay(pdMS_TO_TICKS(400));
            for (int i = 0; i < 3; i++) {
                pulse(80);
                vTaskDelay(pdMS_TO_TICKS(120));
            }
        }

        // 分隔符: 一次超长闪
        vTaskDelay(pdMS_TO_TICKS(700));
        pulse(1200);
        vTaskDelay(pdMS_TO_TICKS(700));

        uint32_t m = g_diag_max_line / 256;
        if (m > 12) m = 12;
        for (uint32_t i = 0; i < m; i++) {
            pulse(110);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

void diagStart() {
    pinMode(LED_PIN, OUTPUT);
    led(false);
    Serial0.println();
    Serial0.println("[DIAG] ========== diagnostic build ==========");
    Serial0.println("[DIAG] efuse burn: DISABLED / overclock: DISABLED");
    Serial0.println("[DIAG] Serial0 status every 2s @115200");
    xTaskCreatePinnedToCore(diagTask, "Diag", 3072, NULL, 1, NULL, 0);
}

#endif  // FW_DIAG
