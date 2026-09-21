// ============================================================================
// usb_desc.cpp —— 设备 / 配置 / 字符串 描述符 (TinyUSB 强定义)
//
// 覆盖 Arduino 核心里的弱定义 (esp32-hal-tinyusb.c:270/279/288),
// 从而完全接管 USB 描述符, 不再经过 Arduino USBHID 的拼接逻辑。
//
// 【身份来源】
//   VID/PID/类/字符串 全部取自 MAKCUNEW 现有的动态数据:
//     descriptor_device  <- 右板解析真设备描述符后传来
//     device_info        <- 真设备的字符串描述符
//   这样"插什么就克隆什么"的行为得以保留。
// ============================================================================

#include <Arduino.h>
#include <string.h>
#include "tusb.h"
#include "esp32-hal-tinyusb.h"
#include "usb_desc.h"
#include "InitSettings.h"   // DeviceInfo / DescriptorDevice

// ---- MAKCUNEW 现有的动态身份数据 (定义在 InitSettings.cpp) ----
extern DeviceInfo device_info;
extern DescriptorDevice descriptor_device;

// ---------------------------------------------------------------------------
// 报告描述符: 键盘 (无 Report ID)
//
// 与真 boot keyboard 完全一致:
//   8 位修饰键 + 8 位保留 + 6×8 位键码 = 8 字节输入
//   5 位 LED + 3 位填充              = 1 字节输出
// ---------------------------------------------------------------------------
static const uint8_t s_descKeyboard[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (0xE0 LeftCtrl)
    0x29, 0xE7,        //   Usage Maximum (0xE7 Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)      -> 修饰键
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const)             -> 保留字节
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LED)
    0x19, 0x01,        //   Usage Minimum (1)
    0x29, 0x05,        //   Usage Maximum (5)
    0x91, 0x02,        //   Output (Data,Var,Abs)     -> LED
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Const)
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data,Ary,Abs)      -> 键码数组
    0xC0               // End Collection
};

// ---------------------------------------------------------------------------
// 报告描述符: 鼠标 (无 Report ID)
// ---------------------------------------------------------------------------
static const uint8_t s_descMouse[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x05,        //     Usage Maximum (5)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x05,        //     Report Count (5)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)    -> 按键
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x03,        //     Report Size (3)
    0x81, 0x01,        //     Input (Const)           -> 填充
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x06,        //     Input (Data,Var,Rel)    -> X/Y/滚轮
    0xC0,              //   End Collection
    0xC0               // End Collection
};

// 编译期长度校验: 若与头文件声明不符, 直接编译失败而不是悄悄出错
static_assert(sizeof(s_descKeyboard) == DESC_KBD_LEN,
              "keyboard report descriptor length mismatch");
static_assert(sizeof(s_descMouse) == DESC_MOUSE_LEN,
              "mouse report descriptor length mismatch");

// ---------------------------------------------------------------------------
// 运行时角色 / 身份
// ---------------------------------------------------------------------------
static uint8_t s_role = USB_ROLE_KEYBOARD;

static tusb_desc_device_t s_deviceDesc;

// 配置描述符 (9 config + 25 接口)
#define CONFIG_TOTAL_LEN   (TUD_CONFIG_DESC_LEN + 25)

static uint8_t s_configDesc[CONFIG_TOTAL_LEN];

// ---------------------------------------------------------------------------
// 填充设备 + 配置描述符 (在枚举前调用一次)
// ---------------------------------------------------------------------------
static void buildDescriptors(void)
{
    memset(&s_deviceDesc, 0, sizeof(s_deviceDesc));
    s_deviceDesc.bLength         = sizeof(tusb_desc_device_t);
    s_deviceDesc.bDescriptorType = TUSB_DESC_DEVICE;
    s_deviceDesc.bcdUSB          = descriptor_device.bcdUSB ? descriptor_device.bcdUSB : 0x0200;

    // ★ bDeviceClass 用真设备的值。
    //   真鼠标/真键盘通常是 0x00 (由接口定义), Windows 会据此加载 usbccgp。
    s_deviceDesc.bDeviceClass    = descriptor_device.bDeviceClass;
    s_deviceDesc.bDeviceSubClass = descriptor_device.bDeviceSubClass;
    s_deviceDesc.bDeviceProtocol = descriptor_device.bDeviceProtocol;
    s_deviceDesc.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE;

    s_deviceDesc.idVendor  = descriptor_device.idVendor;
    s_deviceDesc.idProduct = descriptor_device.idProduct;
    s_deviceDesc.bcdDevice = descriptor_device.bcdDevice ? descriptor_device.bcdDevice : 0x0100;

    s_deviceDesc.iManufacturer      = 0x01;
    s_deviceDesc.iProduct           = 0x02;
    s_deviceDesc.iSerialNumber      = 0x03;
    s_deviceDesc.bNumConfigurations = 0x01;

    // ---- 配置描述符: 按角色只放一个接口 ----
    const bool isKbd = (s_role == USB_ROLE_KEYBOARD);

    uint8_t cfg[TUD_CONFIG_DESC_LEN + 25] = {
        TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN,
                              TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 250),

        // 接口协议: 键盘 = Boot Keyboard (SubClass 01 / Prot 01)
        //           鼠标 = Boot Mouse    (SubClass 01 / Prot 02)
        // 端点包大小: 键盘 8 字节 (无 Report ID), 鼠标 4 字节
        TUD_HID_DESCRIPTOR(0, 4,
                           isKbd ? HID_ITF_PROTOCOL_KEYBOARD : HID_ITF_PROTOCOL_MOUSE,
                           isKbd ? DESC_KBD_LEN : DESC_MOUSE_LEN,
                           EPNUM_HID_IN,
                           isKbd ? 8 : 4,
                           1),
    };
    memcpy(s_configDesc, cfg, sizeof(cfg));
}

// ---------------------------------------------------------------------------
// TinyUSB 回调: 设备描述符
// ---------------------------------------------------------------------------
extern "C" uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&s_deviceDesc;
}

// ---------------------------------------------------------------------------
// TinyUSB 回调: 配置描述符
// ---------------------------------------------------------------------------
extern "C" uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_configDesc;
}

// ---------------------------------------------------------------------------
// TinyUSB 回调: 字符串描述符
//   沿用 MAKCUNEW 原有逻辑 —— 从 device_info 取真设备的字符串
// ---------------------------------------------------------------------------
extern "C" uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t desc[64];
    const char *str = nullptr;
    uint8_t chr_count = 0;

    if (index == 0) {
        desc[1] = 0x0409;                       // 英语(US)
        desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 + 2));
        return desc;
    }

    switch (index) {
        case 1: str = device_info.str_desc_manufacturer; break;
        case 2: str = device_info.str_desc_product;      break;
        case 3: str = device_info.str_desc_serial_num;   break;
        case 4: str = device_info.str_desc_product[0] ? device_info.str_desc_product
                                                      : "HID Interface"; break;
        default: return NULL;
    }

    if (!str || !str[0]) {
        // 字符串缺失时返回一个占位串, 避免主机因空索引而反复重试
        str = "N/A";
    }

    chr_count = (uint8_t)strlen(str);
    if (chr_count > 62) chr_count = 62;
    for (uint8_t i = 0; i < chr_count; ++i) {
        desc[1 + i] = (uint8_t)str[i];
    }
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc;
}

// ---------------------------------------------------------------------------
// 供 usb_hid.cpp 使用: 取当前角色的报告描述符
// ---------------------------------------------------------------------------
extern "C" const uint8_t *kbdDescCurrentReport(uint16_t *lenOut)
{
    if (s_role == USB_ROLE_MOUSE) {
        if (lenOut) *lenOut = DESC_MOUSE_LEN;
        return s_descMouse;
    }
    if (lenOut) *lenOut = DESC_KBD_LEN;
    return s_descKeyboard;
}

// ---------------------------------------------------------------------------
// 初始化: 填描述符 + 启动 TinyUSB
// (tinyusb_init 的声明来自 esp32-hal-tinyusb.h)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 角色查询
//
// 左板的 HID 接口是二选一的: 键盘角色 = 8 字节 boot 键盘, 鼠标角色 = 4 字节鼠标。
// 发送路径靠它互锁 —— 鼠标单板上绝不能把 8 字节键盘报文写进 4 字节鼠标接口。
// ---------------------------------------------------------------------------
bool kbdUsbIsKeyboardRole(void)
{
    return s_role == USB_ROLE_KEYBOARD;
}

void kbdUsbBegin(int role)
{
    s_role = (role == USB_ROLE_MOUSE) ? USB_ROLE_MOUSE : USB_ROLE_KEYBOARD;
    buildDescriptors();

    // 传入的 vid/pid 为 0 -> 不覆盖 (设备描述符由我们自己的回调提供)
    tinyusb_device_config_t cfg = {};
    cfg.vid = 0;
    cfg.pid = 0;
    cfg.product_name      = nullptr;
    cfg.manufacturer_name = nullptr;
    cfg.serial_number     = nullptr;
    cfg.webusb_url        = nullptr;
    cfg.usb_class         = 0x00;
    cfg.usb_subclass      = 0x00;
    cfg.usb_protocol      = 0x00;
    cfg.usb_attributes    = TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP;
    cfg.usb_power_ma      = 250;
    cfg.webusb_enabled    = false;

    // ★ 必须检查返回值
    //   之前没检查就打印"已启动", 初始化失败时会误导排查方向。
    const esp_err_t err = tinyusb_init(&cfg);
    if (err != ESP_OK) {
        Serial0.printf("[USB] !! tinyusb_init 失败: %d (0x%X)\n", (int)err, (unsigned)err);
        return;
    }

    Serial0.printf("[USB] 直写描述符已启动, 角色=%s (VID=%04X PID=%04X)\n",
                   s_role == USB_ROLE_MOUSE ? "鼠标" : "键盘",
                   (unsigned)s_deviceDesc.idVendor,
                   (unsigned)s_deviceDesc.idProduct);
    Serial0.printf("[USB] 描述符: 接口=%u 报告描述符=%u 字节 端点包=%u\n",
                   1u,
                   (unsigned)(s_role == USB_ROLE_MOUSE ? DESC_MOUSE_LEN : DESC_KBD_LEN),
                   (unsigned)(s_role == USB_ROLE_MOUSE ? 4 : 8));
}
