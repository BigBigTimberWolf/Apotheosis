// ============================================================================
// main.cpp —— 右板 (USB Host) 键盘透传
//
// 职责:
//   1) 用 USB Host 读真键盘接收器
//   2) 把端点收到的【原始字节】通过 Serial1 转给左板 —— 不做任何按键解析
//
// 与老固件的根本差别:
//   老固件会解析报告描述符 -> 判断键盘/鼠标 -> 拆出修饰键和键码 -> 重组成
//   标准 8 字节。中间任何一步判断错(接口协议是 NONE 而非 KEYBOARD、描述符
//   编码方式不同等), 键盘就不工作。这正是之前反复失败的根源。
//
//   现在: 端点收到什么就转什么。不解析就不可能解析错;
//         多媒体键、组合键、厂商自定义键也全部天然保留。
//
// 透传边界 (遵循"能透传的都尽量复制透传"):
//   复制:   报告内容(原始字节)、报文长度、报文先后顺序
//   不复制: 轮询间隔、刷新率 —— 传输层特性, 由左板自己决定
//
// 接线:
//   USB-C   -> 真键盘接收器   (Host 口)
//   USB-B   -> CH343 (Serial) (4000000, 日志)
//   Serial1 -> 左板           (5000000 8N1, RX=2 TX=1)
// ============================================================================

#include <Arduino.h>
#include "EspUsbHost.h"
#include "kbd_wire.h"

// 注意: 不再包含 "efuse.h" —— burn_usb_phy_sel_efuse() 已停止调用, 见 setup() 里的说明。
// efuse.cpp / efuse.h 保留在工程里但成为死代码, 以便将来真要处理 PHY 时能查回原实现。

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#ifndef FIRMWARE_VERSION
    #error "FIRMWARE_VERSION is not defined! Set it in platformio.ini build_flags."
#endif

const char* firmware = TOSTRING(FIRMWARE_VERSION);

EspUsbHost usbHost;

// ---------------------------------------------------------------------------
// 统计
// ---------------------------------------------------------------------------
static uint32_t s_reportCount = 0;
static uint32_t s_frameCount  = 0;
static uint32_t s_idFrames    = 0;
static uint8_t  s_txSeq       = 0;

// ---------------------------------------------------------------------------
// 设备类型标志 (由 esp_usb_host.cpp 在接口描述符里设置)
//
// 这是"一块固件适配两种马克"的数据来源:
//   键盘马克 -> g_isKeyboard = true  -> 左板枚举纯键盘描述符
//   鼠标马克 -> g_isMouse    = true  -> 左板枚举纯鼠标描述符
// ---------------------------------------------------------------------------
volatile bool g_isKeyboard = false;
volatile bool g_isMouse    = false;

// 键盘接口号 (-1 = 未知)。由 esp_usb_host.cpp 在接口描述符里记录。
// 原先定义在被删除的 led_forward.cpp 里, 移到这里。
// 仍然保留: 将来若需要做 SET_REPORT / 异步上报会用到。
int volatile g_kbdInterface = -1;

// ---------------------------------------------------------------------------
// 打包并发给左板: 原样透传
// ---------------------------------------------------------------------------
static void sendToLeft(const uint8_t *payload, int plen)
{
    uint8_t frame[KBD_MAX_FRAME];
    const int n = kbdPackFrame(frame, s_txSeq++, payload, plen);
    if (n > 0) {
        Serial1.write(frame, (size_t)n);
        s_frameCount++;
    }
}

// ---------------------------------------------------------------------------
void setup()
{
    Serial.begin(4000000);                        // CH343, 日志
    Serial1.setRxBufferSize(4096);
    Serial1.begin(5000000, SERIAL_8N1, 2, 1);     // 板间: RX=2 TX=1 (与左板相反)
    delay(1000);

    Serial.println();
    Serial.println("========================================");
    Serial.println("[KBD-HOST] 右板 键盘透传 (USB Host)");
    Serial.println("  模式: 原始字节透传, 不解析按键");
    Serial.println("========================================");

    pinMode(9, OUTPUT);
    usbHost.begin();

    Serial.println("[KBD-HOST] USB Host 已启动, 等待键盘...");

    // ★ 这里原先调用 burn_usb_phy_sel_efuse(), 已删除。原因:
    //
    //   1) 它烧的是 eFuse —— 【不可逆】。这块板子的 USB_PHY_SEL 该不该烧、烧成
    //      哪个值, 是板级出厂决定, 固件不该在每次开机时替用户做这个决定。
    //   2) 它烧完会 esp_restart()。把复位塞在 setup() 里, 一旦烧写返回成功但
    //      位没真正写进去(写保护 / 电压不足 / 部分写), 就会变成【无限重启循环】,
    //      而且从串口看起来只是一直重启, 极难定位。
    //   3) 本工程(M AKCUNEW)当初排查 USB 完全无法枚举时, 就是把 bootSelfTest 和
    //      这个烧写一起禁掉才好的 —— 它已经被认定是可疑项。
    //
    //   官方固件自己会处理 PHY 选择; 而且 USB 控制器在软件里可以用
    //   usb_ll_int_phy_enable() 覆盖 eFuse, 并不依赖这一位。
    //   efuse.cpp / efuse.h 保留但不再被任何地方调用。
}

// ---------------------------------------------------------------------------
// 把真设备的身份 + 角色发给左板 (VID/PID/bcdDevice + role)
//
// 【为什么需要 role】
//   一块固件要同时支持"键盘马克"和"鼠标马克"。
//   左板靠 role 决定:
//     - 枚举【纯键盘】还是【纯鼠标】描述符
//     - 把真实报文解释成键盘还是鼠标
//   role 由右板根据 USB-C 插入设备的接口类型判定。
//
// 【调用时机】
//   由 esp_usb_host.cpp 在 onConfig 里读到设备描述符 / 接口描述符之后调用。
// ---------------------------------------------------------------------------
extern "C" void kbdHostSendIdentity(uint16_t vid, uint16_t pid, uint16_t bcd)
{
    // 角色: 优先用已识别出的设备类型
    uint8_t role = KBD_ROLE_UNKNOWN;
    if (g_isKeyboard)      role = KBD_ROLE_KEYBOARD;
    else if (g_isMouse)    role = KBD_ROLE_MOUSE;

    uint8_t frame[KBD_MAX_FRAME];
    const int n = kbdPackIdEx(frame, s_txSeq++, vid, pid, bcd, role);
    if (n > 0) {
        Serial1.write(frame, (size_t)n);
        s_idFrames++;
        Serial.printf("[KBD-HOST] 告知左板: VID=%04X PID=%04X role=%u(%s)\n",
                      vid, pid, role,
                      role == KBD_ROLE_KEYBOARD ? "键盘" :
                      role == KBD_ROLE_MOUSE    ? "鼠标" : "未知");
    }
}

// ---------------------------------------------------------------------------
// 设备类型标志已在上方定义
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 处理来自左板的帧 (LED 回传)
// ---------------------------------------------------------------------------
static void handleFromLeft(void)
{
    static uint8_t buf[KBD_MAX_FRAME];
    static int len = 0;

    while (Serial1.available() > 0) {
        const uint8_t b = (uint8_t)Serial1.read();

        if (len == 0) {
            if (b != KBD_MAGIC0) continue;
            buf[len++] = b;
            continue;
        }
        if (len == 1) {
            if (b == KBD_MAGIC1)      { buf[len++] = b; }
            else if (b == KBD_MAGIC0) { buf[0] = b; }
            else                      { len = 0; }
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
                // 当前左板不回传数据, 消费掉即可
                len = 0;
            } else if (len > need) {
                len = 0;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 诊断心跳: 把右板的状态经【板间链路】发给左板
//
// 【为什么必须走 Serial1, 而不是 Serial】
//   右板只有一个 USB 口(USB-C), 它同时是"键盘 Host 口"和"接电脑的从口"——
//   接键盘时电脑就连不上, 接电脑时键盘就插不了。所以右板的 Serial 日志
//   在【键盘插着】的时候根本没人看得到, 而恰恰那个时候才需要诊断。
//   代码里原本写的 Serial.begin(4000000) "CH343 日志" 是错的: 右板上没有 CH343。
//
//   Serial1(板间 5Mbps)是唯一在键盘插着时仍然通的通道 -> 状态只能从这里出去,
//   到左板再转到 CH343 打印出来。MAKCUNEW 的右板诊断构建用的就是这个办法。
//
// 【帧类型用 KBD_TYPE_ID (0x25) 而不是 REPORT】
//   左板对【任何】合法帧都会让 real 计数 +1, 但只有 REPORT(0x23) 会改动按键状态。
//   所以用 0x25 发诊断不会误触发按键, 又能让 left 看到"右板活着"。
// ---------------------------------------------------------------------------
static void sendDiagHeartbeat()
{
    uint8_t p[8];
    p[0] = 0x5A;                                   // 标记: 这是诊断帧, 不是真身份
    p[1] = g_isKeyboard ? 1 : 0;
    p[2] = g_isMouse ? 1 : 0;
    p[3] = (uint8_t)(g_kbdInterface < 0 ? 0xFF : g_kbdInterface);
    p[4] = (uint8_t)s_reportCount;                 // 端点收到的报文数(低字节)
    p[5] = (uint8_t)s_frameCount;                  // 转发出去的帧数(低字节)
    p[6] = (uint8_t)s_idFrames;                    // 发过几次身份帧
    p[7] = 0x00;

    uint8_t frame[KBD_MAX_FRAME];
    const int n = kbdPackFrameEx(frame, s_txSeq++, KBD_TYPE_ID, p, sizeof(p));
    if (n > 0) Serial1.write(frame, (size_t)n);
}

// ---------------------------------------------------------------------------
void loop()
{
    handleFromLeft();

    // 心跳: 1 秒一次(先前是 3 秒, 诊断时等太久)
    static uint32_t last = 0;
    const uint32_t now = millis();
    if (now - last >= 1000) {
        last = now;

        // (1) 本地日志(键盘没插时 USB-C 接着电脑才看得到, 平时看不到)
        Serial.printf("[KBD-HOST] reports=%lu frames=%lu id=%lu kbd=%d mouse=%d iface=%d\n",
                      (unsigned long)s_reportCount,
                      (unsigned long)s_frameCount,
                      (unsigned long)s_idFrames,
                      g_isKeyboard ? 1 : 0,
                      g_isMouse ? 1 : 0,
                      (int)g_kbdInterface);

        // (2) ★ 经板间链路送给左板 -> 左板转到 CH343。这是键盘插着时唯一可见的通道。
        sendDiagHeartbeat();
    }
    delay(10);
}

// ---------------------------------------------------------------------------
// 由 esp_usb_host.cpp 的 _onReceive() 调用 —— 拿到端点原始字节就转走
//
// 这里【不做】任何判断: 不区分键盘/鼠标、不解析修饰键和键码。
// 全零报文(所有键松开)也必须转发, 否则被控机会一直以为按键被按住。
// ---------------------------------------------------------------------------
extern "C" void kbdHostOnRawReport(const uint8_t *data, int len)
{
    if (!data || len <= 0) return;
    if (len > KBD_MAX_REPORT) len = KBD_MAX_REPORT;

    s_reportCount++;
    sendToLeft(data, len);
}
