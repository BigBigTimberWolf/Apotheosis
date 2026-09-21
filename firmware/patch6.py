path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\include\EspUsbHost.h"
src = open(path, encoding="utf-8", errors="replace").read()

anchor = "    #define MAX_HID_DESCRIPTORS 10 "
assert anchor in src, "anchor MAX_HID_DESCRIPTORS not found"

addition = r'''    // ===== 端点归属表 (复合 HID 设备透传的基础设施) =====
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

    #define MAX_HID_DESCRIPTORS 10 '''

src = src.replace(anchor, addition, 1)
open(path, "w", encoding="utf-8", newline="").write(src)
print("ep_owner table added to header")
