// ============================================================================
// usb_hid.cpp —— HID 类回调 + 报文发送
//
// 只有一个 HID 接口 (instance 0), 里面只有一个 Collection, 且【无 Report ID】。
// 所以报文就是裸数据:
//   键盘: 8 字节 (modifier + reserved + key0..key5)
//   鼠标: 4 字节 (buttons + x + y + wheel)
//
// 这取代了 MAKCUNEW 原来经 Arduino USBHIDMouse/USBHIDKeyboard 的发送路径。
// ============================================================================

#include <Arduino.h>
#include <string.h>
#include "tusb.h"
#include "class/hid/hid_device.h"
#include "usb_desc.h"

// 取当前角色的报告描述符 (实现在 usb_desc.cpp)
extern "C" const uint8_t *kbdDescCurrentReport(uint16_t *lenOut);

// ---------------------------------------------------------------------------
// 报告描述符回调
//   ★ 这正是 Arduino USBHID 做不到的地方 —— 它忽略 instance 参数,
//     无条件返回同一份运行时拼接缓冲。我们直接返回本接口真正的描述符。
// ---------------------------------------------------------------------------
extern "C" uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return kbdDescCurrentReport(nullptr);
}

// ---------------------------------------------------------------------------
// GET_REPORT: 本设备是被动上报, 返回 0
// ---------------------------------------------------------------------------
extern "C" uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                          hid_report_type_t report_type,
                                          uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

// ---------------------------------------------------------------------------
// SET_REPORT: 被控机下发键盘 LED 状态。透传场景下不处理。
// ---------------------------------------------------------------------------
extern "C" void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                                      hid_report_type_t report_type,
                                      uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

// ---------------------------------------------------------------------------
// 等待端点空闲
//
// 【为什么需要】
//   Arduino 的 USBHID::SendReport() 在调完 tud_hid_n_report() 之后会【等信号量】,
//   确认报文真正发完才返回。所以我最初"调一次就返回"的实现丢掉了这层节流语义:
//   端点忙时直接失败 -> 位移被静默吞掉(而调用方还按已发出记账)。
//
//   这里补回等价语义: 发送前等端点空闲, 最多等 KBD_TX_WAIT_MS 毫秒。
//   超时才返回 false, 由调用方把位移还回队列重试。
//
// 注意: 只能在任务上下文调用(内部有 delay)。MAKCUNEW 的发送路径都在任务里,
//       没人在中断里调本函数。
// ---------------------------------------------------------------------------
// 等端点空闲的最长时间。
//   USB 端点的轮询周期是 1ms, 所以等一个周期就够; 原来设 5ms 会让每次
//   抢不到端点的调用白白阻塞 5ms, 把后续按键报文越推越后(卡顿来源之一)。
#define KBD_TX_WAIT_MS   1

static inline bool waitEndpointReady(void)
{
    if (tud_ready() && tud_hid_n_ready(0)) return true;

    const uint32_t t0 = millis();
    while (millis() - t0 < KBD_TX_WAIT_MS) {
        if (tud_ready() && tud_hid_n_ready(0)) return true;
        delay(1);
    }
    return false;
}

// ---------------------------------------------------------------------------
// 就绪判定
// ---------------------------------------------------------------------------
bool kbdUsbReady(void)
{
    return tud_ready() && tud_hid_n_ready(0);
}

// ---------------------------------------------------------------------------
// 发送键盘报文
//
// report8 = modifier + reserved + 6 键码 (标准 boot keyboard 布局)
// 无 Report ID -> report_id 传 0, 数据整包发出。
// ---------------------------------------------------------------------------
bool kbdUsbSendKeyboard(const uint8_t *report8)
{
    if (!report8) return false;

    // ★ 角色互锁(与 kbdUsbSendMouse 对称)。
    //
    //   左板只有一个 HID 接口, 它的报文格式由角色决定: 键盘角色 = 8 字节,
    //   鼠标角色 = 4 字节。若鼠标单板上有人发 8 字节键盘报文, 主机会把它按
    //   4 字节鼠标解析 —— 读到错位数据, 表现为鼠标乱动/乱按键, 而且不报错。
    //   这里硬性拒绝, 让两种报文在构造上就不可能交叉。
    if (!kbdUsbIsKeyboardRole()) return false;

    if (!waitEndpointReady()) return false;
    return tud_hid_n_report(0, 0, report8, 8);
}

// ---------------------------------------------------------------------------
// 发送鼠标报文
//
// 【为什么把 int16 夹到 int8】
//   报告描述符里声明的是 Logical Min/Max = -127..127 (单字节)。
//   超出范围会被主机 HID 解析器截断成不可预测的结果, 所以这里饱和夹取。
//
// 【注意】调用方负责把夹掉的部分留到下一次发送 —— 本函数不做补偿,
//         以免在不同调用点产生不一致的行为。
// ---------------------------------------------------------------------------
static inline int8_t satI8(int16_t v)
{
    if (v >  127) return  127;
    if (v < -127) return -127;
    return (int8_t)v;
}

bool kbdUsbSendMouse(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel)
{
    // ★ 角色互锁(与 kbdUsbSendKeyboard 对称)。
    //
    //   键盘角色的左板上报的是 8 字节 boot 键盘描述符, 没有鼠标接口。此时若把
    //   4 字节鼠标报文发出去, 主机会把它当成键盘报文解析 —— 4 个字节恰好落进
    //   "修饰键 + 保留 + 键码" 里, 于是【每一次鼠标移动都会在被控机上随机按键】,
    //   甚至把某个键按住不放。这正是"键卡住"最容易出现的另一条来路。
    //
    //   什么时候会触发: 右板把键盘接收器某个非键盘 HID 接口(protocol=NONE,
    //   报文 >=3 字节)误判成鼠标兜底接口时。这里拒绝, 保证键盘单板干净。
    if (kbdUsbIsKeyboardRole()) return false;

    if (!waitEndpointReady()) return false;

    uint8_t rep[4];
    rep[0] = buttons & 0x1F;
    rep[1] = (uint8_t)satI8(dx);
    rep[2] = (uint8_t)satI8(dy);
    rep[3] = (uint8_t)wheel;
    return tud_hid_n_report(0, 0, rep, 4);
}

// 供调用方夹取后再自行补偿余量时使用
int8_t kbdClampAxis(int16_t v) { return satI8(v); }
