// ============================================================================
// usb_hid.cpp —— HID 类回调 + 报文发送
//
// 只有一个 HID 接口 (instance 0), 里面只有一个 Collection, 且【无 Report ID】。
// 所以报文就是裸数据:
//   鼠标: 4 字节 (buttons + x + y + wheel)
//
// 本板只做鼠标 (键盘已删除, 由独立的 KBD_PASSTHROUGH 固件负责)。
// 这取代了 MAKCUNEW 原来经 Arduino USBHIDMouse/USBHIDKeyboard 的发送路径。
// ============================================================================

#include <Arduino.h>
#include <string.h>
#include "tusb.h"
#include "class/hid/hid_device.h"
#include "usb_desc.h"
#include "USBSetup.h"     // isCloneMouseActive / mouseLayout
#include "ClonedHID.h"    // cloned::buildMouseReport

// 取本板的报告描述符 (实现在 usb_desc.cpp, 恒为鼠标)
extern "C" const uint8_t *descCurrentReport(uint16_t *lenOut);

// ---------------------------------------------------------------------------
// 报告描述符回调
//   ★ 这正是 Arduino USBHID 做不到的地方 —— 它忽略 instance 参数,
//     无条件返回同一份运行时拼接缓冲。我们直接返回本接口真正的描述符。
// ---------------------------------------------------------------------------
extern "C" uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return descCurrentReport(nullptr);
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
// SET_REPORT: 被控机下发的报文。透传场景下不处理。
// (设备只枚举鼠标接口, 因此这里不会收到键盘 LED 输出报文)
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
//   这里补回等价语义: 发送前等端点空闲, 最多等 HID_TX_WAIT_MS 毫秒。
//   超时才返回 false, 由调用方把位移还回队列重试。
//
// 注意: 只能在任务上下文调用(内部有 delay)。MAKCUNEW 的发送路径都在任务里,
//       没人在中断里调本函数。
// ---------------------------------------------------------------------------
// 等端点空闲的最长时间。
//   USB 端点的轮询周期是 1ms, 所以等一个周期就够; 原来设 5ms 会让每次
//   抢不到端点的调用白白阻塞 5ms, 把后续按键报文越推越后(卡顿来源之一)。
#define HID_TX_WAIT_MS   1

static inline bool waitEndpointReady(void)
{
    if (tud_ready() && tud_hid_n_ready(0)) return true;

    const uint32_t t0 = millis();
    while (millis() - t0 < HID_TX_WAIT_MS) {
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
    if (!waitEndpointReady()) return false;

    // 若真设备的报告描述符已就绪, 按真描述符解析出的 layout 组装报文 ——
    // 主机看到的描述符与字节流格式一致, 厂商专属驱动才有可能识别成对应产品。
    // 兼容性兜底: layout 无效或未就绪时用内置 4 字节 Boot Mouse 报文。
    if (isCloneMouseActive()) {
        const ClonedReportLayout *L = mouseLayout();
        if (L && L->valid && L->reportLen > 0 && L->reportLen <= 64) {
            uint8_t rep[64];
            cloned::buildMouseReport(*L, rep, L->reportLen,
                                     (int)satI8(dx), (int)satI8(dy),
                                     (int)wheel, buttons);
            const uint8_t id = L->hasReportId ? L->reportId : 0;
            return tud_hid_n_report(0, id, rep, L->reportLen);
        }
    }

    uint8_t rep[4];
    rep[0] = buttons & 0x1F;
    rep[1] = (uint8_t)satI8(dx);
    rep[2] = (uint8_t)satI8(dy);
    rep[3] = (uint8_t)wheel;
    return tud_hid_n_report(0, 0, rep, 4);
}
