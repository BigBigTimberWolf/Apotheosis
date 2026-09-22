#include "EspUsbHost.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// ============================================================================
// Serial1 二进制帧发送器(复合 HID 设备透传用)
//
// 帧格式: A5 5C | LEN | SEQ | CMD | PAYLOAD | CRC16(2B, MODBUS, 低字节先)
//   LEN = CMD + PAYLOAD 的总字节数(不含 A5 5C / LEN / SEQ / CRC)
//   CRC 覆盖范围 = LEN 起、共 (3 + LEN) 字节(LEN, SEQ, CMD, PAYLOAD)
//
// 与设备侧 proto_parser 的解析严格对应(见 fw_device/include/proto_parser.h)。
// 键盘帧 CMD = 0x23 KB_REPORT, PAYLOAD = [modifiers][key0..key5]
// ============================================================================
static uint8_t s_tx_seq = 0;
static char    s_tx_frame[24];      // 最大帧 = 2+1+1+1+1+6+2 = 14, 24 足够

void EspUsbHost::serial1SendKeyboardBinary(uint8_t modifiers, const uint8_t *keys, uint8_t keyCount)
{
    if (keyCount > 6) keyCount = 6;
    const uint8_t len = (uint8_t)(1 + keyCount);

    s_tx_frame[0] = (char)0xA5;
    s_tx_frame[1] = (char)0x5C;
    s_tx_frame[2] = (char)len;
    s_tx_frame[3] = (char)(s_tx_seq++);
    s_tx_frame[4] = (char)0x23;                      // CMD_KB_REPORT
    s_tx_frame[5] = (char)modifiers;
    for (uint8_t i = 0; i < keyCount; ++i) {
        s_tx_frame[6 + i] = (char)keys[i];
    }

    const int crcStart = 2;
    const int crcLen   = 3 + len;
    uint16_t crc = 0xFFFF;
    for (int i = crcStart; i < crcStart + crcLen; ++i) {
        crc ^= (uint8_t)s_tx_frame[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    s_tx_frame[crcStart + crcLen]     = (char)(crc & 0xFF);
    s_tx_frame[crcStart + crcLen + 1] = (char)((crc >> 8) & 0xFF);

    Serial1.write((const uint8_t *)s_tx_frame, (size_t)(len + 7));
}


// Buffers defined here
RingBuf<char, 512> rxBuffer;
RingBuf<char, 512> txBuffer;
SemaphoreHandle_t ledSemaphore;
TaskHandle_t cleanupTaskHandle = NULL;
TaskHandle_t rxSerialTaskHandle;
#define USB_TASK_PRIORITY 1
#define CLIENT_TASK_PRIORITY 2

void EspUsbHost::begin(void)
{
    usbTransferSize = 0;
    deviceSuspended = false;
    last_activity_time = millis();

    const usb_host_config_t host_config = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    usb_host_install(&host_config);

    ledSemaphore = xSemaphoreCreateBinary();

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->receiveSerial0(arg); 
    }, "RxTaskSerial0", 4096, this, 5, &rxSerialTaskHandle) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create RxTaskSerial0.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->receiveSerial1(arg); 
    }, "RxTaskSerial1", 4096, this, 5, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create RxTaskSerial1.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->usbLibraryTask(arg); 
    }, "usbLibTask", 4096, this, USB_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create usbLibTask.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->usbClientTask(arg); 
    }, "usbClientTask", 4096, this, CLIENT_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create usbClientTask.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->cleanupTask(arg); 
    }, "CleanupTask", 4096, this, 5, &cleanupTaskHandle) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create CleanupTask.");
    }

    if (xTaskCreate([](void *arg) { 
        static_cast<EspUsbHost *>(arg)->monitorInactivity(arg); 
    }, "MonitorInactivityTask", 3200, this, 3, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create MonitorInactivityTask.");
    }

    if (xTaskCreate(flashLEDToggleTask, "LED Flash Task", 1500, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE("EspUsbHost", "Failed to create LED Flash Task.");
    }

    ESP_LOGI("EspUsbHost", "EspUsbHost::begin() completed. Tasks created.");
}



// combine both maybe mutex?

void handleSerialInput(HardwareSerial &serial, EspUsbHost *instance) {
    while (serial.available() > 0) {
        char byte = serial.read();

        if (byte == '\r') continue;

        if (!rxBuffer.isFull()) {
            rxBuffer.push(byte);
        } else {
            ESP_LOGW("EspUsbHost", "RX buffer overflow detected.");
            break;
        }

        if (byte == '\n') {
            char commandBuffer[620];
            int commandIndex = 0;

            while (!rxBuffer.isEmpty() && commandIndex < sizeof(commandBuffer) - 1) {
                rxBuffer.pop(commandBuffer[commandIndex++]);
                if (commandBuffer[commandIndex - 1] == '\n') break;
            }

            commandBuffer[commandIndex] = '\0';

            if (commandIndex > 0 && commandBuffer[commandIndex - 1] == '\n') {
                commandBuffer[commandIndex - 1] = '\0';
            }

            instance->handleIncomingCommands(commandBuffer);
        }
    }
}

void EspUsbHost::receiveSerial0(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1));                                                               // Set 1ms, be lazy be happy
        handleSerialInput(Serial, instance);
    }
}

void EspUsbHost::receiveSerial1(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1));                                                              // Set 1ms, be lazy be happy
        handleSerialInput(Serial1, instance);
    }
}

bool EspUsbHost::serial1Send(const char *format, ...)
{
    char logMsg[256];

    va_list args;
    va_start(args, format);
    int len = vsnprintf(logMsg, sizeof(logMsg), format, args);
    va_end(args);

    if (len > 0) {
        if (len >= sizeof(logMsg)) len = sizeof(logMsg) - 1;
        Serial1.write((const uint8_t*)logMsg, len);
        return true;
    }

    return false;
}

void EspUsbHost::monitorInactivity(void *arg)
{
    // 定制固件: 自动挂起已禁用 —— 根治"游戏PC开机后鼠标必须重新插拔"bug。
    // 原厂逻辑: 空闲10s -> suspend_device(); 挂起后 _onReceive 不再resubmit传输,
    // 而恢复逻辑恰恰写在 _onReceive 里等数据触发 => 死锁, 只有重新插拔能救。
    (void)arg;
    vTaskDelete(NULL);
}

void EspUsbHost::usbLibraryTask(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

    while (true) {
        uint32_t event_flags;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (err != ESP_OK) {
            ESP_LOGE("EspUsbHost", "usb_host_lib_handle_events() failed with error: %x", err);
            continue;
        }

        if (instance->clientHandle == NULL || !instance->isClientRegistering) {
            ESP_LOGI("EspUsbHost", "Registering client...");
            const usb_host_client_config_t client_config = {
                .max_num_event_msg = 10,
                .async = {
                    .client_event_callback = instance->_clientEventCallback,
                    .callback_arg = instance,
                }
            };

            err = usb_host_client_register(&client_config, &instance->clientHandle);
            if (err != ESP_OK) {
                ESP_LOGW("EspUsbHost", "Failed to register client, retrying...");
                vTaskDelay(pdMS_TO_TICKS(100));
            } else {
                ESP_LOGI("EspUsbHost", "Client registered successfully.");
                instance->isClientRegistering = true;
            }
        }
    }
}


void EspUsbHost::usbClientTask(void *arg)
{
    EspUsbHost *instance = static_cast<EspUsbHost *>(arg);

    while (true) {
        if (!instance->isClientRegistering) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        usb_host_client_handle_events(instance->clientHandle, portMAX_DELAY);
    }
}

void flashLEDToggleTask(void *parameter)
{
    pinMode(9, OUTPUT);

    while (true) {
        if (xSemaphoreTake(ledSemaphore, portMAX_DELAY) == pdTRUE) {
            digitalWrite(9, HIGH);
            vTaskDelay(pdMS_TO_TICKS(25));
            digitalWrite(9, LOW);
        }
    }
}

void flashLED()
{
    if (ledSemaphore != NULL) {
        xSemaphoreGive(ledSemaphore);
    }
}
