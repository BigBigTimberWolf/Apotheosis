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
//   那套封装只能产生一个 HID 接口, 且接口协议固定为 NONE,
//   导致插键盘时被控机不认。现由 usb_desc.cpp / usb_hid.cpp 直写 TinyUSB 描述符。

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
    //   键盘接收器(VID_3554)描述符更复杂(2 接口 + 6 collection),
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

    // ===== 只暴露真设备【实际拥有】的那一类 HID =====
    //
    // 【老代码的问题】这里无条件 begin 了 Mouse 和 Kbd 两个, 于是被控机看到的
    // 永远是一个"鼠标+键盘"的复合设备 —— 无论真设备插的是鼠标还是键盘:
    //     插真鼠标 -> 被控机看到 鼠标 + 键盘   (多出一个不该有的键盘)
    //     插真键盘 -> 被控机看到 鼠标 + 键盘   (多出一个不该有的鼠标)
    // 这既不符合真设备的描述符结构(影响驱动识别/身份一致性), 也让
    // "插什么就克隆什么"这个目标落空。
    //
    // 【本版改法】按真设备接口列表决定 begin 谁:
    //   Class 0x03 (HID) + bInterfaceProtocol:
    //       0x02 -> 鼠标(Boot Mouse)
    //       0x01 -> 键盘(Boot Keyboard)
    //   只看【第一个】HID 接口的 protocol 作为主功能判据 —— 不能扫"有没有",
    //   因为键盘接收器这类设备内部常附带一个鼠标接口(键鼠一体), 扫"有没有"
    //   会导致插键盘也暴露出鼠标。
    //
    // 【时机】InitUSB() 在握手最后(10 条命令全部完成后)才被调用, 此时
    // interface_descriptors[] 与真设备报告描述符都已经到齐, 因此判断有效。
    //
    // 【兜底】识别不出来时按老行为两个都 begin —— 宁可多暴露, 也不要整个
    // 设备失效。这样任何未知设备至少保持原有可用性。

    // ===== 判据来源 =====
    //
    // 这些标记由右板解析【真设备的 HID 报告描述符内容】得出, 随接口列表 JSON
    // 一起传来(见 fw_host/serialization.cpp 与 InitSettings.cpp)。
    //
    // 【为什么不能用 bInterfaceProtocol】实测: 键盘接收器(VID_3554)与鼠标接收器
    // (VID_249A)报的都是 Class_03 SubClass_00 Prot_00 —— 完全一样, 无法区分。
    // 唯一可靠的依据是报告描述符里的 Usage:
    //     Usage Page 0x01 + Usage 0x02 -> 鼠标
    //     Usage Page 0x01 + Usage 0x06 -> 键盘
    bool realKbd   = false;
    bool realMouse = false;
    for (int i = 0; i < interfaceCounter && i < MAX_IFACE_TYPE; i++) {
        if (iface_isKbd[i])   realKbd   = true;
        if (iface_isMouse[i]) realMouse = true;
    }

    // 兜底判据: 报告描述符标记一个都没有时, 退回看接口 protocol。
    // (例如描述符请求失败、或设备不是标准 HID 的情况)
    int8_t primaryProto = -1;
    if (!realKbd && !realMouse) {
        for (int i = 0; i < interfaceCounter; i++) {
            if (interface_descriptors[i].bInterfaceClass != 0x03) continue;
            primaryProto = (int8_t)interface_descriptors[i].bInterfaceProtocol;
            break;
        }
    }

    bool wantMouse = false;
    bool wantKbd   = false;

    if (realKbd) {
        // 键盘优先 —— 键盘接收器常同时带一个鼠标接口(键鼠一体), 而本口接的是
        // 键盘, 因此只要检测到键盘就【只暴露键盘】, 不去看那个附带的鼠标接口。
        wantKbd = true;
    } else if (realMouse) {
        wantMouse = true;
    } else if (primaryProto == 0x01) {
        wantKbd = true;
    } else if (primaryProto == 0x02) {
        wantMouse = true;
    } else {
        // 什么都认不出来 -> 保持老行为(两个都暴露), 宁可多暴露也不让设备失效。
        wantMouse = true;
        wantKbd   = true;
    }

    Serial0.printf("[USB] exposing: mouse=%d kbd=%d (realMouse=%d realKbd=%d proto=%d ifaces=%u)\n",
                   (int)wantMouse, (int)wantKbd, (int)realMouse, (int)realKbd,
                   (int)primaryProto, (unsigned)interfaceCounter);

    // ===== 启动直写 TinyUSB 描述符层 =====
    //
    // 按角色只暴露一种设备 (方案乙):
    //   键盘马克 -> 只有键盘接口 (Boot Keyboard, 无 Report ID)
    //   鼠标马克 -> 只有鼠标接口 (Boot Mouse,    无 Report ID)
    //
    // 这取代了原来经 Arduino USBHID 的那套 begin() 与注册流程。
    // Arduino 的 USB 封装不再参与枚举 —— 我们对 tud_descriptor_* 与
    // tud_hid_descriptor_report_cb 给出的强定义会直接生效。
    const int role = wantKbd ? USB_ROLE_KEYBOARD : USB_ROLE_MOUSE;
    kbdUsbBegin(role);

    // 克隆实例的注册保留(其内容由 ClonedHID.cpp 维护, 不依赖 Arduino USB)
    initClonedDevices();

    s_usb_initialized = true;
}

bool isUsbReadyToTransfer() {
    if (!s_usb_initialized) return false;
    // 使用 TinyUSB 的原生就绪判断 (挂载且未挂起，且HID端点准备好)。
    // 本设备只有一个 HID 实例(实例 0), 鼠标与键盘在该实例内由 Report ID 区分,
    // 所以 tud_hid_n_ready(0) 同时代表两条通路就绪。
    return tud_ready() && tud_hid_n_ready(0);
}

// ============================================================================
// 真设备描述符的接收(诊断 + 报文格式记录)
//
// 【重要设计说明】为什么这里只记录、不改被控机看到的描述符:
//
// 被控机读到的 HID 报告描述符由 Arduino 框架决定, 本固件无法替换:
//   1. 整个 USB 设备只有【一个】HID 接口。注册多个 USBHIDDevice 不会多出
//      接口, 只会把各自的描述符【首尾拼接】成一大块整体上报。
//   2. 拼接缓冲的大小在 USB.begin() 时按"各设备注册长度之和"一次 malloc,
//      运行时不能改。而真游戏鼠标的描述符(100~200 字节)比内建的(52 字节)长。
//   3. 覆盖 tud_hid_descriptor_report_cb / tusb_hid_load_descriptor 都不可行:
//      实测链接报 multiple definition —— 框架源文件是参与编译的, 不是可按需
//      拉取的静态库成员。
//   4. 继承 USBHIDMouse 重写 _onGetDescriptor 可行(是虚函数), 但【注册长度】
//      写死在基类构造函数里(sizeof(compile-time 常量)), 改不了; 重复调用
//      addDevice 会让长度累加两次, 描述符布局整体错位。
//
// 所以本固件做到的是【身份克隆】: VID / PID / 厂商 / 产品名 / 序列号 /
// 设备类别全部照抄真设备(见 InitUSB 里 USB.* 的赋值)。被控机看到的是真鼠标的
// 身份信息, 但报文格式仍是标准 Arduino 鼠标格式。
//
// 需要明确的是: 身份克隆能过"型号/VID-PID 白名单"类检测, 但报文格式和 USB
// 时序特征仍与真设备不同, 挡不住做深度指纹的检测。
//
// 下面的代码把真设备的报告描述符收下来, 用途是:
//   - 解析出真设备的报文格式(轴位宽/按钮字节/滚轮位置)
//   - 供克隆是否可行的现场诊断(通过 km.cloneinfo 回报)
// ============================================================================

// 真设备报告描述符(静态区)
static uint8_t  s_mouseDesc[CLONE_DESC_MAX];
static uint16_t s_mouseDescLen = 0;
static bool     s_mouseDescReady = false;

static uint8_t  s_kbdDesc[CLONE_DESC_MAX];
static uint16_t s_kbdDescLen = 0;
static bool     s_kbdDescReady = false;

static ClonedReportLayout s_mouseLayout;
static ClonedReportLayout s_kbdLayout;

// 真描述符是否已收到(供软体查询)
bool isCloneMouseActive() { return s_mouseDescReady; }
bool isCloneKbdActive()   { return s_kbdDescReady; }
const ClonedReportLayout *mouseLayout() { return &s_mouseLayout; }
const ClonedReportLayout *kbdLayout()   { return &s_kbdLayout; }

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

    if (L.isKeyboard) {
        memcpy(s_kbdDesc, tmp, cap);
        s_kbdDescLen = (uint16_t)cap;
        s_kbdDescReady = true;
        s_kbdLayout = L;
    } else {
        memcpy(s_mouseDesc, tmp, cap);
        s_mouseDescLen = (uint16_t)cap;
        s_mouseDescReady = true;
        s_mouseLayout = L;
    }

    Serial0.printf("[USB] real %s descriptor: iface=%u len=%u reportLen=%u "
                   "x(sz=%u@%u) y(sz=%u@%u) wheel@%u%s\n",
                   L.isKeyboard ? "keyboard" : "mouse",
                   (unsigned)iface, (unsigned)cap, (unsigned)L.reportLen,
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
    // 内部设备在 InitUSB() 里已自行注册。此处无需额外动作。
}

// 报文发送: 本固件不替换报文格式, 统一走内置 Mouse/Kbd, 因此这两个函数
// 返回 false 表示"未启用克隆发送", 调用方应回落到内置路径。
bool sendClonedMouseReport(int, int, int, uint8_t) { return false; }
bool sendClonedKbdReport(uint8_t, const uint8_t *) { return false; }
