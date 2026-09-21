path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\src\esp_usb_host.cpp"
src = open(path, encoding="utf-8", errors="replace").read()

OLD = """    const bool ifaceValid    = (ifaceOfFrame >= 0);
    const uint8_t ifaceProto = ifaceValid
                                 ? usbHost->endpoint_data_list[ifaceOfFrame].bInterfaceProtocol
                                 : (uint8_t)HID_ITF_PROTOCOL_NONE;

    // 键盘接口: protocol == KEYBOARD(1)
    const bool ifaceIsKeyboard = ifaceValid && (ifaceProto == HID_ITF_PROTOCOL_KEYBOARD);

    // 指针接口: protocol == MOUSE(2) 或 NONE(0)
    // 注意必须【排除键盘】—— 键盘接口的 protocol 也常为 NONE, 不排除的话
    // 键盘报文会被当成位移注入(老代码就有这个风险)。
    const bool ifaceLooksMouse = ifaceValid && !ifaceIsKeyboard &&
                                 (ifaceProto == HID_ITF_PROTOCOL_MOUSE ||
                                  ifaceProto == HID_ITF_PROTOCOL_NONE);
"""

NEW = """    const bool ifaceValid    = (ifaceOfFrame >= 0);
    const uint8_t ifaceProto = ifaceValid
                                 ? usbHost->endpoint_data_list[ifaceOfFrame].bInterfaceProtocol
                                 : (uint8_t)HID_ITF_PROTOCOL_NONE;

    // ===== 接口类别判定: 以【报告描述符内容】为准, protocol 只作兜底 =====
    //
    // 【为什么不能只看 bInterfaceProtocol —— 实测踩到的坑】
    // 2.4G 接收器这类设备的键盘接口常常报:
    //     Class_03 (HID)  SubClass_00  Prot_00      <- protocol = NONE, 不是 1!
    // (键盘接收器 VID_3554:PID_FA09 就是如此, 其键盘接口为 MI_00/Prot_00)
    // 于是 `proto == HID_ITF_PROTOCOL_KEYBOARD` 判据永远不成立, 键盘报文
    // 被判成鼠标 -> 拿鼠标的布局去解码 -> 键盘完全失效。
    //
    // 正确判据是直接看该接口的报告描述符里出现了哪个 Usage:
    //     Usage Page 0x01 + Usage 0x02 -> Mouse
    //     Usage Page 0x01 + Usage 0x06 -> Keyboard
    // 这在 _onReceiveControl 里已经按接口解析并记录 (iface_isMouse/iface_isKbd)。
    //
    // protocol 仅在描述符尚未到达时兜底, 保证"描述符没解析出来"也不会
    // 让纯鼠标设备失效。
    bool descIsKbd   = false;
    bool descIsMouse = false;
    bool descKnown   = false;
    if (ifaceValid && ifaceOfFrame < EspUsbHost::kMaxReportDescIfaces) {
        if (usbHost->iface_descValid[ifaceOfFrame]) {
            descKnown   = true;
            descIsKbd   = usbHost->iface_isKbd[ifaceOfFrame];
            descIsMouse = usbHost->iface_isMouse[ifaceOfFrame];
        }
    }

    // 键盘接口
    const bool ifaceIsKeyboard = ifaceValid &&
        (descKnown ? descIsKbd : (ifaceProto == HID_ITF_PROTOCOL_KEYBOARD));

    // 指针接口: 排除键盘后, 描述符说是鼠标 / 或 protocol 为 MOUSE|NONE。
    // 注意【必须排除键盘】—— 很多键盘接口 protocol 也是 NONE,
    // 不排除的话键盘报文会被当位移注入。
    const bool ifaceLooksMouse = ifaceValid && !ifaceIsKeyboard &&
        (descKnown ? descIsMouse
                   : (ifaceProto == HID_ITF_PROTOCOL_MOUSE ||
                      ifaceProto == HID_ITF_PROTOCOL_NONE));
"""

assert OLD in src, "protocol-based classification block not found"
src = src.replace(OLD, NEW, 1)
open(path, "w", encoding="utf-8", newline="").write(src)
print("interface classification now descriptor-based")
