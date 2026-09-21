#include "USBSetup.h"
#include "ClonedHID.h"
#include "usb_desc.h"   // 直写 TinyUSB 描述符层
#include "tusb.h"

extern DeviceInfo device_info;
extern DescriptorDevice descriptor_device;
extern usb_endpoint_descriptor_t endpoint_descriptors[MAX_ENDPOINT_DESCRIPTORS];
extern uint8_t endpointCounter;
extern usb_interface_descriptor_t interface_descriptors[MAX_INTERFACE_DESCRIPTORS];
extern uint8_t interfaceCounter;
extern usb_hid_descriptor_t hid_descriptors[MAX_HID_DESCRIPTORS];
extern uint8_t hidDescriptorCounter;
extern usb_iad_desc_t descriptor_interface_association;
extern endpoint_data_t endpoint_data_list[17];
extern usb_unknown_descriptor_t unknown_descriptors[MAX_UNKNOWN_DESCRIPTORS];
extern uint8_t unknownDescriptorCounter;
extern DescriptorConfiguration configuration_descriptor;

volatile bool deviceConnected = false;
static bool s_usb_initialized = false;
static unsigned long s_last_ready_poll = 0;

// ★ 不再使用 Arduino 的 USBHIDMouse / USBHIDKeyboard:
//   那套封装只能产生一个 HID 接口, 且接口协议固定为 NONE。
//   现由 usb_desc.cpp / usb_hid.cpp 直写 TinyUSB 描述符, 本板恒定鼠标角色。

// ★ tud_descriptor_string_cb 已移入 usb_desc.cpp
//   (那里的版本同样从 device_info 取真设备的字符串)

void requestUSBDescriptors() {
    unsigned long now = millis();
    if (now - s_last_ready_poll < 200) {
        return;
    }
    s_last_ready_poll = now;

    // 若尚未收到物理鼠标描述符，周期性向 Host 请求握手
    if (!deviceConnected) {
        Serial1.println("READY");
    }

    // ===== 握手看门狗 (关键健壮性修复) =====
    //
    // 【为什么必须有】
    //   握手是一条"发一条命令 -> 等一条应答 -> 再发下一条"的串行链,
    //   而 InitUSB() 只在【整条队列发完】时才会被调用。
    //   中间任何一条应答丢失 —— 设备不支持该命令 / JSON 解析失败 /
    //   响应超长被 8192 缓冲截断 —— 整条链就【永久卡住】:
    //       InitUSB() 永不执行 -> tinyusb_init() 永不调用
    //       -> 被控机看不到【任何】USB 设备, 而且不报任何错。
    //   现象就是"板子灯亮、右板也在收数据, 但被控机里什么都没有"。
    //   真游戏鼠标的描述符(多接口 + 多 collection)比这更长,
    //   正是最容易触发这条路径的设备。
    //
    // 【本看门狗做什么】
    //   监视 currentCommandIndex 是否还在推进。3 秒无进展就用【当前已知的
    //   信息】强制完成握手并拉起 USB。宁可身份克隆不全, 也绝不能让整块板
    //   连 USB 设备都不出现。
    if (!s_usb_initialized) {
        extern int  currentCommandIndex;
        static uint32_t s_lastProgress = 0;
        static int      s_lastIndex    = -1;

        if (currentCommandIndex != s_lastIndex) {
            s_lastIndex    = currentCommandIndex;
            s_lastProgress = now;
        } else if (now - s_lastProgress > 3000) {
            Serial0.printf("[USB] 握手超时(停在命令 %d) -> 用当前信息强制初始化\n",
                           currentCommandIndex);
            s_lastProgress = now;          // 避免反复刷屏
            InitUSB();
        }
    }
}

void InitUSB() {
    if (s_usb_initialized) {
        return;
    }

    // ★ VID/PID/类/字符串 不再通过 Arduino 的 USB.xxx() 设置。
    //   它们由 usb_desc.cpp 直接从 descriptor_device / device_info 读取并填进
    //   我们自己的描述符, 效果与原来一致(身份克隆得以保留)。

    // ===== 描述符判据: 只用来判断"这一路到底有没有真实鼠标" =====
    //
    // 【历史】这里以前按真设备的主功能类型在【键盘角色】与【鼠标角色】之间
    // 二选一(方案乙)。左板现在只做鼠标, 角色选择已删除: 恒定枚举一个
    // Boot Mouse 接口。键盘由独立的 KBD_PASSTHROUGH 固件负责 ——
    // 右板(fw_host)仍会把真键盘接口的标记一并送上来, 但本板不再据此改变
    // 自己的 USB 形状, 也不再消费真实键盘报文。
    //
    // 【为什么保留 realMouse / primaryProto 的判断】只为日志:
    //   若真设备侧一个鼠标标记都没有(例如插的是纯键盘, 或描述符请求失败),
    //   打印出来便于现场判断"鼠标为什么不动"。
    //
    // 【判据来源】
    // 这些标记由右板解析【真设备的 HID 报告描述符内容】得出, 随接口列表 JSON
    // 一起传来(见 fw_host/serialization.cpp 与 InitSettings.cpp)。
    //
    // 【为什么主判据不是 bInterfaceProtocol】实测: 键盘接收器(VID_3554)与鼠标
    // 接收器(VID_249A)报的都是 Class_03 SubClass_00 Prot_00 —— 完全一样,
    // 无法区分。唯一可靠的依据是报告描述符里的 Usage:
    //     Usage Page 0x01 + Usage 0x02 -> 鼠标
    //     Usage Page 0x01 + Usage 0x06 -> 键盘
    bool realMouse = false;
    bool realKbd   = false;
    for (int i = 0; i < interfaceCounter && i < MAX_IFACE_TYPE; i++) {
        if (iface_isMouse[i]) realMouse = true;
        if (iface_isKbd[i])   realKbd   = true;
    }

    // 兜底判据: 报告描述符标记一个都没有时, 退回看接口 protocol。
    // (例如描述符请求失败、或设备不是标准 HID 的情况)
    int8_t primaryProto = -1;
    if (!realMouse && !realKbd) {
        for (int i = 0; i < interfaceCounter; i++) {
            if (interface_descriptors[i].bInterfaceClass != 0x03) continue;
            primaryProto = (int8_t)interface_descriptors[i].bInterfaceProtocol;
            break;
        }
    }

    Serial0.printf("[USB] role=MOUSE (realMouse=%d realKbd=%d proto=%d ifaces=%u)\n",
                   (int)realMouse, (int)realKbd,
                   (int)primaryProto, (unsigned)interfaceCounter);

    // ===== 启动直写 TinyUSB 描述符层 =====
    //
    // 恒定鼠标角色: 只有鼠标接口 (Boot Mouse, 无 Report ID, 4 字节报文)。
    //
    // 这取代了原来经 Arduino USBHID 的那套 begin() 与注册流程。
    // Arduino 的 USB 封装不再参与枚举 —— 我们对 tud_descriptor_* 与
    // tud_hid_descriptor_report_cb 给出的强定义会直接生效。
    kbdUsbBegin();

    // 克隆实例的注册保留(其内容由 ClonedHID.cpp 维护, 不依赖 Arduino USB)
    initClonedDevices();

    s_usb_initialized = true;
}

bool isUsbReadyToTransfer() {
    if (!s_usb_initialized) return false;
    // 使用 TinyUSB 的原生就绪判断 (挂载且未挂起，且HID端点准备好)。
    // 本设备只有一个 HID 实例(实例 0) —— 唯一的鼠标接口, 无 Report ID。
    return tud_ready() && tud_hid_n_ready(0);
}

// ============================================================================
// 真设备描述符的接收(诊断 + 报文格式记录)
//
// 【重要设计说明】为什么这里只记录、不改被控机看到的描述符:
//
// 被控机读到的 HID 报告描述符是【本固件内置的】鼠标描述符(见 usb_desc.cpp),
// 真设备的报告描述符【不会】被装上去。原因:
//   1. 一个 HID 接口只能有一个 Collection 布局, 而"真设备描述符"可能描述
//      多接口 / 多 collection 的复合设备, 直接塞进单接口会让布局错位。
//   2. 真游戏鼠标的描述符(100~200 字节)比内置的(52 字节)长, 端点包大小、
//      报文长度、有无 Report ID 都可能不同 —— 报告描述符换了, 发送侧
//      (usb_hid.cpp 的 4 字节报文)必须同步换, 否则主机解析与字节流对不上。
//   3. 报文格式克隆(阶段 4)尚未实现: sendClonedMouseReport() 恒返回 false,
//      调用方一律回落内置鼠标路径。
//
// 所以本固件做到的是【身份克隆】: VID / PID / 厂商 / 产品名 / 序列号 /
// 设备类别全部照抄真设备(见 buildDescriptors 里对 s_deviceDesc 的填充)。
// 被控机看到的是真鼠标的身份信息, 但报文格式仍是内置的标准 4 字节鼠标格式。
//
// 需要明确的是: 身份克隆能过"型号/VID-PID 白名单"类检测, 但报文格式和 USB
// 时序特征仍与真设备不同, 挡不住做深度指纹的检测。
//
// 下面的代码把真设备的报告描述符收下来, 用途是:
//   - 解析出真设备的报文格式(轴位宽/按钮字节/滚轮位置)
//   - 供克隆是否可行的现场诊断(通过 km.cloneinfo 回报)
// ============================================================================

// 真设备鼠标报告描述符(静态区)
static uint8_t  s_mouseDesc[CLONE_DESC_MAX];
static uint16_t s_mouseDescLen = 0;
static bool     s_mouseDescReady = false;

static uint8_t  s_kbdDesc[CLONE_DESC_MAX];
static uint16_t s_kbdDescLen = 0;
static bool     s_kbdDescReady = false;

static ClonedReportLayout s_mouseLayout;

// 真鼠标描述符是否已收到(供软体查询)
bool isCloneMouseActive() { return s_mouseDescReady; }
const ClonedReportLayout *mouseLayout() { return &s_mouseLayout; }

// 从 hex 字符串解析真设备描述符
// (hex 由 fw_host 的 USB_sendRawHidDescriptors:<iface>:<hex> 提供)
void receiveRealHidDescriptor(uint8_t iface, const char *hex)
{
    if (!hex) return;

    const size_t hexLen = strlen(hex);
    const size_t n = hexLen / 2;
    if (n == 0) return;

    uint8_t tmp[CLONE_DESC_MAX];
    const size_t cap = (n < sizeof(tmp)) ? n : sizeof(tmp);

    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };

    for (size_t i = 0; i < cap; ++i) {
        tmp[i] = (uint8_t)((hexVal(hex[2 * i]) << 4) | hexVal(hex[2 * i + 1]));
    }

    ClonedReportLayout L = cloned::parse(tmp, (int)cap);
    if (!L.valid) {
        Serial0.printf("[USB] iface %u descriptor not usable (len=%u)\n",
                       (unsigned)iface, (unsigned)cap);
        return;
    }

    // 真键盘接口的描述符也会被右板送来(它仍然探测真设备类型)。本板不做键盘,
    // 因此直接忽略 —— 但记录在诊断输出里, 便于判断"右板到底插了什么"。
    if (L.isKeyboard) {
        Serial0.printf("[USB] iface %u is a keyboard descriptor (len=%u) "
                       "-> ignored, this board is mouse-only\n",
                       (unsigned)iface, (unsigned)cap);
        return;
    }

    // 只按【实际解析出的字节数】记录: 曾用 cap 会把 tmp 尾部未解析的字节
    // (可能超出描述符真实长度)一并算进去, 报文长度就会比真设备长。
    memcpy(s_mouseDesc, tmp, n);
    s_mouseDescLen = (uint16_t)n;
    s_mouseDescReady = true;
    s_mouseLayout = L;

    Serial0.printf("[USB] real mouse descriptor: iface=%u len=%u reportLen=%u "
                   "x(sz=%u@%u) y(sz=%u@%u) wheel@%u%s\n",
                   (unsigned)iface, (unsigned)n, (unsigned)L.reportLen,
                   (unsigned)L.xBits, (unsigned)L.xByte,
                   (unsigned)L.yBits, (unsigned)L.yByte,
                   (unsigned)L.wheelByte,
                   L.hasReportId ? " (has report id)" : "");
}

// 触发重枚举: 拉低 D+ 让被控机看到"拔出", 再拉高重新枚举。
//
// 用途: 在 VID/PID/字符串等身份信息变化后, 让被控机重新读取设备描述符。
// 副作用: CDC 串口(Serial0/软体链路)会断开约 1 秒 —— USB 设备级重枚举的
// 必然结果(CDC 与 HID 在同一个 USB 设备上)。调用方必须能容忍这次断线。
void cloneReenumerate()
{
    Serial0.println("[USB] re-enumerating...");
    Serial0.flush();
    delay(50);

    tud_disconnect();
    delay(300);
    tud_connect();
    delay(300);

    Serial0.println("[USB] re-enumerate done");
}

void initClonedDevices()
{
    // 克隆实例由 ClonedHID.cpp 维护。此处无需额外动作。
}

// 报文发送: 本固件不替换报文格式(恒用内置 4 字节鼠标报文), 因此本函数
// 返回 false 表示"未启用克隆发送", 调用方应回落到内置路径。
bool sendClonedMouseReport(int, int, int, uint8_t) { return false; }
