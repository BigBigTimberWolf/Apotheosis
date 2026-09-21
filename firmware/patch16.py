# 左板: 新增全局 iface_isKbd / iface_isMouse 数组
p_h = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\include\InitSettings.h"
s = open(p_h, encoding="utf-8", errors="replace").read()

anchor = "extern uint8_t interfaceCounter;"
assert anchor in s, "interfaceCounter extern not found"

if "iface_isKbd" not in s:
    s = s.replace(anchor, anchor + """

// 每个接口的 HID 类型标记(由右板解析真设备报告描述符得出, 随接口列表 JSON 传来)。
// 用途: InitUSB 据此决定本板暴露成鼠标还是键盘。
// 为什么需要: 真设备的 bInterfaceProtocol 实测常为 0x00(键盘接收器与鼠标接收器
// 都是 Class_03 SubClass_00 Prot_00), 无法据此区分, 只能看报告描述符内容。
#define MAX_IFACE_TYPE 16
extern bool iface_isKbd[MAX_IFACE_TYPE];
extern bool iface_isMouse[MAX_IFACE_TYPE];""", 1)
    open(p_h, "w", encoding="utf-8", newline="").write(s)
    print("[3/3] InitSettings.h: 已声明 iface_isKbd / iface_isMouse")
else:
    print("[3/3] 已存在, 跳过")

# 在 InitSettings.cpp 里定义
p_c = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\src\InitSettings.cpp"
s2 = open(p_c, encoding="utf-8", errors="replace").read()

if "bool iface_isKbd[" not in s2:
    anc2 = "uint8_t interfaceCounter = 0;"
    if anc2 not in s2:
        # 找 interfaceCounter 的定义
        import re
        m = re.search(r"^uint8_t\s+interfaceCounter[^\n]*;", s2, re.M)
        assert m, "interfaceCounter definition not found"
        anc2 = m.group(0)
    s2 = s2.replace(anc2, anc2 + """

// 每个接口的 HID 类型标记(见 InitSettings.h 说明)
bool iface_isKbd[MAX_IFACE_TYPE]   = {false};
bool iface_isMouse[MAX_IFACE_TYPE] = {false};""", 1)
    open(p_c, "w", encoding="utf-8", newline="").write(s2)
    print("     InitSettings.cpp: 已定义数组")
else:
    print("     已定义, 跳过")

# 确认 index 不越界
idx = None
for i, ln in enumerate(s2.split("\n")):
    if "iface_isKbd[interfaceCounter]" in ln:
        idx = i
        break
print("     写入点行号:", idx + 1 if idx else "未找到")
