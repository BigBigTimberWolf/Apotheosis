#ifndef ESP_USB_HOST_H
#define ESP_USB_HOST_H

#include <Arduino.h>
#include <usb/usb_host.h>
#include <class/hid/hid.h>
#include <rom/usb/usb_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include "freertos/queue.h"
#include <cstring>
#include <ArduinoJson.h>
#include <iomanip>
#include <sstream>
#include <string>
#include <RingBuf.h>

#define LOG_LEVEL_OFF    0
#define LOG_LEVEL_FIXED  1
#define LOG_LEVEL_INFO   2
#define LOG_LEVEL_WARN   3
#define LOG_LEVEL_ERROR  4
#define LOG_LEVEL_DEBUG  5
#define LOG_LEVEL_PARSED 6

#define LOG_QUEUE_SIZE 20
#define LOG_MESSAGE_SIZE 620

#define USB_ACTION_OPEN_DEVICE   0x01
#define USB_ACTION_CLOSE_DEVICE  0x02


void flashLEDToggleTask(void *parameter);
extern SemaphoreHandle_t ledSemaphore;

extern RingBuf<char, 512> rxBuffer;    // RX Buffer for incoming data

class EspUsbHost
{
public:
    // Debug and Log
    bool debugModeActive = false;
    bool isReady = false;
    static bool deviceMouseReady;;
    uint8_t interval;
    bool isClientRegistering = false;
    bool deviceSuspended = false;
    static bool deviceConnected;
    uint32_t last_activity_time;
    TaskHandle_t cleanupTaskHandle = nullptr;

    uint8_t actionsPending = 0;
    uint8_t deviceAddress = 0;
    SemaphoreHandle_t mutex;

    enum Strutstate {
        STATE_IDLE,
        STATE_PROCESSING_COMMAND,
        STATE_SENDING_RESPONSE,
    };

    struct usb_string_descriptor_t {
        uint8_t bLength;
        uint8_t bDescriptorType;
        String data;
    };

    struct endpoint_data_t
    {
        uint8_t bInterfaceNumber;
        uint8_t bInterfaceClass;
        uint8_t bInterfaceSubClass;
        uint8_t bInterfaceProtocol;
        uint8_t bCountryCode;
    } endpoint_data_list[17];

    esp_err_t claim_err;
    usb_host_client_handle_t clientHandle;
    usb_device_handle_t deviceHandle;
    usb_transfer_t *usbTransfer[16];
    uint8_t usbTransferSize;
    uint8_t usbInterface[16];
    uint8_t usbInterfaceSize;

    TaskHandle_t usbTaskHandle = nullptr;
    TaskHandle_t clientTaskHandle = nullptr;

    struct HIDReportDescriptor {
        uint8_t reportId;
        uint8_t buttonSize;
        uint8_t xAxisSize;
        uint8_t yAxisSize;
        uint8_t wheelSize;
        uint8_t buttonStartByte;
        uint8_t xAxisStartByte;
        uint8_t yAxisStartByte;
        uint8_t wheelStartByte;
    };

    static struct HIDReportDescriptor HIDReportDesc;

    // ===== 按接口存档的报告描述符 (复合 HID 设备) =====
    //
    // 为什么要按接口存: 一台复合设备(键盘+鼠标接收器)的【每个 HID 接口】
    // 都有自己的报告描述符, 布局可能完全不同(鼠标 3 字节位移, 键盘 8 字节
    // 键码)。老版本只有一个全局 HIDReportDesc, 多个接口的描述符互相覆盖,
    // 结果用错误的布局去解码 -> 键盘整个失效。
    //
    // 索引 = 接口号(bInterfaceNumber)。
    enum : uint8_t { kMaxReportDescIfaces = 8 };

    HIDReportDescriptor iface_report_desc[kMaxReportDescIfaces];
    bool iface_descValid[kMaxReportDescIfaces];   // 该接口描述符是否已解析
    bool iface_isMouse[kMaxReportDescIfaces];     // 描述符里是否出现 Mouse usage
    bool iface_isKbd[kMaxReportDescIfaces];       // 描述符里是否出现 Keyboard usage

    struct DeviceInfo {
        uint8_t speed;                         // USB device speed
        uint8_t dev_addr;                      // Device address
        uint8_t vMaxPacketSize0;               // Maximum packet size for endpoint 0
        uint8_t bConfigurationValue;           // Configuration value
        char str_desc_manufacturer[64];        // Manufacturer string descriptor
        char str_desc_product[64];             // Product string descriptor
        char str_desc_serial_num[64];          // Serial number string descriptor
    } device_info;

    struct DescriptorDevice {
        uint8_t bLength;                       // Descriptor length
        uint8_t bDescriptorType;               // Descriptor type
        uint16_t bcdUSB;                       // USB specification number
        uint8_t bDeviceClass;                  // Device class
        uint8_t bDeviceSubClass;               // Device subclass
        uint8_t bDeviceProtocol;               // Device protocol
        uint8_t bMaxPacketSize0;               // Maximum packet size for endpoint 0
        uint16_t idVendor;                     // Vendor ID
        uint16_t idProduct;                    // Product ID
        uint16_t bcdDevice;                    // Device release number
        uint8_t iManufacturer;                 // Index of manufacturer string descriptor
        uint8_t iProduct;                      // Index of product string descriptor
        uint8_t iSerialNumber;                 // Index of serial number string descriptor
        uint8_t bNumConfigurations;            // Number of possible configurations
    } descriptor_device;

    #define MAX_ENDPOINT_DESCRIPTORS 10
    struct usb_endpoint_descriptor_t {
        uint8_t bLength;           
        uint8_t bDescriptorType;   
        uint8_t bEndpointAddress;  
        uint8_t endpointID;        
        String direction;          
        uint8_t bmAttributes;      
        String attributes;        
        uint16_t wMaxPacketSize;   
        uint8_t bInterval;         
    } endpoint_descriptors[MAX_ENDPOINT_DESCRIPTORS];
    int endpointCounter = 0; 

    struct DescriptorConfiguration {
        uint8_t bLength;             // Descriptor length
        uint8_t bDescriptorType;     // Descriptor type
        uint16_t wTotalLength;       // Total length of data for this configuration
        uint8_t bNumInterfaces;      // Number of interfaces
        uint8_t bConfigurationValue; // Configuration value
        uint8_t iConfiguration;      // Index of string descriptor describing this configuration
        uint8_t bmAttributes;        // Configuration characteristics
        uint8_t bMaxPower;           // Maximum power consumption (in 2mA units)
    } descriptor_configuration;

    #define MAX_INTERFACE_DESCRIPTORS 10
    struct usb_interface_descriptor_t {
        uint8_t bLength;           
        uint8_t bDescriptorType;   
        uint8_t bInterfaceNumber;  
        uint8_t bAlternateSetting; 
        uint8_t bNumEndpoints;     
        uint8_t bInterfaceClass;   
        uint8_t bInterfaceSubClass;
        uint8_t bInterfaceProtocol;
        uint8_t iInterface;
    } interface_descriptors[MAX_INTERFACE_DESCRIPTORS];
    int interfaceCounter = 0;

    // ===== 端点归属表 (复合 HID 设备透传的基础设施) =====
    //
    // 作用: 记录"每个 IN 端点属于哪个接口、是什么传输类型"。
    //
    // 为什么必须单独开一张表, 而不是复用 endpoint_data_list:
    //   endpoint_data_list 是按【接口号】索引的, 而这里需要按【端点号】
    //   索引 —— 两个下标空间会重叠(端点 1 会覆盖接口 1 的记录)。
    //   老版本的放大 bug 正是这种覆盖导致的。
    //
    // 为什么必须按端点定位接口(而不能"取第一个 HID 接口"):
    //   复合设备(如 2.4G 接收器: 键盘接口 + 鼠标接口)有多个 HID 接口,
    //   每个接口有自己的 IN 端点。"取第一个"会让后一个接口的报文被
    //   前一个接口的解析规则错误处理 —— 实测表现就是键盘完全失效。
    //
    // 索引用 epSlot(): 端点地址低 4 位是端点号, 最高位是方向。
    // 只用低 4 位(0..15)即可覆盖 S3 的端点范围, 因此表开 16 就够了;
    // 这里给到 32 留余量。
    enum : uint8_t { kEpSlotCount = 32 };

    struct EpOwner {
        uint8_t iface;      // 该端点所属的接口号
        bool    isIn;       // 是否为 IN 端点(设备->主机)
        uint8_t xferType;   // bmAttributes 低 2 位: 0=CTRL 1=ISOC 2=BULK 3=INT
        bool    used;       // 是否已被填写
    };
    EpOwner ep_owner[kEpSlotCount];

    // 端点地址 -> 槽位下标。取低 4 位(端点号), 忽略方向位。
    static inline uint8_t epSlot(uint8_t bEndpointAddress) {
        return (uint8_t)(bEndpointAddress & 0x0F);
    }

    #define MAX_HID_DESCRIPTORS 10 
    struct usb_hid_descriptor_t {
        uint8_t bLength;        
        uint8_t bDescriptorType;
        uint16_t bcdHID;        
        uint8_t bCountryCode;   
        uint8_t bNumDescriptors;
        uint8_t bReportType;
        uint16_t wReportLength;
    } hid_descriptors[MAX_HID_DESCRIPTORS];
    int hidDescriptorCounter = 0;

    struct usb_iad_desc_t {
        uint8_t bLength;
        uint8_t bDescriptorType;
        uint8_t bFirstInterface;
        uint8_t bInterfaceCount;
        uint8_t bFunctionClass;
        uint8_t bFunctionSubClass;
        uint8_t bFunctionProtocol;
        uint8_t iFunction;
    } descriptor_interface_association;

 struct UsbConfigurationDescriptor {
        uint8_t bLength;
        uint8_t bDescriptorType;
        uint16_t wTotalLength;
        uint8_t bNumInterfaces;
        uint8_t bConfigurationValue;
        uint8_t iConfiguration;
        uint8_t bmAttributes;
        uint8_t bMaxPower;
    } configurationDescriptor;

    #define MAX_UNKNOWN_DESCRIPTORS 10

    struct usb_unknown_descriptor_t {
        uint8_t bLength;
        uint8_t bDescriptorType;
        String data;
    } unknown_descriptors[MAX_UNKNOWN_DESCRIPTORS];
    int unknownDescriptorCounter = 0;

    struct ParsedValues
    {
        uint8_t usagePage;
        uint8_t usage;
        uint8_t reportId;
        uint8_t reportSize;
        uint8_t reportCount;
        int16_t logicalMin16;
        int16_t logicalMax;
        int8_t logicalMin8;
        int8_t logicalMax8;
        uint8_t level;
        uint8_t size;
        uint8_t collection;
        uint16_t usageMinimum;
        uint16_t usageMaximum;
        bool hasReportId;
        uint16_t currentBitOffset;
    };

    void begin(void);
    static void receiveSerial1(void *parameter);
    static void _clientEventCallback(const usb_host_client_event_msg_t *eventMsg, void *arg);
    static void _onReceiveControl(usb_transfer_t *transfer);
    static void monitorInactivity(void *arg);
    static void _onReceive(usb_transfer_t *transfer);
    void get_device_status();
    void suspend_device();
    void resume_device();
    bool serial1Send(const char *format, ...);
    void onConfig(const uint8_t bDescriptorType, const uint8_t *p);
    static String getUsbDescString(const usb_str_desc_t *str_desc);
    esp_err_t submitControl(const uint8_t bmRequestType, const uint8_t bDescriptorIndex, const uint8_t bDescriptorType, const uint16_t wInterfaceNumber, const uint16_t wDescriptorLength);
    void _configCallback(const usb_config_desc_t *config_desc);
   HIDReportDescriptor parseHIDReportDescriptor(uint8_t *data, int length);
    virtual void onReceive(const usb_transfer_t *transfer) {};
    virtual void onGone(const usb_host_client_event_msg_t *eventMsg) {};
    virtual void onMouse(hid_mouse_report_t report, uint8_t last_buttons);
    virtual void onMouseButtons(hid_mouse_report_t report, uint8_t last_buttons);
    virtual void onMouseMove(hid_mouse_report_t report);

    // 键盘透传(复合 HID 设备支持)。
    //
    // 老版完全没有键盘通路: 键盘接口的报文进 _onReceive 后因为 protocol
    // 判定不匹配而被静默丢弃。现在按接口类别分派, 键盘接口的报文会走到
    // 这里, 再把键码快照透传给设备侧。
    //
    // 参数即标准 HID 键盘报告的语义:
    //   mod     = 修饰键位图(bit0 L_Ctrl ... bit7 R_GUI)
    //   keys[6] = 当前按下的 6 个键码(0 表示空位)
    virtual void onKeyboardSnapshot(uint8_t mod, const uint8_t *keys);

    // 经 Serial1 发送二进制键盘透传帧(0x23 KB_REPORT)。
    void serial1SendKeyboardBinary(uint8_t modifiers, const uint8_t *keys, uint8_t keyCount);
    void receiveSerial0(void *command);
    void logRawBytes(const char *functionName, const uint8_t *data, uint16_t length);
    void cleanupTask(void *arg);

    void sendDeviceInfo();
    void sendDescriptorDevice();
    void sendEndpointDescriptors();
    void sendInterfaceDescriptors();
    void sendHidDescriptors();
    void sendIADescriptors();
    void sendEndpointData();
    void sendUnknownDescriptors();
    void sendDescriptorconfig();
    void handleIncomingCommands(const String &command);
    void usbLibraryTask(void *arg);
    void usbClientTask(void *arg);

    // USB fix


};


#endif
