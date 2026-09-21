import io

# ============================================================
# 第 1 步: 右板 —— 在接口列表 JSON 里带上"该接口是鼠标还是键盘"
# ============================================================
p1 = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\src\serialization.cpp"
s1 = open(p1, encoding="utf-8", errors="replace").read()

OLD1 = """        desc["bInterfaceProtocol"] = interface_descriptors[i].bInterfaceProtocol;
        desc["iInterface"] = interface_descriptors[i].iInterface;
    }"""
NEW1 = """        desc["bInterfaceProtocol"] = interface_descriptors[i].bInterfaceProtocol;

        // ===== 附带该接口的 HID 类型 (鼠标 / 键盘) =====
        //
        // 【为什么必须传这个】左板需要据此决定"暴露成鼠标还是键盘"。
        // 但真设备的 bInterfaceProtocol 常常是 0(实测: 键盘接收器与鼠标接收器
        // 都是 Class_03 SubClass_00 Prot_00, 完全无法区分), 因此不能只看 protocol。
        //
        // 这里附带的是右板【解析真设备报告描述符内容】得到的结论 ——
        // 依据是描述符里出现的 Usage(0x01/0x02 = 鼠标, 0x01/0x06 = 键盘),
        // 这才是可靠判据。
        desc["isKbd"]   = iface_isKbd[i];
        desc["isMouse"] = iface_isMouse[i];

        desc["iInterface"] = interface_descriptors[i].iInterface;
    }"""

assert OLD1 in s1, "serialization anchor not found"
s1 = s1.replace(OLD1, NEW1, 1)
open(p1, "w", encoding="utf-8", newline="").write(s1)
print("[1/3] serialization.cpp: isKbd/isMouse 已加入接口列表 JSON")

# ============================================================
# 第 2 步: 左板 —— 接收这两个字段并存起来
# ============================================================
p2 = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\src\InitSettings.cpp"
s2 = open(p2, encoding="utf-8", errors="replace").read()

lines = s2.split("\n")
# 找到 receiveInterfaceDescriptors 里写 bInterfaceProtocol 的那行
idx = None
for i, ln in enumerate(lines):
    if "interface_descriptors[interfaceCounter].bInterfaceProtocol" in ln and "obj[" in ln:
        idx = i
        break
assert idx is not None, "left receiveInterfaceDescriptors bInterfaceProtocol line not found"

lines.insert(idx + 1, """        // 该接口的 HID 类型(由右板解析真设备报告描述符得出)。
        // 真设备的 bInterfaceProtocol 常为 0, 无法区分鼠标/键盘, 因此以这两个
        // 标记为准来决定本板暴露成什么设备(见 USBSetup.cpp 的 InitUSB)。
        iface_isKbd[interfaceCounter]   = obj["isKbd"]   | false;
        iface_isMouse[interfaceCounter] = obj["isMouse"] | false;""")

s2 = "\n".join(lines)
open(p2, "w", encoding="utf-8", newline="").write(s2)
print("[2/3] InitSettings.cpp: 左板已接收 isKbd/isMouse")
