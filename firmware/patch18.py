path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\src\USBSetup.cpp"
src = open(path, encoding="utf-8", errors="replace").read()

OLD = """    if (wantMouse) {
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

NEW = """    // ===== 注册顺序至关重要 (Arduino 框架的硬限制) =====
    //
    // 【框架限制】USBHID::SendReport() 里是:
    //     res = tud_hid_n_report(0, id, data, len);
    //                         ^ 实例号【硬编码为 0】
    // 也就是说 SendReport 只会往"第 0 个 HID 实例"写报文, 它假定设备里
    // 只有一个 HID 实例。而实例号由 addDevice() 的调用顺序决定:
    //     第 1 个 begin 的 = 实例 0, 第 2 个 = 实例 1 ...
    //
    // 【老代码的 bug】原来是 Mouse.begin() 在前 -> 鼠标=实例0, 键盘=实例1。
    // 于是 Kbd.press() 的报文被写进【鼠标实例】, 键盘永远收不到任何输入
    // (症状: 键盘设备枚举正常、驱动已绑定, 但打字完全无反应)。
    //
    // 【本版做法】
    //   1) 只暴露真设备实际拥有的那一类(见上面的判据), 正常情况下只会
    //      begin 一个, 它自然就是实例 0, SendReport 命中正确。
    //   2) 万一两个都要 begin(兜底分支), 则让【键盘先注册】, 保证键盘是
    //      实例 0 —— 因为键盘没有别的发送通道, 而鼠标位移在本固件里走的是
    //      ASCII 命令(km.move)与独立协议, 不依赖 HID 实例 0 的编号。
    //      这样至少让"更难修的那个"先拿到实例 0。

    if (wantKbd) {
        // 键盘必须 begin(): handleCommands.cpp 里 0x21 KEY_MASK / 0x22 KEY_TAP 两条
        // 路径都会调用 Kbd.press()/Kbd.release(), 而 USBHIDKeyboard 只有 begin()
        // 之后才会把自己的报告描述符注册到 TinyUSB。
        // CFG_TUD_HID 已配成 2(鼠标+键盘), 足以支撑两个 HID 实例。
        Kbd.begin();
    }
    if (wantMouse) {
        Mouse.begin();
    }
"""

assert OLD in src, "begin block not found"
src = src.replace(OLD, NEW, 1)
open(path, "w", encoding="utf-8", newline="").write(src)
print("begin order changed: Kbd first (becomes instance 0)")
