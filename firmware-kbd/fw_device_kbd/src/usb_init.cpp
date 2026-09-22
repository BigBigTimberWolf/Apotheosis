// ============================================================================
// usb_init.cpp —— TinyUSB 设备栈启动
//
// 【架构结论 (已用符号验证)】
//   Arduino 核心里这三个回调是 __attribute__((weak)):
//       esp32-hal-tinyusb.c:270  tud_descriptor_configuration_cb
//       esp32-hal-tinyusb.c:279  tud_descriptor_device_cb
//       esp32-hal-tinyusb.c:288  tud_descriptor_string_cb
//   而预编译的 libarduino_tinyusb.a 对它们都是【未定义引用(U)】。
//
//   => 我们在 usb_desc.cpp 里给出的【强定义】会直接覆盖 Arduino 的弱定义,
//      不需要 --wrap, 不需要改框架源。这就是本方案能成立的根本原因。
//
// 【必须避免的冲突】
//   Arduino 的 USBHID 类(USBHID.cpp)会注册 USB_INTERFACE_HID 接口, 并让
//   Arduino 的 config 描述符里出现它自己的 HID 接口。若它被链接进来,
//   tinyusb_load_enabled_interfaces() 会尝试装载那个接口, 导致我们的配置
//   描述符与接口列表不一致。
//
//   => 本工程【绝不构造 USB / USBHID 对象】, 也不调用 USB.begin()。
//      只需调用 tinyusb_init() 把栈跑起来即可。
// ============================================================================

#include <Arduino.h>
#include "tusb.h"
#include "esp32-hal-tinyusb.h"
#include "kbd_usb.h"
#include "kbd_wire.h"

// 由 usb_desc.cpp 提供: 运行时设定设备身份
// 注意必须用 extern "C": usb_desc.cpp 里是按 C 链接定义的, 否则名字修饰不匹配。
extern "C" void kbdDescSetIdentity(uint16_t vid, uint16_t pid, uint16_t bcd);

static bool s_started = false;

// ---------------------------------------------------------------------------
// 等右板发来真键盘的 VID/PID (带超时)
//
// 【为什么必须等】
//   被控机在【枚举那一刻】就要读到 VID/PID。要让被控机看到的值等于真键盘,
//   就必须在 tinyusb_init() 之前拿到 —— 枚举一旦开始, 描述符就固定了。
//
// 【时序上可行吗】
//   插 USB-A 时两芯片同时上电。右板要先枚举真键盘(约 300~500ms)才能拿到
//   设备描述符, 所以左板必须等一会儿。
//
// 【超时后怎么办】
//   用内置默认值(3554:FA09)照常枚举 —— 保证右板没响应时也能正常工作,
//   不会因为等不到身份而变砖。
// ---------------------------------------------------------------------------
#define KBD_ID_WAIT_MS   1500

static void waitForIdentity(void)
{
    uint8_t buf[KBD_MAX_FRAME];
    int len = 0;
    const uint32_t t0 = millis();

    while (millis() - t0 < KBD_ID_WAIT_MS) {
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
                    uint8_t seq = 0, type = 0;
                    const uint8_t *pl = nullptr;
                    const int n = kbdUnpackFrameEx(buf, len, &seq, &type, &pl);

                    if (n == KBD_ID_LEN && type == KBD_TYPE_ID) {
                        const uint16_t vid = (uint16_t)(pl[0] | (pl[1] << 8));
                        const uint16_t pid = (uint16_t)(pl[2] | (pl[3] << 8));
                        const uint16_t bcd = (uint16_t)(pl[4] | (pl[5] << 8));

                        if (vid != 0 && pid != 0) {
                            kbdDescSetIdentity(vid, pid, bcd);
                            Serial.printf("[KBD-DEV] 真键盘身份: VID=%04X PID=%04X bcd=%04X\n",
                                          vid, pid, bcd);
                            return;
                        }
                    }
                    len = 0;
                } else if (len > need) {
                    len = 0;
                }
            }
        }
        delay(5);
    }

    Serial.println("[KBD-DEV] 未收到真键盘身份, 用内置默认 3554:FA09");
}

void kbdUsbBegin(void)
{
    if (s_started) return;
    s_started = true;

    // ★ 先等右板送来真键盘的 VID/PID, 再枚举。
    //   必须放在 tinyusb_init() 之前 —— 枚举开始后描述符就固定了。
    waitForIdentity();

    tinyusb_device_config_t cfg = {};
    // vid/pid 为 0 -> 不覆盖, 由 usb_desc.cpp 里保存的身份决定
    cfg.vid = 0;
    cfg.pid = 0;
    cfg.product_name      = nullptr;   // 不覆盖(字符串由 usb_desc.cpp 提供)
    cfg.manufacturer_name = nullptr;
    cfg.serial_number     = nullptr;
    cfg.webusb_url        = nullptr;

    // ★ 关键: usb_class = 0x00
    //   表示"每个接口自己声明类" -> Windows 交给 usbccgp 复合设备驱动,
    //   再按接口分别加载 kbdhid / mouhid。与真键盘接收器一致。
    //   (Arduino 默认用 TUSB_CLASS_MISC + IAD, 那是另一套结构, 这里必须改掉)
    cfg.usb_class    = 0x00;
    cfg.usb_subclass = 0x00;
    cfg.usb_protocol = 0x00;

    cfg.usb_attributes = TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP;
    cfg.usb_power_ma   = 100;
    cfg.webusb_enabled = false;

    const esp_err_t err = tinyusb_init(&cfg);
    if (err != ESP_OK) {
        Serial.printf("[KBD-DEV] tinyusb_init failed: %d\n", (int)err);
    } else {
        Serial.println("[KBD-DEV] tinyusb_init OK (自定义描述符已生效)");
    }
}

void kbdUsbTask(void)
{
    // tinyusb_init() 内部已创建任务循环调用 tud_task(), 这里无需额外处理。
}
