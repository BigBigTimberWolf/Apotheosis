// ============================================================
// diag.cpp (右板 / fw_host) - 诊断固件（仅 -DFW_DIAG=1 时编译）
//
// 为什么右板需要单独的诊断: 右板是焊死的整板, 【没有 CH343】, 没有任何
// 能直接给电脑读的串口; 而它偏偏是最容易出问题的一侧(USB Host 枚举真实
// 鼠标)。左板有 CH343 可以随时看, 右板基本是黑盒。
//
// 两条串口的真实身份(务必记住):
//   左板: Serial0 = UART0 -> CH343 -> 电脑(115200) | Serial1 = 板间链路
//   右板: Serial1 = 板间链路(5000000)              | Serial0 = 4000000, 无人读
//
// 本文件提供【两条独立的观测通道】:
//
//   通道 A: "#HBT" 心跳 + 状态行, 经【板间链路 Serial1】发给左板, 左板
//           把它转到自己的 CH343 给电脑看。左板侧的转发见 fw_device/
//           src/handleCommands.cpp 里 FW_DIAG 分支。
//           ★ 但这条通道【依赖正在被诊断的链路本身】, 链路一断就什么
//             都看不到 —— 这正是本次排查要区分的情况, 所以不能只靠它。
//
//   通道 B: GPIO9 LED 闪码 —— 【不依赖任何通信】, 链路全断也能观测。
//           这是最可靠的通道, 定位链路故障时必须以它为准。
//
// 生产固件里两处高风险动作在本构建中被禁用:
//   - burn_usb_phy_sel_efuse()  一次性不可逆 eFuse
//     ★ 对右板尤其关键: 这个 efuse 决定 S3 唯一的 USB PHY 给 Device 还是 Host。
//       右板必须是 Host, 一旦烧错方向 【不可逆】。诊断构建绝不烧它,
//       只把当前值读出来报告 —— 这本身就是本次要查的头号嫌疑。
// ============================================================
#include "diag.h"

#if FW_DIAG

#include <Arduino.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp32-hal-cpu.h>
#include "EspUsbHost.h"

// 右板的 Serial0 是板间链路(4000000), 用 serial1Send 那套发会让左板按命令解析;
// 这里直接往同一物理链路打带前缀的普通文本行, 左板认不出就转给 PC。
extern EspUsbHost usbHost;

static const int LED_PIN = 9;

// 右板固件用 digitalWrite(9, HIGH) 点亮
static inline void led(bool on) { digitalWrite(LED_PIN, on ? HIGH : LOW); }

static void pulse(int ms) {
    led(true);
    vTaskDelay(pdMS_TO_TICKS(ms));
    led(false);
}

// 诊断行 "DIAG|"。
//
// 【通道选择 — 这里原来写错了, 已修正】
//
// 事实: 右板是焊死的整板, 【没有 CH343】。它没有任何能直接给电脑读的串口。
//   左板: Serial0 = UART0 -> CH343 -> 电脑(115200), Serial1 = 板间链路
//   右板: Serial1 = 板间链路(5000000)  ← 所有 serial1Send() 都走这条
//         Serial0 = 4000000, 没有芯片往外送, 发出去【没人能看见】
//
// 所以右板只有两条真实可用的输出:
//   1) LED9 闪码            —— 不依赖任何通信, 最可靠
//   2) Serial1 -> 左板 -> 左板 CH343 -> 电脑   —— 依赖板间链路本身
//
// 上一版的错误: 以为右板 Serial0 是板间链路, 于是既发错口、又在注释里
// 写反了模型。现在按事实修正。
//
// 注意这里【故意只发 Serial0】的原因也要说清: 诊断行走 Serial1 会污染
// 正在被诊断的那条链路(左板 s1_rx 恒 0 时根本收不到, 等于用坏掉的通道
// 去诊断它自己, 是循环依赖)。所以完整状态行走 Serial0(无害, 反正没人读),
// 而 Serial1 上【只发一个极短的心跳】专门用来验证链路是否通 —— 见下方
// "#HBT" 探测, 那是本次排查的核心手段。
static void diagLine(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // ★ 同时写 Serial0 和 Serial1。
    //
    // 【为什么必须写 Serial1】
    //   右板是焊死的整板, 【没有 CH343】—— Serial0 上的内容没有任何人能读到。
    //   原来只写 Serial0, 等于把诊断信息写进了虚空: 排查右板问题时两眼一抹黑
    //   (实测 s1bytes 只有 #HBT 的 ~2 B/s, DIAG 行完全没过来)。
    //
    //   写 Serial1 后, 这些行经板间链路到左板, 左板把它们转到自己的 CH343,
    //   电脑就能看到右板内部状态了 —— 这是观测右板的唯一通道。
    //
    // 开销: 每 2 秒约 110 字节, 板间链路 5Mbps, 可忽略。
    Serial0.println(buf);
    Serial1.println(buf);
}

static void diagTask(void *) {
    pinMode(LED_PIN, OUTPUT);
    led(false);

    Serial0.println();
    diagLine("DIAG| ========== RIGHT (fw_host) diagnostic build ==========");
    diagLine("DIAG| efuse burn: DISABLED  (USB_PHY_SEL 绝不在诊断构建里改写)");

    // ---- 头号嫌疑: USB_PHY_SEL 当前值 ----
    //
    // 0 = PHY 给 Device (CDC/串口那侧)
    // 1 = PHY 给 Host   (右板需要的值)
    // 读不出来时会打印 -1, 表示该 efuse 位不可读/未实现。
    int phy = -1;
    if (esp_efuse_read_field_bit(ESP_EFUSE_USB_PHY_SEL)) phy = 1;
    else phy = 0;
    diagLine("DIAG| USB_PHY_SEL=%d (0=Device侧, 1=Host侧; 右板必须是 1)", phy);

    // ---- LED 极性自检: 4 段, 每段 2.5s, HIGH/LOW 交替 ----
    diagLine("DIAG| LED polarity check: HIGH,LOW,HIGH,LOW (each 2.5s)");
    for (int i = 0; i < 2; i++) {
        digitalWrite(LED_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(2500));
        digitalWrite(LED_PIN, LOW);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
    led(false);
    diagLine("DIAG| polarity check done -> status loop");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        const int connected = EspUsbHost::deviceConnected ? 1 : 0;
        const int mouseRdy  = EspUsbHost::deviceMouseReady ? 1 : 0;
        const int ready     = usbHost.isReady ? 1 : 0;
        const int susp      = usbHost.deviceSuspended ? 1 : 0;

        // 关键: usbTransferSize > 0 表示已经为某个端点提交了传输。
        // 如果它是 0, 说明 C 口设备压根没枚举/没有可用端点 —— 这才是
        // "C 口灯不闪" 的直接原因(灯只在 _onReceive 收到报文时闪)。
        const int xferCount = (int)usbHost.usbTransferSize;

        // ---- 板间链路活性探测 ----
        //
        // 心跳直接走 Serial1(真正的板间链路), 内容是一个左板必然识别为
        // 【无效命令】的短行 —— 左板会把它丢给 handleDebugcommand 静默丢弃,
        // 不干扰握手的任何状态机, 但足以让左板 s1_rx 计数器增长。
        //
        // 判据:
        //   左板 s1_rx 增长  -> 链路是通的, 问题在协议/握手层
        //   左板 s1_rx 恒 0  -> 链路本身不通(引脚/波特率/硬件)
        //
        // 用 '#' 开头而不是 'km.' 开头: 绝不会被误认为是任何已知命令。
        Serial1.print("#HBT\n");
        Serial1.flush();

        diagLine("DIAG| up=%lus cpu=%uMHz phy=%d heap=%u conn=%d mouseRdy=%d "
                 "ready=%d susp=%d xfer=%d lastAct=%lu rx0=%lu rx1=%lu",
                 (unsigned long)(millis() / 1000),
                 (unsigned)getCpuFrequencyMhz(),
                 phy,
                 (unsigned)ESP.getFreeHeap(),
                 connected, mouseRdy, ready, susp, xferCount,
                 (unsigned long)usbHost.last_activity_time,
                 (unsigned long)Serial0.available(),
                 (unsigned long)Serial1.available());

          // ===== 链路逐级计数 =====
          //   定位"卡在哪一级": 每个数都是上一级的输出、下一级的输入。
          //   reports -> mouseGate -> decoded -> fwd 任何一级为 0 就是卡点。
          extern uint32_t volatile g_rxReports;
          extern uint32_t volatile g_rxMouseGate;
          extern uint32_t volatile g_rxDecoded;
          extern uint32_t volatile g_rxFwd;
          diagLine("DIAG| CHAIN reports=%lu mouseGate=%lu decoded=%lu fwd=%lu",
                   (unsigned long)g_rxReports,
                   (unsigned long)g_rxMouseGate,
                   (unsigned long)g_rxDecoded,
                   (unsigned long)g_rxFwd);

          // 描述符解析结果 (鼠标解码的 layoutOk 判定直接依赖这些值)
          diagLine("DIAG| DESC valid=%d bSz=%d bStart=%d xStart=%d xSz=%d yStart=%d ySz=%d whSz=%d rid=%d hasRid=%d",
                   (int)(usbHost.descPerIfaceValid[0] ? 1 : 0),
                   (int)usbHost.descPerIface[0].buttonSize,
                   (int)usbHost.descPerIface[0].buttonStartByte,
                   (int)usbHost.descPerIface[0].xAxisStartByte,
                   (int)usbHost.descPerIface[0].xAxisSize,
                   (int)usbHost.descPerIface[0].yAxisStartByte,
                   (int)usbHost.descPerIface[0].yAxisSize,
                   (int)usbHost.descPerIface[0].wheelSize,
                   (int)usbHost.descPerIface[0].reportId,
                   (int)(usbHost.descPerIface[0].hasReportId ? 1 : 0));

        // ---- LED 状态指示: 唯一不依赖通信的观测通道 ----
        //
        // 设计原则: 用户只能"看灯", 不能数节拍。所以【不数闪次】, 只用
        // 四种节奏差异极大、一眼可分的行为来表达状态:
        //
        //   ① 完全安静(2 秒灭, 几乎不亮)  = 右芯片固件没跑起来 / 没电
        //   ② 慢闪  亮1.0s 灭1.0s         = 固件在跑, 但 C 口【没枚举到设备】
        //                                    (xfer==0: 没有可用端点)
        //   ③ 中闪  亮0.3s 灭0.3s         = C 口枚举到了, 但鼠标【没数据过来】
        //                                    (xfer>0 但 deviceConnected==0)
        //   ④ 快闪  亮0.08s 灭0.08s       = 一切正常, 正在收 C 口数据
        //                                    (收到报文时 refresh)
        //
        // ②和③的区别在于"设备有没有被认出来", ④则证明数据链路完全通了。
        // 这四种节奏的周期差了近 10 倍, 不需要计数, 肉眼一看就能归类。
        const bool gotData = (usbHost.last_activity_time != 0) &&
                             ((uint32_t)(millis() - usbHost.last_activity_time) < 1500);

        if (gotData) {
            // ④ 快闪: 有真实数据在流
            led(true);  vTaskDelay(pdMS_TO_TICKS(80));
            led(false); vTaskDelay(pdMS_TO_TICKS(80));
        } else if (connected) {
            // ③ 中闪: 设备认到了, 但没数据
            led(true);  vTaskDelay(pdMS_TO_TICKS(300));
            led(false); vTaskDelay(pdMS_TO_TICKS(300));
        } else if (xferCount > 0) {
            // ③ 中闪: 端点已提交, 但设备还没连上
            led(true);  vTaskDelay(pdMS_TO_TICKS(300));
            led(false); vTaskDelay(pdMS_TO_TICKS(300));
        } else {
            // ② 慢闪: 固件活着(能走到这里就是活着), 但没有任何端点
            led(true);  vTaskDelay(pdMS_TO_TICKS(1000));
            led(false); vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void diagStart() {
    xTaskCreatePinnedToCore(diagTask, "RightDiag", 4096, NULL, 1, NULL, 0);
}

#endif  // FW_DIAG
