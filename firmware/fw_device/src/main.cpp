#include "main.h"
#include "efuse.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#if USB_IS_DEBUG
    #warning "DEBUG MODE ENABLED: USB host will not work! For logging purposes only."
#endif

#ifndef FIRMWARE_VERSION
    #error "FIRMWARE_VERSION is not defined! Please set FIRMWARE_VERSION in the build flags."
#endif

const char* firmware = TOSTRING(FIRMWARE_VERSION);

void setup() {
    delay(1100);
    Serial0.begin(115200);
    pinMode(9, OUTPUT);
    digitalWrite(9, LOW);

    // 必须在 begin() 之前设置: arduino-esp32 的 UART RX 环缓冲默认只有 256 字节,
    // 5Mbps 下约 0.5ms 就写满, 端点描述符 JSON 可达 ~1700 字节 -> 必然丢字节 -> JSON 截断.
    Serial1.setRxBufferSize(8192);
    Serial1.begin(5000000, SERIAL_8N1, 1, 2);

    if (USB_IS_DEBUG) {
        Serial0.println("WARNING: Debug mode is enabled!");
        Serial0.println("USB Host will not work!");
        Serial0.println("For logging purposes only!!!\n");
    }

    Serial0.print("MAKCM version ");
    Serial0.print(firmware);
    Serial0.println("\n");

    // 读出 USB_PHY_SEL eFuse 的真实值 —— 不烧, 只读。
    // USB_PHY_SEL 决定 USB 引脚接到哪个 PHY:
    //   未烧(0) -> 内部 USB-OTG PHY  -> TinyUSB 可用
    //   已烧(1) -> USB-Serial-JTAG    -> TinyUSB 无法工作(mounted 恒为 0)
    // burn_usb_phy_sel_efuse() 会【不可逆地】烧写这一位, 烧错这块板的 USB Device 功能
    // 永久失效。本工程不再自动烧写: 板级出厂时该 bit 通常已经处理好, 固件不该在
    // 每次开机替用户做这个决定; 且实测本板未烧状态下 USB Device 正常工作。
    // 保留读出供诊断, 不再调用 burn_usb_phy_sel_efuse()。
    {
        const int phySel = (int)esp_efuse_read_field_bit(ESP_EFUSE_USB_PHY_SEL);
        Serial0.printf("[EFUSE] USB_PHY_SEL=%d (%s)\n", phySel,
                       phySel ? "已烧 -> USB 走 Serial-JTAG, TinyUSB 不可用"
                              : "未烧 -> USB 走 OTG PHY, TinyUSB 可用");
    }

    Serial1.onReceive(serial1ISR);
    Serial0.onReceive(serial0ISR);
    tasks();
}

void loop() {
    if (!USB_IS_DEBUG) {
        requestUSBDescriptors();
    }
}
