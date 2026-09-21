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
#include <RingBuf.h>

#define USB_ACTION_OPEN_DEVICE   0x01
#define USB_ACTION_CLOSE_DEVICE  0x02


void flashLEDToggleTask(void *parameter);
extern SemaphoreHandle_t ledSemaphore;

// 每链路独立的行缓冲(原为单个全局 512 字节缓冲, 被两条任务共用 -> 串话+竞态,
// 且装不下最坏 1050 字节的长行)。详见 esp_tasks.cpp 的定义处说明。
extern RingBuf<char, 4096> rxBuffer;    // Serial0
extern RingBuf<char, 4096> rxBuffer1;   // Serial1

class EspUsbHost
{
public:
    // Debug and Log
    bool debugModeActive = false;
    bool isReady = false;
    static bool deviceMouseReady;;
    bool isClientRegistering = false;
    bool deviceSuspended = false;
    static bool deviceConnected;
    uint32_t last_activity_time;
    TaskHandle_t cleanupTaskHandle = nullptr;

    // 说明: 原先这里声明了 `SemaphoreHandle_t mutex;` 但从未创建/取用/释放,
    // 是纯粹的死成员(已删除)。真正的跨任务互斥由 s_mask_mtx 承担。

    // 上一帧的按键位图. 原来它是 _onReceive 里的 static 局部变量:
    //  - 被所有端点共用, 且 设备重连 后不会复位;
    //  - 更糟的是, 报文解码被丢弃(布局非法/ReportID 不匹配)时它也不更新,
    //    于是后续帧的 last_buttons 与实际不一致 -> 抬键的差分判定失效 -> 按键卡死.
    // 现在提升为成员, 并在设备插入/拔出时显式复位。
    uint8_t lastReportedButtons = 0;

    // ---- Serial1 位移帧格式协商 ----
    // 0 = ASCII km.move (默认, 向后兼容)   1 = 二进制 0x01 MOVE
    // 只有设备对 "km.movefmt(1)" 回了 OK 才会置 1; 超时/无应答则保持 0。
    bool     binaryMovesEnabled = false;
    uint8_t  moveFmtAttempts = 0;
    uint32_t moveFmtDeadline = 0;
    bool     moveFmtPending = false;
    // 0 = 关闭协商(永远用 ASCII)。需要强制 ASCII 时把它设为 0。
    uint8_t  moveFmtMaxAttempts = 3;

    // 说明: 原先这里还有一个 `enum Strutstate { STATE_IDLE, ... }`(拼写本身
    // 就是 StructState 的笔误), 全工程零引用, 已删除。

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

    // 端点 -> 所属接口 的权威映射。
    //
    // 下标 = (端点号 & 0x0F) | (方向 IN ? 0x10 : 0x00), 共 32 槽。
    //
    // 为什么必须带方向: bEndpointAddress 的 bit7 是方向位, 而端点号只有 4 位。
    // 复合设备(鼠标 + 键盘 + 厂商接口)里 IN/OUT 端点号会重复, 只按低 4 位索引
    // 会让两个不同端点互相覆盖 —— 这正是旧 ep_owner_class[16]/protocol[16] 的缺陷,
    // 它只按端点号索引, 方向信息被丢弃。
    //
    // 为什么不能只按接口号: 接口号也无法区分同一接口内的多个端点, 且端点才是
    // 数据实际来源(_onReceive 拿到的是 bEndpointAddress)。所以这里以端点为键,
    // 存"该端点归属哪个接口", 报文布局再去 descPerIface[] 里按接口取。
    // 用 enum 而非 static const 成员: enum 是纯编译期常量, 绝不会被 odr-use,
    // 因此不需要类外定义, 在 C++11/14 下取地址/强制转换也都安全。
    enum : uint8_t { kEpSlotCount = 32, kMaxIface = 8 };

    struct EpOwner {
        uint8_t iface;      // 所属接口号
        uint8_t xferType;   // bmAttributes 传输类型(中断/控制/...)
        bool    isIn;       // 方向
        bool    used;       // true = 该槽位已被端点描述符填写
    };
    EpOwner ep_owner[kEpSlotCount];

    // 端点槽位 = 端点号(低 4 位) | 方向位(bit4)。传入 bEndpointAddress 原值即可。
    static uint8_t epSlot(uint8_t bEndpointAddress) {
        return (uint8_t)((bEndpointAddress & 0x0F) | ((bEndpointAddress & 0x80) ? 0x10 : 0x00));
    }

    // 控制传输回调的上下文。
    //
    // 回调( _onReceiveControl )是静态函数, 只能透过 transfer->context 拿到对象;
    // 而 usb_transfer_s 里没有任何字段可以顺手携带"这是哪个接口的描述符" ——
    // bInterval 只存在于端点描述符(usb_types_ch9.h), 不在 transfer 结构里。
    // 复合设备会连续发起多个接口的 GET_DESCRIPTOR(0x22), 缺了接口号就只能全局猜,
    // 那正是旧代码用单个静态 HIDReportDesc 被互相覆盖的根因。
    // 因此用一个短生命周期的小结构把 (对象, 接口号) 一起传给回调。
    struct ControlContext {
        EspUsbHost *host;
        uint8_t     iface;
    };
    // 数组开 kMaxIface+1 个槽: 前 kMaxIface 个按接口号索引, 末位专门留给
    // 非描述符控制传输(kCtlCtxMisc)。绝不能复用接口号的槽位 —— 否则接口 7
    // 的描述符请求与 SET_FEATURE 会互相覆盖对方的 context。
    ControlContext controlCtx[kMaxIface + 1];

    // 非描述符控制传输(SET_FEATURE/CLEAR_FEATURE 远程唤醒)专用的上下文槽位。
    //
    // 为什么必须单独占一个槽: 那些传输同样把 callback 设成 _onReceiveControl,
    // 而该回调把 context 解释为 ControlContext*。若沿用旧写法传裸 this,
    // 回调读到的 ctx->host 就是对象首字节(debugModeActive=0) → nullptr →
    // 回调立刻 return, 之后 get_device_status() 永远发不出去, 设备状态机停滞。
    // iface = 0xFF 是"这不是 HID 描述符请求"的哨兵值, 回调据此直接放行丢弃。
    enum : uint8_t { kCtlCtxMisc = kMaxIface, kCtlIfaceNone = 0xFF };

    esp_err_t claim_err;
    usb_host_client_handle_t clientHandle;
    usb_device_handle_t deviceHandle;
    usb_transfer_t *usbTransfer[16];
    uint8_t usbTransferSize;
    uint8_t usbInterface[16];
    uint8_t usbInterfaceSize;

    // 说明: 原先这里还有 usbTaskHandle / clientTaskHandle 两个成员, 但它们
    // 创建任务时传的就是 NULL(见 esp_tasks.cpp 的 begin()), 从未被赋过值,
    // 也无任何读取者 —— 已删除。

    // 从 HID Report Descriptor 解析出的报表布局.
    //
    // 修复要点: 原来用 xAxisSize/yAxisSize 表示"轴宽"(8/12/16), 但轴宽是拿
    // LOGICAL_MAXIMUM 猜的 —— 1 字节的 LOGICAL_MAXIMUM 只会写进 logicalMax8, 而判定
    // 读的是 16 位 logicalMax(恒为 0), 于是永远走 12-bit 分支: 标准 8-bit 鼠标被当成
    // 12-bit 解码 => Y 变成"自身高半字节 + 滚轮字节低半字节"的垃圾, 滚轮读到的是
    // 分配块里从未写过的填充字节 => 每次移动都往游戏里注入随机滚动.
    // 现在改为用 REPORT_SIZE 直接表达轴宽, 并记录"每个物理字节最多承载多少位"
    // (maxAxisBitsPerByte), 供解码端判断该走哪种位打包格式。
    struct HIDReportDescriptor {
        uint8_t  reportId;
        bool     hasReportId;            // 描述符里出现过 REPORT_ID -> 报文首字节是 ID
        bool     valid;                  // 解析成功(至少拿到按钮或某个轴)
        uint8_t  buttonSize;             // 按钮字段占的位数
        uint16_t buttonBitOffset;        // 按钮字段的位偏移(注意是"位", 可达 4096)
        uint8_t  buttonStartByte;
        uint16_t xAxisBitOffset;
        uint8_t  xAxisStartByte;
        uint8_t  xAxisSize;              // 每轴位的位数(REPORT_SIZE): 8 / 12 / 16
        uint16_t yAxisBitOffset;
        uint8_t  yAxisStartByte;
        uint8_t  yAxisSize;
        uint8_t  wheelStartByte;
        uint8_t  wheelSize;              // 位数
        uint8_t  maxAxisBitsPerByte;     // 同一字节里打包了几个轴(1=顺序存放, 2=半字节交织)

        // ===== 键盘字段 =====
        //
        // 标准 HID 键盘报文是 8 字节: [修饰键][保留][键码 x6]。
        // 但游戏键盘常带额外字段(NKRO 多键无冲、音量旋钮、宏键), 所以这里记录
        // 实际偏移而不是写死 0/2。
        //
        // 修饰键(Usage Page 0x07, Usage 0xE0..0xE7)是 8 个 1-bit 字段,
        // 描述符里会写成 REPORT_SIZE=1 / REPORT_COUNT=8 / USAGE_MIN..MAX,
        // 解析后 buttonStartByte 即修饰键字节, modifierBits 为位数(通常 8)。
        // 主键码数组是 Usage 0x00..0x65 的 REPORT_COUNT 个 8-bit 字段。
        bool     isKeyboard;             // 该接口判定为键盘
        uint8_t  keyArrayStartByte;      // 键码数组起始字节
        uint8_t  keyArrayCount;          // 键码个数(标准键盘=6, NKRO 可能更大)
        uint8_t  modifierByte;           // 修饰键所在字节(通常 0)
        bool     hasModifierByte;
    };

    // 每个接口各存一份布局。
    //
    // 这是复合 HID 透传的关键修复。原来 HIDReportDesc 是全局唯一的一份,
    // 而复合设备(鼠标接口 + 键盘接口 + 厂商接口)会对**每个** HID 接口都发一次
    // GET_DESCRIPTOR(0x22) 来取报告描述符, 后解析的接口会整体覆盖先解析的:
    //   鼠标描述符先写入 -> 键盘描述符后写入(键盘也有 Usage Page 0x09 / Usage 1..16,
    //   于是 haveButtons=true -> valid=true, 轴宽被补成默认 8 -> 校验全部通过)
    //   => 布局"合法但错误": XY 偏移指向键盘报文里不存在的字节
    //   => 鼠标移动/滚轮静默失效, 且不打任何告警(因为 valid=1)
    //
    // 现在按接口号各存一份, 报文按"实际来源端点 -> 所属接口"取自己那份布局。
    // 附带修掉了 Report ID 单值化: 复合设备常在鼠标/键盘接口上用不同 Report ID,
    // 单份 hasReportId/reportId 会让另一个接口所有偏移整体错位 1 字节。
    // 原样转发模式下锁定的唯一键盘接口(-1 = 未选定)。
    // 复合键盘接收器有多个 HID 接口, 必须只认一个, 否则报文交错。
    int kbdRawIface = -1;
    void resetKeyboardRawIface() { kbdRawIface = -1; }

    HIDReportDescriptor descPerIface[kMaxIface];
    bool                descPerIfaceValid[kMaxIface] = {false};

    // 真设备各接口的原始 HID 报告描述符字节。
    //
    // 阶段 4(报文格式克隆)的数据源: fw_device 拿它原样应付被控机的
    // GET_DESCRIPTOR(0x22), 使被控机看到的报文格式与真设备逐字节一致。
    // 512 字节对绝大多数 HID 描述符够用(标准鼠标约 50 字节, 游戏鼠标约 120),
    // 超出部分截断并告警 —— 截断只影响克隆保真度, 不影响本机解析。
    struct RawHidDesc {
        uint8_t  bytes[512];
        uint16_t len;
    };
    RawHidDesc rawDescPerIface[kMaxIface];

    // "报告描述符没解析成功, 已退回标准 8 位鼠标兜底解码" 只告警一次。
    //
    // 这个标志存在的意义: 兜底路径是静默工作的(否则每帧都刷日志会拖垮热路径),
    // 但排查时必须能确认到底走没走兜底 —— 打一次日志就够, 且不影响帧率。
    bool mouseFallbackLogged = false;

    // 被选中的"鼠标接口"(-1 = 尚未确定)。
    //
    // 复合 HID 里可能有接口的 protocol == NONE(厂商自定义/无协议), 旧代码把它们
    // 一律当作鼠标候选, 于是同一份位移可能被多个接口各注入一次。现在显式选一个:
    // 优先 protocol==MOUSE(2), 其次 NONE(0); 选中后只处理该接口的端点。
    int8_t mouseIface = -1;

    // 被选中的"键盘接口"(-1 = 没有键盘)。
    //
    // 与 mouseIface 完全对称。支持"只插键盘"的情形: 此时 mouseIface 保持 -1,
    // 鼠标链路整体空转, 键盘链路独立工作, 不再像旧代码那样因为没有鼠标而
    // 让整台设备的处理逻辑全部失效。
    int8_t keyboardIface = -1;

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

    void begin(void);
    static void receiveSerial1(void *parameter);
    // 设备握手完成后调用一次, 启动"位移帧格式"协商(非阻塞)
    void onDeviceReady();
    // 收到 Serial1 上的一整行 ASCII(设备回包)时调用
    void onSerial1Line(const char *line);
    static void _clientEventCallback(const usb_host_client_event_msg_t *eventMsg, void *arg);
    static void _onReceiveControl(usb_transfer_t *transfer);
    static void monitorInactivity(void *arg);
    static void _onReceive(usb_transfer_t *transfer);
    void get_device_status();
    void suspend_device();
    void resume_device();
    bool serial1Send(const char *format, ...);
    void serial1SendMoveBinary(int8_t x, int8_t y);
    void serial1SendKeyboardBinary(uint8_t modifiers, const uint8_t *keys, uint8_t keyCount);
    void serial1SendKeyboardRaw(uint8_t iface, const uint8_t *report, uint8_t len);
    void onConfig(const uint8_t bDescriptorType, const uint8_t *p);
    static String getUsbDescString(const usb_str_desc_t *str_desc);
    esp_err_t submitControl(const uint8_t bmRequestType, const uint8_t bDescriptorIndex, const uint8_t bDescriptorType, const uint16_t wInterfaceNumber, const uint16_t wDescriptorLength);
    void _configCallback(const usb_config_desc_t *config_desc);
   // ifaceNum: 该描述符属于哪个接口; 解析结果写入 descPerIface[ifaceNum]
   HIDReportDescriptor parseHIDReportDescriptor(uint8_t *data, int length, uint8_t ifaceNum);
    virtual void onReceive(const usb_transfer_t *transfer) {};
    virtual void onGone(const usb_host_client_event_msg_t *eventMsg) {};
    virtual void onMouseButtons(hid_mouse_report_t report, uint8_t last_buttons);
    virtual void onMouseMove(hid_mouse_report_t report);

    // 键盘报文。
    //
    // modifiers: 8 个修饰位(bit0=LCtrl .. bit7=RGui), 与 USB HID 定义一致。
    // keys[6]:   当前按下的键码(HID Usage ID), 0 表示空槽。
    //
    // 采用"快照"而非"差分": 键盘协议本身就是把当前所有按下的键完整报一遍,
    // 所以设备侧直接按快照重建状态即可, 不需要在主机侧算按下/抬起。
    // 这样做的好处是丢包不会导致按键永久卡住 —— 下一帧快照会自然纠正。
    struct KeyboardReport {
        uint8_t modifiers;
        uint8_t keys[6];
        uint8_t keyCount;   // 实际使用的键码槽数(标准键盘=6)
    };
    virtual void onKeyboard(const KeyboardReport &report);
    void receiveSerial0(void *command);
    void logRawBytes(const char *functionName, const uint8_t *data, uint16_t length);
    void cleanupTask(void *arg);
    void moveFmtTick();

    // ===== 瞬时屏蔽真实输入 =====
    //
    // 在指定时长内丢弃真实键鼠输入, 让被控机只收到软体注入的信号。
    //
    // 安全设计(三条都必须有, 缺一个就会出事故):
    //  1. 硬超时: 到时自动解除。软体崩溃/忘记发解除命令也不会让鼠标永久失灵。
    //     maskTick() 由主循环定期调用, 是唯一的解除保证。
    //  2. 上限钳制: 请求时长超过 kMaskMaxMs 一律按上限处理, 防止软体发一个
    //     极大值把设备"焊死"在屏蔽态。
    //  3. 恢复时补发抬键: 屏蔽期间如果用户正按着某个键/鼠标键, 恢复时必须
    //     补一个"抬起", 否则被控机侧该键永久卡住(游戏里表现为人物一直前进)。
    //     注意鼠标位移无需补偿(相对位移, 丢弃即等于没动)。
    enum : uint32_t { kMaskMaxMs = 2000, kMaskDefaultMs = 50 };

    void maskStart(uint32_t durationMs);
    void maskStop();
    void maskTick();                      // 主循环调用: 处理超时恢复

    // 键盘链路维护 (1ms 任务里调用, 每 50ms 实际动作一次)。做两件事:
    //
    //   1) 发心跳: 0x23 帧 + payload=[0xFF], 【不携带按键状态】。
    //      左板靠它区分"链路断了"与"用户一直按着", 才能安全地做卡键兜底。
    //      ★ 心跳绝不能携带状态 —— 那会让"链路活着"的判据也依赖状态帧,
    //        两者就纠缠在一起了。
    //
    //   2) 重申当前键盘状态: 兜住 Serial1(无重传的普通串口)偶发丢帧。
    //      这是安全的, 因为右板的状态来自 USB(可靠总线, NAK/ACK 重试),
    //      记的就是设备的真实状态, 重申等于把真相再说一遍。
    //
    //   屏蔽窗口内跳过 (2) 但保留 (1)。
    void kbdLinkTick();
    void serial1SendKeyboardHeartbeat();

    // maskUntilMs 横跨两条任务: maskStart 在 RxTaskSerial0(km.mask 命令),
    // maskTick/maskStop 在 RxTaskSerial1(硬超时)。原实现只靠 volatile, 而
    // volatile 不提供原子性也不提供互斥 => maskStart 的"检查是否已激活"与
    // "写入新时刻"之间存在 check-then-act 窗口, 且 maskStop 尾段的
    // `maskUntilMs = 0` 会覆盖掉 maskStart 刚写下的值(屏蔽被静默取消)。
    // 本互斥量串行化这两个函数整体, 并保护 maskLast* 快照组。
    SemaphoreHandle_t s_mask_mtx = nullptr;
    bool maskIsActive() const { return maskUntilMs != 0; }

    volatile uint32_t maskUntilMs = 0;    // 0 = 未屏蔽; 否则=解除时刻(millis)
    uint8_t maskLastButtons = 0;          // 屏蔽前最后一次鼠标按键状态
    uint8_t maskLastModifiers = 0;        // 屏蔽前最后一次键盘修饰键
    uint8_t maskLastKeys[6] = {0};        // 屏蔽前最后一次键盘键码
    bool    maskHeldStateValid = false;   // maskLast* 是否有效

    // 真键盘"当前按键状态"的影子副本。
    //
    // 屏蔽功能需要它: 进入屏蔽时要记下设备侧此刻认为按着什么, 才能在恢复时
    // 补发释放。lastReportedButtons 已经担任鼠标的同一角色, 这里补上键盘的。
    // 由 onKeyboard() 在每次真正转发时更新。
    uint8_t kbdLastModifiers = 0;
    uint8_t kbdLastKeys[6] = {0};

    // 键盘快照去重缓存。
    //
    // 用成员而非函数内 static, 因为 maskStop() 必须能把它作废: 屏蔽期间键盘
    // 状态变化被丢弃, 若用户始终按着同一个键, 屏蔽结束后那一帧内容与缓存相同
    // 就会被去重逻辑丢弃 —— 于是设备侧永远收不到"这个键还按着", 表现为
    // 屏蔽后按键失灵, 直到用户松手重按。
    uint8_t kbdDedupModifiers = 0xFF;
    uint8_t kbdDedupKeys[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t kbdDedupCount = 0xFF;

    void sendDeviceInfo();
    void sendDescriptorDevice();
    void sendRawHidDescriptors();
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
