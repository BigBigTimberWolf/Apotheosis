#include "main.h"
#include "tusb.h"   // tud_mounted()
#include "efuse.h"
#include "esp_efuse.h"        // esp_efuse_read_field_bit
#include "esp_efuse_table.h"  // ESP_EFUSE_USB_PHY_SEL
#include "oc.h"
#include "diag.h"

// 诊断计数器 (定义在 handleCommands.cpp)
// 左板是链路下游, 这组数能区分断点在右板还是在本级发送口。
extern uint32_t volatile g_diagMoveRx;
extern uint32_t volatile g_diagBtnRx;
extern uint32_t volatile g_diagEmitOk;
extern uint32_t volatile g_diagEmitFail;
extern uint32_t volatile g_diagMoved;

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#if USB_IS_DEBUG
    #warning "DEBUG MODE ENABLED: USB host will not work! For logging purposes only."
#endif

// Ensure the FIRMWARE_VERSION is defined
#ifndef FIRMWARE_VERSION
    #error "FIRMWARE_VERSION is not defined! Please set FIRMWARE_VERSION in the build flags."
#endif

const char* firmware = TOSTRING(FIRMWARE_VERSION);

void setup() {
    delay(1100);
    Serial0.begin(115200);
    pinMode(9, OUTPUT);
    digitalWrite(9, LOW);
    // 必须在下行 begin() 之前设置: arduino-esp32 的 UART RX 环缓冲默认只有 256 字节,
    // 5Mbps 下约 0.5ms 就写满, 端点描述符 JSON 可达 ~1700 字节 -> 必然丢字节 -> JSON 截断.
    Serial1.setRxBufferSize(8192);
    Serial1.begin(5000000, SERIAL_8N1, 1, 2);

    if (USB_IS_DEBUG) {
        Serial0.println("WARNING: Debug mode is enabled!");
        Serial0.println("USB Host will not work!");
        Serial0.println("For logging purposes only!!!\n");
    }

    Serial0.print("MAKCU-PASSTHROUGH ");
    Serial0.print(firmware);
    Serial0.println("\n");

    Serial0.onReceive(serial0ISR);
    Serial1.onReceive(serial1ISR);

    // ===== 诊断: 先读出 USB_PHY_SEL eFuse 的真实值 =====
    //
    // 【为什么必须先读】
    //   USB_PHY_SEL 决定 USB 引脚接到哪个 PHY:
    //     未烧(0) -> 内部 USB-OTG PHY  -> TinyUSB 可用
    //     已烧(1) -> USB-Serial-JTAG    -> TinyUSB 无法工作(mounted 恒为 0)
    //
    //   burn_usb_phy_sel_efuse() 会【不可逆地】烧写这一位。若烧错, 这块板的
    //   USB Device 功能就永久失效。所以在弄清原因前【绝不能再调用它】。
    {
        const int phySel = (int)esp_efuse_read_field_bit(ESP_EFUSE_USB_PHY_SEL);
        Serial0.printf("[EFUSE] USB_PHY_SEL=%d (%s)\n", phySel,
                       phySel ? "已烧 -> USB 走 Serial-JTAG, TinyUSB 不可用"
                              : "未烧 -> USB 走 OTG PHY, TinyUSB 可用");
    }

    // ★ 已停用: 这个调用会【不可逆烧写 eFuse】, 在确认它对 USB 的影响前不能执行。
    //   我的 KBD_PASSTHROUGH 固件没有这个调用, 那块板的 USB 完全正常。
    // #if !FW_DIAG
    //     burn_usb_phy_sel_efuse();
    // #endif

    // 固件初始化: 超频自检 -> 协议接线 -> 任务创建
    // ★ 暂时停用超频自检: 日志里它每次都失败(260MHz 不被允许),
    //   而 KBD_PASSTHROUGH 没有这一步却正常。先排除这个变量。
    // #if !FW_DIAG
    //     oc::bootSelfTest();
    // #endif
    protoInit();
    xTaskCreate([](void *) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        oc::confirmStable();
        vTaskDelete(NULL);
    }, "ocConfirm", 2048, NULL, 1, NULL);

    tasks();

#if FW_DIAG
    diagStart();
#endif
}

void loop() {
    if (!USB_IS_DEBUG) {
        requestUSBDescriptors();
    }

    // ===== 诊断心跳 =====
    // MAKCUNEW 原来只在开机打印一次, 事后接串口什么都看不到, 无法排查
    // "设备没枚举"这类问题。这里每 3 秒报一次链路状态。
    //
    // 左板是链路下游, 这组数字能一次定位断点:
    //   moveRx/btnRx      = 右板送来多少真实鼠标输入
    //   emitOk/emitFail   = 本级发出多少、失败多少
    //     moveRx 一直 0            -> 右板没发(断点在上游)
    //     moveRx 涨但 emitFail 也涨 -> 发送口问题(A 修的就是这个)
    static uint32_t s_lastStat = 0;
    const uint32_t now = millis();
    if (now - s_lastStat >= 3000) {
        s_lastStat = now;
        Serial0.printf("[STAT] ready=%d mounted=%d | s1bytes=%lu | rx move=%lu btn=%lu | emit ok=%lu fail=%lu moved=%lu\n",
                       (int)isUsbReadyToTransfer(),
                       (int)tud_mounted(),
                       (unsigned long)g_diag_rx1_bytes,
                       (unsigned long)g_diagMoveRx,
                       (unsigned long)g_diagBtnRx,
                       (unsigned long)g_diagEmitOk,
                       (unsigned long)g_diagEmitFail,
                       (unsigned long)g_diagMoved);

        // 首次收到 Serial1 数据时, 把头几十字节的十六进制打出来 ——
        // 这样能直接看出右板发的是什么(结构化帧? 原始报告? 还是乱码?)
        static bool s_snapDone = false;
        if (!s_snapDone && g_diag_snap_len > 0) {
            s_snapDone = true;
            Serial0.printf("[S1RAW] %lu 字节: ", (unsigned long)g_diag_snap_len);
            for (uint32_t i = 0; i < g_diag_snap_len && i < 40; ++i) {
                Serial0.printf("%02X ", g_diag_snap[i]);
            }
            Serial0.println();
        }
    }
}
