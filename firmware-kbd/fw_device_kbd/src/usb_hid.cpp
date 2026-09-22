// ============================================================================
// usb_hid.cpp —— HID 类回调 + 键盘报文发送 (单接口版)
//
// 只有一个 HID 接口 -> 只有 instance 0。
// 接口内有两个 Top-Level Collection (键盘 @RID1, 鼠标 @RID2)。
//
// 【透传时 Report ID 的处理】
//   接口内有两个 Collection, 所以报文必须带 Report ID 才能区分。
//   真键盘的原始报文通常【不带】Report ID (标准 boot keyboard 是 8 字节)。
//   因此发送时:
//     1) 若是标准 8 字节键盘报文 -> 前面补 Report ID 1, 变成 9 字节发出
//     2) 若原报文已带 Report ID -> 用右板传来的 rid 字段(见 kbd_wire.h 扩展)
//   这里只做补/换 Report ID, 键码字节【一个都不改】。
// ============================================================================

#include "tusb.h"
#include "class/hid/hid_device.h"
#include "kbd_desc.h"
#include "kbd_usb.h"
#include "kbd_wire.h"
#include <string.h>

// ---------------------------------------------------------------------------
// 报告描述符 —— 单接口, 只有 instance 0
// ---------------------------------------------------------------------------
static const uint8_t s_report_desc[] = { KBD_REPORT_DESC };

// 编译期校验: 宏展开长度必须与 kbd_desc.h 声明一致。
// 不一致会导致主机的描述符长度与内容不符 -> 设备被拒绝。
static_assert(sizeof(s_report_desc) == KBD_REPORT_DESC_LEN,
              "report descriptor length mismatch - run count_desc.py");

// ---------------------------------------------------------------------------
// ★ 报告描述符回调 —— Arduino USBHID 在这里是无脑返回同一份拼接缓冲,
//    我们直接返回本接口真正的描述符。
// ---------------------------------------------------------------------------
extern "C" uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_report_desc;      // 只有一个接口
}

// ---------------------------------------------------------------------------
// GET_REPORT: 键盘是被动上报, 这里返回 0。
// ---------------------------------------------------------------------------
extern "C" uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                          hid_report_type_t report_type,
                                          uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

// ---------------------------------------------------------------------------
// SET_REPORT: 被控机下发键盘 LED 状态 (Caps/Num/Scroll) 时调用。
//
// 【为什么留空】
//   透传 LED 需要一条反向通道(左板 -> 右板 -> 真键盘的 SET_REPORT),
//   实测不稳定且用户明确表示不需要, 因此移除。
//   功能不受影响: Caps Lock 打字照样是大写, 只是真键盘的指示灯不跟着变。
// ---------------------------------------------------------------------------
extern "C" void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                                      hid_report_type_t report_type,
                                      uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
    // 有意留空
}

// ---------------------------------------------------------------------------
// 发送: 把真键盘的原始报文发到键盘 Collection
//
// payload/len = 右板转来的原始字节。
//
// 两种情况:
//   A) len == 8 (标准 boot keyboard: modifier + reserved + key0..key5)
//      -> 前面补 Report ID 1, 发出 9 字节。键码原样不动。
//   B) len == 9 且 payload[0] == RID_KEYBOARD (真键盘本身就带 Report ID)
//      -> 原样发出。
//
// 其余长度按"原始报文直接补 RID"处理, 保证不丢字节。
// ---------------------------------------------------------------------------
bool kbdUsbSendReport(const uint8_t *payload, uint8_t len)
{
    if (!payload || len == 0) return false;
    if (!tud_ready() || !tud_hid_n_ready(0)) return false;

    uint8_t buf[KBD_MAX_REPORT + 1];

    if (len == 9 && payload[0] == RID_KEYBOARD) {
        // 情况 B: 已带 Report ID, 原样发出
        memcpy(buf, payload, 9);
        return tud_hid_n_report(0, 0, buf, 9);
    }

    // 情况 A(及其它): 补 Report ID 1, 原始字节原样跟在后面
    if (len > KBD_MAX_REPORT) len = KBD_MAX_REPORT;
    buf[0] = RID_KEYBOARD;
    memcpy(&buf[1], payload, len);
    return tud_hid_n_report(0, 0, buf, (uint16_t)(len + 1));
}

// ---------------------------------------------------------------------------
// 发送鼠标报文 (Report ID 2, 鼠标 Collection)
//
// 布局 (5 字节, 与描述符里的鼠标 Collection 对应):
//   byte0 = 按键位图 (bit0 L, bit1 R, bit2 M, bit3 S1, bit4 S2)
//   byte1 = X  (int8, 相对)
//   byte2 = Y  (int8, 相对)
//   byte3 = 滚轮 (int8)
//
// 【为什么把 int16 夹到 int8】
//   描述符里声明的是 Logical Min/Max = -127..127 (单字节)。
//   发送超出范围的值会被主机的 HID 解析器截断成不可预测的结果。
//   所以这里【饱和夹取】, 而不是让它溢出回绕:
//     实际值 300 -> 发 127 (而不是回绕成 44)
//   丢掉的位移由调用方下一周期继续补发(位移是累加的)。
// ---------------------------------------------------------------------------
static inline int8_t satI8(int16_t v)
{
    if (v >  127) return  127;
    if (v < -127) return -127;
    return (int8_t)v;
}

bool kbdUsbSendMouse(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel)
{
    if (!tud_ready() || !tud_hid_n_ready(0)) return false;

    uint8_t buf[5];
    buf[0] = buttons & 0x1F;
    buf[1] = (uint8_t)satI8(dx);
    buf[2] = (uint8_t)satI8(dy);
    buf[3] = (uint8_t)wheel;

    // 无 Report ID 项在描述符里? 有 —— 鼠标 Collection 带 RID_MOUSE。
    // 所以这里只发数据, Report ID 由 tud_hid_n_report 的参数给出。
    uint8_t out[6];
    out[0] = RID_MOUSE;
    memcpy(out + 1, buf, 5);
    return tud_hid_n_report(0, 0, out, 6);
}

// ---------------------------------------------------------------------------
// 就绪判定
// ---------------------------------------------------------------------------
bool kbdUsbReady(void)
{
    return tud_ready() && tud_hid_n_ready(0);
}
