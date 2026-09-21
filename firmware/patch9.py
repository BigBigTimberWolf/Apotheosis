path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\include\EspUsbHost.h"
src = open(path, encoding="utf-8", errors="replace").read()

anchor = "    static struct HIDReportDescriptor HIDReportDesc;"
assert anchor in src, "HIDReportDesc anchor not found"

addition = anchor + r'''

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
    bool iface_isKbd[kMaxReportDescIfaces];       // 描述符里是否出现 Keyboard usage'''

src = src.replace(anchor, addition, 1)
open(path, "w", encoding="utf-8", newline="").write(src)
print("per-interface report descriptor storage added to header")
