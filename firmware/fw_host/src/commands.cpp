#include "EspUsbHost.h"
#include "esp_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <sstream>



void EspUsbHost::handleIncomingCommands(const String &command)
{
    ESP_LOGI("EspUsbHost", "Received command: %s", command.c_str());

    if (command == "DEBUG_ON")
    {
        debugModeActive = true;
        serial1Send("Debug mode activated.\n");
        serial1Send("USB_ISDEBUG\n");
        ESP_LOGI("EspUsbHost", "Debug mode activated.");
    }
    else if (command == "DEBUG_OFF")
    {
        if (USB_IS_DEBUG)
        {
            return;
        }
        else
        {
            debugModeActive = false;
            ESP_LOGI("EspUsbHost", "Debug mode deactivated. System will restart.");
            Serial1.print("USB_GOODBYE\n");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
    else if (command == "READY")
    {
        if (debugModeActive)
        {
            serial1Send("USB_ISDEBUG\n");
            ESP_LOGI("EspUsbHost", "Debug mode is active. Sent USB_ISDEBUG.");
        }
        else
        {
            if (EspUsbHost::deviceConnected)
            {
                serial1Send("USB_HELLO\n");
                ESP_LOGI("EspUsbHost", "Device is connected.");
            }
            else
            {
                serial1Send("USB_ISNULL\n");
                ESP_LOGW("EspUsbHost", "No device is connected.");
            }
        }
    }
    else if (command == "USB_INIT")
    {
        deviceMouseReady = true;
        serial1Send("USB Initialized. Mouse ready.\n");
        ESP_LOGI("EspUsbHost", "USB initialized. Mouse ready.");
        // 设备就绪后再协商位移帧格式: 成功则后续用二进制 0x01, 失败自动停在 ASCII
        onDeviceReady();
    }
    else if (command == "sendDeviceInfo")
    {
        sendDeviceInfo();
        serial1Send("Device information sent.\n");
        ESP_LOGI("EspUsbHost", "Sending device information.");
    }
    else if (command == "sendDescriptorDevice")
    {
        sendDescriptorDevice();
        serial1Send("Device descriptor sent.\n");
        ESP_LOGI("EspUsbHost", "Sending device descriptor.");
    }
    else if (command == "sendEndpointDescriptors")
    {
        sendEndpointDescriptors();
        serial1Send("Endpoint descriptors sent.\n");
        ESP_LOGI("EspUsbHost", "Sending endpoint descriptors.");
    }
    else if (command == "sendInterfaceDescriptors")
    {
        sendInterfaceDescriptors();
        serial1Send("Interface descriptors sent.\n");
        ESP_LOGI("EspUsbHost", "Sending interface descriptors.");
    }
    else if (command == "sendHidDescriptors")
    {
        sendHidDescriptors();
        serial1Send("HID descriptors sent.\n");
        ESP_LOGI("EspUsbHost", "Sending HID descriptors.");
    }
    else if (command == "sendIADescriptors")
    {
        sendIADescriptors();
        serial1Send("Interface Association Descriptors sent.\n");
        ESP_LOGI("EspUsbHost", "Sending Interface Association Descriptors.");
    }
    else if (command == "sendEndpointData")
    {
        sendEndpointData();
        serial1Send("Endpoint data sent.\n");
        ESP_LOGI("EspUsbHost", "Sending endpoint data.");
    }
    else if (command == "sendUnknownDescriptors")
    {
        sendUnknownDescriptors();
        serial1Send("Unknown descriptors sent.\n");
        ESP_LOGI("EspUsbHost", "Sending unknown descriptors.");
    }
    else if (command == "sendRawHidDescriptors")
    {
        sendRawHidDescriptors();
        serial1Send("Raw HID report descriptors sent.\n");
        ESP_LOGI("EspUsbHost", "Sending raw HID report descriptors.");
    }
    else if (command == "sendDescriptorconfig")
    {
        sendDescriptorconfig();
        serial1Send("Configuration descriptor sent.\n");
        ESP_LOGI("EspUsbHost", "Sending configuration descriptor.");
    }
    // ---- 瞬时屏蔽真实输入 ----
    // 形如 km.mask(50) -> 屏蔽 50ms; km.mask(0) -> 立即解除。
    // 采用与其它 km.* 一致的前缀匹配, 便于上位机沿用现有命令拼装方式。
    else if (command.startsWith("km.mask("))
    {
        const int lp = command.indexOf('(');
        const int rp = command.indexOf(')', lp + 1);
        if (lp >= 0 && rp > lp) {
            const String arg = command.substring(lp + 1, rp);
            // 必须校验是否为合法整数: String::toInt() 对非数字返回 0, 会让
            // "km.mask(abc)" 这种畸形输入落进 ms<=0 分支被当成【解除屏蔽】执行。
            // 畸形输入应被拒绝, 而不是产生一个语义完全相反的动作。
            bool numeric = arg.length() > 0;
            for (unsigned int i = 0; i < arg.length(); ++i) {
                const char c = arg[i];
                if (i == 0 && (c == '-' || c == '+')) continue;
                if (c < '0' || c > '9') { numeric = false; break; }
            }
            if (!numeric) {
                serial1Send("km.mask err: not a number\n");
                return;
            }
            const long ms = arg.toInt();
            if (ms <= 0) {
                maskStop();
                serial1Send("km.mask(0) ok\n");
            } else {
                // 注意: 记录"当前按着什么"由 maskStart() 内部完成 —— 放在这里
                // 会漏掉其它调用路径, 导致恢复时不补发抬键、按键卡死。
                maskStart((uint32_t)ms);
                serial1Send("km.mask(%ld) ok\n", ms);
            }
        } else {
            serial1Send("km.mask err: bad format\n");
        }
    }
    else if (command == "km.maskoff")
    {
        maskStop();
        serial1Send("km.maskoff ok\n");
    }
    else
    {
        serial1Send("Unknown command received: %s\n", command.c_str());
        ESP_LOGW("EspUsbHost", "Unknown command received: %s", command.c_str());
    }
}
