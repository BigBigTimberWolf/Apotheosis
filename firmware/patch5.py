path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\src\esp_usb_host.cpp"
src = open(path, encoding="utf-8", errors="replace").read()

OLD = """    // 本帧来自哪个接口?
    //
    // 判据: 槽位记录的 bInterfaceNumber 必须等于槽位下标(说明它是
    // "接口号槽位"而非"端点号槽位"), 且 class == HID。
    // 这样天然排除了 onConfig() 复制到端点号槽位的那份副本 ——
    // 正是那份副本导致老代码重复注入。
    int8_t ifaceOfFrame = -1;
    for (int i = 0; i < 16; i++)
    {
        const auto &e = usbHost->endpoint_data_list[i];
        if (e.bInterfaceClass != USB_CLASS_HID) continue;
        if (e.bInterfaceNumber != i) continue;      // 只认"接口号槽位"
        ifaceOfFrame = (int8_t)i;
        break;
    }
"""

NEW = """    // 本帧来自哪个接口?
    //
    // ★ 这里必须按【端点】定位, 不能"取第一个 HID 接口"。
    //
    // 【上一版的 bug】原实现是"遍历 16 个槽位, 取第一个 class==HID 且
    // bInterfaceNumber == 下标 的槽位"。这在纯鼠标上碰巧能用, 但对
    // "键盘接收器"这类设备会彻底失效, 原因是:
    //   1) 它忽略了 transfer->bEndpointAddress —— 帧到底从哪个端点来,
    //      代码根本没看。多个 HID 接口时永远只会命中同一个接口。
    //   2) 判据 bInterfaceNumber == 下标 只是"槽位是接口号索引"的弱证据,
    //      接口 0 被鼠标占用时, 键盘接口(接口 1/2)永远轮不到。
    //   结果: 键盘接口的报文被拿去当【鼠标】解码, 键盘静默失效
    //        (而 flashLED() 在更前面, 所以灯照样闪 —— 现象与实测完全一致)。
    //
    // 【本版改法】用端点地址反查接口:
    //   onConfig 已经为每个 IN 端点填好了 ep_owner[epSlot(addr)].iface,
    //   那是"这个端点属于哪个接口"的权威记录。按它定位才正确。
    //
    // epSlot()/ep_owner 的定义见 EspUsbHost.h。
    int8_t ifaceOfFrame = -1;

    const uint8_t epSlotIdx = epSlot(transfer->bEndpointAddress);
    if (epSlotIdx < EspUsbHost::kEpSlotCount && usbHost->ep_owner[epSlotIdx].used) {
        ifaceOfFrame = (int8_t)usbHost->ep_owner[epSlotIdx].iface;
    }

    // 兜底: 老版本里端点如果没被 ep_owner 记录(或表未初始化), 再退回
    // "第一个 HID 接口"的老做法, 保证纯鼠标设备不至于因此失效。
    if (ifaceOfFrame < 0) {
        for (int i = 0; i < 16; i++) {
            const auto &e = usbHost->endpoint_data_list[i];
            if (e.bInterfaceClass != USB_CLASS_HID) continue;
            if (e.bInterfaceNumber != i) continue;
            ifaceOfFrame = (int8_t)i;
            break;
        }
    }
"""

assert OLD in src, "old interface-detection block not found"
src = src.replace(OLD, NEW, 1)
open(path, "w", encoding="utf-8", newline="").write(src)
print("interface detection rewritten to use ep_owner")
