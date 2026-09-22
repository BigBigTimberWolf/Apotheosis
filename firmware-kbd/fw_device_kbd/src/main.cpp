// ============================================================================
// main.cpp —— 左板 (USB Device): 真实输入透传 + 上位机注入
//
// 【角色自动识别】
//   一块固件同时支持键盘马克和鼠标马克。
//   右板检测 USB-C 插入的是键盘还是鼠标, 通过身份帧告知左板,
//   左板据此决定:
//     - 枚举哪种 USB 描述符 (纯键盘 / 纯鼠标)
//     - 如何解释真实设备报文
//
// 【数据流】
//   右板 ──Serial1──> 真实设备原始报文 ──┐
//                                         ├─> 合并 ─> USB ─> 被控机
//   上位机 ─CH343──> makcu 注入命令     ──┘
//
// 【接线】
//   USB-A   -> 被控机
//   USB-B   -> CH343 (上位机, 115200)
//   Serial1 -> 右板  (5000000 8N1, RX=1 TX=2)
// ============================================================================

#include <Arduino.h>
#include "kbd_usb.h"
#include "kbd_wire.h"
#include "makcu_proto.h"
#include "ascii_cmd.h"

// ---- makcu_link.cpp 提供的接口 ----
void makcuLinkFeedPc(uint8_t b);
void makcuLinkFeedHost(uint8_t b);
void makcuLinkTick(void);
void makcuLinkStats(uint32_t *cmds, uint32_t *crcs, uint32_t *realFrames,
                    uint32_t *ghostDrops, bool *ghost);
bool makcuLinkAlive(void);
bool makcuMaskActive(void);

// ---- 设备角色 (定义在 makcu_link.cpp) ----
extern int volatile g_deviceRole;

// ---- 延迟弹起用的全局 (定义在 makcu_link.cpp 外部引用) ----
uint32_t volatile g_clickReleaseAt = 0;
uint8_t  volatile g_clickBits      = 0;

// ---------------------------------------------------------------------------
// 重启请求
// ---------------------------------------------------------------------------
static volatile bool s_rebootReq = false;
void makcuRequestReboot(void) { s_rebootReq = true; }

// ---- 由 usb_desc.cpp 提供 ----
// 必须用 extern "C": usb_desc.cpp 里按 C 链接定义, 否则名字修饰不匹配。
extern "C" void kbdDescSetIdentity(uint16_t vid, uint16_t pid, uint16_t bcd);

// ---------------------------------------------------------------------------
// 等右板送来设备身份 (VID/PID + 角色)
//
// 左板必须在枚举前知道:
//   - VID/PID  -> 填自己的描述符
//   - 设备角色 -> 决定暴露键盘还是鼠标
// 所以这里阻塞等待, 带超时。
// ---------------------------------------------------------------------------
#define ID_WAIT_MS   2000

static void waitForIdentity(void)
{
    uint8_t buf[KBD_MAX_FRAME];
    int len = 0;
    const uint32_t t0 = millis();

    while (millis() - t0 < ID_WAIT_MS) {
        while (Serial1.available() > 0) {
            const uint8_t b = (uint8_t)Serial1.read();

            if (len == 0) {
                if (b != KBD_MAGIC0) continue;
                buf[len++] = b;
                continue;
            }
            if (len == 1) {
                if (b == KBD_MAGIC1)      buf[len++] = b;
                else if (b == KBD_MAGIC0) buf[0] = b;
                else                      len = 0;
                continue;
            }

            buf[len++] = b;

            if (len == 5) {
                const int plen = buf[2];
                if (plen <= 0 || plen > KBD_MAX_REPORT) { len = 0; continue; }
            }
            if (len >= 5) {
                const int need = 5 + buf[2] + 2;
                if (len == need) {
                    uint8_t seq = 0, type = 0;
                    const uint8_t *pl = nullptr;
                    const int n = kbdUnpackFrameEx(buf, len, &seq, &type, &pl);

                    // 身份帧 payload: vid(2) pid(2) bcd(2) role(1), 共 KBD_ID_LEN = 7 字节
                    //
                    // ★★ 必须排除【诊断帧】★★
                    //   诊断帧用的是同一个 KBD_TYPE_ID 类型, 但 payload 是
                    //     [0x5A, isKbd, isMouse, iface, repLo, repHi, idFrames, ...]  长度 8
                    //   原来这里只查 type 和 n>=1, 于是右板每秒一发的诊断帧被当成
                    //   身份帧吞掉, 而且立刻 return:
                    //     vid  = 0x5A | (isKbd=1)<<8 = 0x015A
                    //     pid  = isMouse(1) | iface(0)<<8 = 0x0001
                    //     role = pl[6] = idFrames = 0
                    //   然后 `if (role) g_deviceRole = role;` 里 role=0 为假 ->
                    //   g_deviceRole 永远是 0 -> makcuLinkFeedHost 里 role==1/==2
                    //   两个分支都不进 -> 【真实报文全部被丢弃】-> 按键盘完全没反应。
                    //
                    //   修法: 认准 0x5A 标记排除诊断帧; 只在拿到【完整且可用】的
                    //   身份(含非零 role)时才结束等待; 否则继续等 -> 超时后走下面
                    //   的 g_deviceRole = 1(键盘) 兜底。
                    if (type == KBD_TYPE_ID && pl[0] != 0x5A) {
                        const uint16_t vid  = (uint16_t)(pl[0] | (pl[1] << 8));
                        const uint16_t pid  = (uint16_t)(pl[2] | (pl[3] << 8));
                        const uint16_t bcd  = (uint16_t)(pl[4] | (pl[5] << 8));
                        const uint8_t  role = (n >= KBD_ID_LEN) ? pl[6] : 0;

                        if (vid && pid && role) {
                            kbdDescSetIdentity(vid, pid, bcd);
                            g_deviceRole = role;
                            Serial.printf("[KBD-DEV] 身份: VID=%04X PID=%04X role=%u(%s)\n",
                                          vid, pid, role,
                                          role == 1 ? "键盘" : role == 2 ? "鼠标" : "未知");
                            len = 0;
                            return;
                        }
                        // 不完整或 role==0: 丢掉这一帧, 继续等真身份帧
                    }
                    len = 0;
                } else if (len > need) {
                    len = 0;
                }
            }
        }
        delay(5);
    }
    Serial.println("[KBD-DEV] 未收到身份, 默认 3554:FA09 / 角色=键盘");
    g_deviceRole = 1;
}

// ---------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);                        // CH343: 上位机 + 日志
    Serial1.setRxBufferSize(4096);
    Serial1.begin(5000000, SERIAL_8N1, 1, 2);    // 右板: RX=1 TX=2

    delay(50);

    Serial.println();
    Serial.println("========================================");
    Serial.println("[KBD-DEV] 左板: 透传 + 注入");
    Serial.println("  日志会在收到上位机首帧后静音");
    Serial.println("========================================");

    waitForIdentity();
    kbdUsbBegin();

    Serial.printf("[KBD-DEV] USB 就绪, 角色=%s\n",
                  g_deviceRole == 1 ? "键盘" : g_deviceRole == 2 ? "鼠标" : "未知");
    pinMode(LED_BUILTIN, OUTPUT);
}

// ---------------------------------------------------------------------------
void loop()
{
    // ---- CH343: 上位机注入命令 ----
    while (Serial.available() > 0) {
        makcuLinkFeedPc((uint8_t)Serial.read());
    }

    // ---- Serial1: 右板来的真实设备报文 ----
    while (Serial1.available() > 0) {
        makcuLinkFeedHost((uint8_t)Serial1.read());
    }

    // ---- 合并 + 发射 ----
    makcuLinkTick();

    // ---- 重启请求 ----
    if (s_rebootReq) {
        delay(50);
        ESP.restart();
    }

    // ---- 日志 ----
    // ★ 一旦上位机开口就永久静音, 避免文本污染协议流。
    //
    //   判据用 asciiCmdActive() 而不是 makcuLinkAlive(): 后者要等到【第一帧
    //   合法二进制命令】才算活着, 而上位机连接时先走的是 ASCII 握手 ——
    //   那段时间里本板的日志仍会照常打出去, 混进上位机的文本通道。
    //   这里提前到"收到第一个可打印字节"就静音。
    static uint32_t lastLog = 0;
    const uint32_t now = millis();
    if (!asciiCmdActive() && !makcuLinkAlive() && now - lastLog >= 2000) {
        lastLog = now;
        uint32_t c, crc, rf, gd; bool gh;
        makcuLinkStats(&c, &crc, &rf, &gd, &gh);
        Serial.printf("[KBD-DEV] role=%d cmds=%lu real=%lu ghost=%d/%lu mask=%d crc=%lu\n",
                      g_deviceRole,
                      (unsigned long)c, (unsigned long)rf,
                      gh ? 1 : 0, (unsigned long)gd,
                      makcuMaskActive() ? 1 : 0,
                      (unsigned long)crc);
    }
}
