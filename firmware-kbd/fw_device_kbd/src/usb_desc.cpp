// ============================================================================
// usb_desc.cpp —— 设备/配置/字符串 描述符 (TinyUSB 原生回调)
//
// 关键设计: bDeviceClass = 0x00
//   这样 Windows 不会把设备当成单一类设备, 而是交给 usbccgp 复合设备驱动,
//   再按每个接口的类去分别加载子驱动 —— 与真键盘接收器完全一致。
// ============================================================================

#include "tusb.h"
#include "kbd_desc.h"

// ---------------------------------------------------------------------------
// 设备描述符
//   bDeviceClass = 0x00 (由接口定义) -> usbccgp
//   bNumConfigurations = 1
//
// ★ 身份是【运行时可变】的:
//   开机后左板会先等右板送来真键盘的 VID/PID, 再用 kbdDescSetIdentity()
//   填进这份描述符, 然后才调 tinyusb_init() 开始枚举。
//   所以这里不能是 const, 也不能写死。
// ---------------------------------------------------------------------------
static tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,      // USB 2.0

    // ★ 0x00 = 每个接口自己声明类 -> Windows 加载 usbccgp 复合设备驱动
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    // 默认值(仅在没等到真键盘身份时使用)
    .idVendor           = 0x3554,
    .idProduct          = 0xFA09,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01,
};

// ---------------------------------------------------------------------------
// 运行时设定身份 (由 usb_init.cpp 在 tinyusb_init() 之前调用)
//
// 必须在枚举开始前调用 —— 枚举后改这里不会生效。
// ---------------------------------------------------------------------------
extern "C" void kbdDescSetIdentity(uint16_t vid, uint16_t pid, uint16_t bcd)
{
    if (vid != 0) desc_device.idVendor  = vid;
    if (pid != 0) desc_device.idProduct = pid;
    if (bcd != 0) desc_device.bcdDevice = bcd;
}

// 配置描述符总长
//   9 (config) + 25 (HID 接口: 9 接口 + 9 HID + 7 端点) = 34
#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + 25)

// ---------------------------------------------------------------------------
// 配置描述符 —— 单接口, 双 Top-Level Collection
//
// 为什么只有一个接口:
//   预编译 TinyUSB 库写死 CFG_TUD_HID=1 (符号 _hidd_itf=140 字节, 步长也=140),
//   第二个 HID 接口会被 hidd_open() 拒绝, 导致整个配置枚举失败。
//   而 Windows 的 kbdhid 是按 Top-Level Collection 加载的, 不是按接口,
//   所以单接口内放【键盘 + 鼠标两个 Collection】同样能被分别识别。
//
// 接口协议 = Boot Keyboard (Class_03/SubClass_01/Prot_01),
// 与真键盘接收器的 MI_00 对齐。
// ---------------------------------------------------------------------------
uint8_t const desc_configuration[] = {
    // config 编号, 接口数, 字符串索引, 总长, 属性, 电流(mA/2)
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // ---------- 唯一接口: HID (键盘 + 鼠标 两个 Collection) ----------
    // 端点包大小 8: 键盘报文带 Report ID 后是 9 字节, 取 8 不够。
    // 见 usb_hid.cpp 的说明 —— 这里用 16 保证两种 Report 都能整包发出。
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 4, HID_ITF_PROTOCOL_KEYBOARD,
                       KBD_REPORT_DESC_LEN,
                       EPNUM_HID_IN, 16, 1),
};

// ---------------------------------------------------------------------------
// 字符串描述符
// ---------------------------------------------------------------------------
static char const *string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 },   // 0: 语言 = 英语(US)
    "Wired Keyboard",               // 1: 厂商
    "USB Keyboard",                 // 2: 产品
    "000000000001",                 // 3: 序列号
    "Keyboard",                     // 4: 接口名称
};

// ---------------------------------------------------------------------------
// TinyUSB 回调: 返回设备描述符
// ---------------------------------------------------------------------------
extern "C" uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

// ---------------------------------------------------------------------------
// TinyUSB 回调: 返回配置描述符
// ---------------------------------------------------------------------------
extern "C" uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

// ---------------------------------------------------------------------------
// TinyUSB 回调: 返回字符串描述符
// ---------------------------------------------------------------------------
extern "C" uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t _desc_str[32];
    uint8_t chr_count;

    if (index == 0) {
        // 语言 ID
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
            return NULL;
        }
        const char *str = string_desc_arr[index];

        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;

        // ASCII -> UTF-16LE
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = (uint16_t)str[i];
        }
    }

    // 首字节: 长度(字节) = 2 + 字符数*2
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
