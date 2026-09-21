path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\src\USBSetup.cpp"
src = open(path, encoding="utf-8", errors="replace").read()

OLD = """    Mouse.begin();
    // 键盘必须也 begin(): handleCommands.cpp 里 0x21 KEY_MASK / 0x22 KEY_TAP 两条
    // 路径都会调用 Kbd.press()/Kbd.release(), 而 USBHIDKeyboard 只有 begin() 之后
    // 才会把自己的 HID 接口与报告描述符注册到 TinyUSB。原来这里只 begin 了 Mouse,
    // 于是键盘接口从未被枚举 —— Kbd.press() 写进的是没人读的缓冲区, 按键注入静默失效。
    // CFG_TUD_HID 已配成 2(鼠标+键盘), 足以支撑两个 HID 接口。
    Kbd.begin();
"""

NEW = """    // ===== 只暴露真设备【实际拥有】的那一类 HID =====
    //
    // 【老代码的问题】这里无条件 begin 了 Mouse 和 Kbd 两个, 于是被控机看到的
    // 永远是一个"鼠标+键盘"的复合设备 —— 无论真设备插的是鼠标还是键盘:
    //     插真鼠标 -> 被控机看到 鼠标 + 键盘   (多出一个不该有的键盘)
    //     插真键盘 -> 被控机看到 鼠标 + 键盘   (多出一个不该有的鼠标)
    // 这既不符合真设备的描述符结构(影响驱动识别/身份一致性), 也让
    // "插什么就克隆什么"这个目标落空。
    //
    // 【本版改法】按真设备接口列表决定 begin 谁:
    //   Class 0x03 (HID) + bInterfaceProtocol:
    //       0x02 -> 鼠标(Boot Mouse)
    //       0x01 -> 键盘(Boot Keyboard)
    //   只看【第一个】HID 接口的 protocol 作为主功能判据 —— 不能扫"有没有",
    //   因为键盘接收器这类设备内部常附带一个鼠标接口(键鼠一体), 扫"有没有"
    //   会导致插键盘也暴露出鼠标。
    //
    // 【时机】InitUSB() 在握手最后(10 条命令全部完成后)才被调用, 此时
    // interface_descriptors[] 与真设备报告描述符都已经到齐, 因此判断有效。
    //
    // 【兜底】识别不出来时按老行为两个都 begin —— 宁可多暴露, 也不要整个
    // 设备失效。这样任何未知设备至少保持原有可用性。

    // 判据 1: 真设备接口列表里第一个 HID 接口的 protocol
    int8_t primaryProto = -1;
    for (int i = 0; i < interfaceCounter; i++) {
        if (interface_descriptors[i].bInterfaceClass != 0x03) continue;  // 只关心 HID
        primaryProto = (int8_t)interface_descriptors[i].bInterfaceProtocol;
        break;
    }

    // 判据 2(优先): 真设备报告描述符解析出的类型 —— 这个更可靠, 因为它看的是
    // 报告描述符内容(Usage), 不受厂商是否填对 bInterfaceProtocol 影响。
    const bool realKbd   = isCloneKbdActive();
    const bool realMouse = isCloneMouseActive();

    bool wantMouse = false;
    bool wantKbd   = false;

    if (realKbd || realMouse) {
        // 报告描述符到了: 以它为准。
        // 注意 realKbd 优先 —— 键盘接收器可能同时有鼠标描述符, 而用户的意图
        // 是"这个口接键盘", 因此键盘存在时只暴露键盘。
        if (realKbd)       { wantKbd   = true; }
        else if (realMouse){ wantMouse = true; }
    } else if (primaryProto == 0x01) {
        wantKbd = true;                       // 接口列表说是键盘
    } else if (primaryProto == 0x02) {
        wantMouse = true;                     // 接口列表说是鼠标
    } else {
        // 兜底: 两者都认不出来 -> 保持老行为
        wantMouse = true;
        wantKbd   = true;
    }

    Serial0.printf("[USB] exposing: mouse=%d kbd=%d (realMouse=%d realKbd=%d primaryProto=%d)\\n",
                   (int)wantMouse, (int)wantKbd, (int)realMouse, (int)realKbd, (int)primaryProto);

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

assert OLD in src, "Mouse.begin()/Kbd.begin() block not found"
src = src.replace(OLD, NEW, 1)
open(path, "w", encoding="utf-8", newline="").write(src)
print("InitUSB now conditionally exposes mouse/keyboard")
