path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\src\USBSetup.cpp"
src = open(path, encoding="utf-8", errors="replace").read()

start = src.find("    // 判据 1: 真设备接口列表里第一个 HID 接口的 protocol")
end   = src.find("    // 克隆实例的注册必须在这里做完")
assert start > 0 and end > start, "decision block bounds not found"

NEW = """    // ===== 判据来源 =====
    //
    // 这些标记由右板解析【真设备的 HID 报告描述符内容】得出, 随接口列表 JSON
    // 一起传来(见 fw_host/serialization.cpp 与 InitSettings.cpp)。
    //
    // 【为什么不能用 bInterfaceProtocol】实测: 键盘接收器(VID_3554)与鼠标接收器
    // (VID_249A)报的都是 Class_03 SubClass_00 Prot_00 —— 完全一样, 无法区分。
    // 唯一可靠的依据是报告描述符里的 Usage:
    //     Usage Page 0x01 + Usage 0x02 -> 鼠标
    //     Usage Page 0x01 + Usage 0x06 -> 键盘
    bool realKbd   = false;
    bool realMouse = false;
    for (int i = 0; i < interfaceCounter && i < MAX_IFACE_TYPE; i++) {
        if (iface_isKbd[i])   realKbd   = true;
        if (iface_isMouse[i]) realMouse = true;
    }

    // 兜底判据: 报告描述符标记一个都没有时, 退回看接口 protocol。
    // (例如描述符请求失败、或设备不是标准 HID 的情况)
    int8_t primaryProto = -1;
    if (!realKbd && !realMouse) {
        for (int i = 0; i < interfaceCounter; i++) {
            if (interface_descriptors[i].bInterfaceClass != 0x03) continue;
            primaryProto = (int8_t)interface_descriptors[i].bInterfaceProtocol;
            break;
        }
    }

    bool wantMouse = false;
    bool wantKbd   = false;

    if (realKbd) {
        // 键盘优先 —— 键盘接收器常同时带一个鼠标接口(键鼠一体), 而本口接的是
        // 键盘, 因此只要检测到键盘就【只暴露键盘】, 不去看那个附带的鼠标接口。
        wantKbd = true;
    } else if (realMouse) {
        wantMouse = true;
    } else if (primaryProto == 0x01) {
        wantKbd = true;
    } else if (primaryProto == 0x02) {
        wantMouse = true;
    } else {
        // 什么都认不出来 -> 保持老行为(两个都暴露), 宁可多暴露也不让设备失效。
        wantMouse = true;
        wantKbd   = true;
    }

    Serial0.printf("[USB] exposing: mouse=%d kbd=%d (realMouse=%d realKbd=%d proto=%d ifaces=%u)\\n",
                   (int)wantMouse, (int)wantKbd, (int)realMouse, (int)realKbd,
                   (int)primaryProto, (unsigned)interfaceCounter);

    if (wantMouse) {
        Mouse.begin();
    }
    if (wantKbd) {
        // 键盘必须 begin(): handleCommands.cpp 里 0x21 KEY_MASK / 0x22 KEY_TAP 两条
        // 路径都会调用 Kbd.press()/Kbd.release(), 而 USBHIDKeyboard 只有 begin()
        // 之后才会把自己的报告描述符注册到 TinyUSB。
        // CFG_TUD_HID 已配成 2(鼠标+键盘), 足以支撑两个 HID 实例。
        Kbd.begin();
    }

"""

src = src[:start] + NEW + src[end:]
open(path, "w", encoding="utf-8", newline="").write(src)
print("InitUSB decision logic replaced with descriptor-based flags")
