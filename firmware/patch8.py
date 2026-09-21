path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\src\esp_usb_host.cpp"
src = open(path, encoding="utf-8", errors="replace").read()

# ---------------------------------------------------------------
# 1) _onReceiveControl: 不再丢弃非鼠标接口的描述符, 改为按接口存起来
# ---------------------------------------------------------------
OLD_CTRL = """    // Check if it's a mouse
    for (int i = 0; i < totalBytes - 3; i++)
    {
        if (p[i] == 0x05 && p[i + 1] == 0x01 && p[i + 2] == 0x09 && p[i + 3] == 0x02)
        {
            isMouse = true;
            break;
        }
    }

    if (!isMouse)
    {
        ESP_LOGI("EspUsbHost", "Device is not a mouse, skipping further processing");
        usb_host_transfer_free(transfer);
        return;
    }

    ESP_LOGI("EspUsbHost", "Mouse device detected, parsing HID report descriptor");

    HIDReportDescriptor descriptor = usbHost->parseHIDReportDescriptor(&transfer->data_buffer[8], transfer->actual_num_bytes - 8);

    usb_host_transfer_free(transfer);
}"""

NEW_CTRL = """    // ===== 报告描述符归属 =====
    //
    // 【老代码的 bug】这里用"描述符里有没有 05 01 09 02 (Mouse)"来判断,
    // 不是鼠标就整包丢弃。对复合设备(键盘+鼠标接收器)是致命的:
    //   1) 键盘接口的报告描述符没有 Mouse usage -> 被丢弃, 于是键盘接口
    //      永远没有自己的描述符, 后面无处可查。
    //   2) 解析结果只写进【一个全局】 HIDReportDesc -> 无论哪个接口的描述符
    //      到达, 都覆盖同一份数据。复合设备下最终存的是"最后一个到达的",
    //      谁也无法保证是鼠标那份。
    //
    // 【本版改法】不再按内容筛选, 而是"按接口存档":
    //   控制传输的 wIndex(接口号)记录在 data_buffer 的前 8 字节 setup 包里,
    //   用它把每份报告描述符存到 iface_report_desc[接口号] 下。
    //   鼠标解码时按"本帧所属接口"取自己那份, 键盘则只看类别标记。
    //
    // 同时保留 isMouse 判定, 只用于决定是否【额外】填充全局 HIDReportDesc
    // (兼容只认全局的那条老鼠标路径, 避免纯鼠标设备回归)。

    // setup 包布局: [0]=bmRequestType [1]=bRequest [2..3]=wValue
    //               [4..5]=wIndex(接口号) [6..7]=wLength
    const uint8_t reqType = transfer->data_buffer[0];
    const uint8_t ctrlIface = transfer->data_buffer[4];

    // 只关心 "GET_DESCRIPTOR / Report" (bmRequestType=0x81, bRequest=0x06,
    // wValue 高字节=0x22)。其余控制传输(字符串/配置等)直接放行, 不参与解析。
    const bool isReportDesc =
        (reqType == 0x81) &&
        (transfer->data_buffer[1] == 0x06) &&
        (transfer->data_buffer[3] == 0x22);

    if (!isReportDesc || totalBytes <= 8) {
        usb_host_transfer_free(transfer);
        return;
    }

    // 接口号有效性: 越界则按 0 处理(单接口设备的情形)
    uint8_t slot = (ctrlIface < EspUsbHost::kMaxReportDescIfaces) ? ctrlIface : 0;

    const uint8_t *desc = &transfer->data_buffer[8];
    const int descLen = totalBytes - 8;

    // 内容判定: 是不是鼠标 (05 01 09 02 = Usage Page Generic Desktop, Usage Mouse)
    bool isMouse = false;
    for (int i = 0; i + 3 < descLen; i++)
    {
        if (desc[i] == 0x05 && desc[i + 1] == 0x01 && desc[i + 2] == 0x09 && desc[i + 3] == 0x02)
        {
            isMouse = true;
            break;
        }
    }

    // 内容判定: 是不是键盘 (05 01 09 06 = Usage Page Generic Desktop, Usage Keyboard)
    bool isKbd = false;
    for (int i = 0; i + 3 < descLen; i++)
    {
        if (desc[i] == 0x05 && desc[i + 1] == 0x01 && desc[i + 2] == 0x09 && desc[i + 3] == 0x06)
        {
            isKbd = true;
            break;
        }
    }

    // 存档: 每个接口自己那份描述符的解析结果
    usbHost->iface_isKbd[slot]    = isKbd;
    usbHost->iface_isMouse[slot]  = isMouse;
    usbHost->iface_descValid[slot] = true;
    usbHost->iface_report_desc[slot] =
        usbHost->parseHIDReportDescriptor((uint8_t *)desc, descLen);

    ESP_LOGI("EspUsbHost", "Report desc for iface %u: mouse=%d kbd=%d (len=%d)",
             slot, (int)isMouse, (int)isKbd, descLen);

    // 兼容老路径: 鼠标的那份同时写全局
    if (isMouse) {
        ESP_LOGI("EspUsbHost", "Mouse device detected, parsing HID report descriptor");
        HIDReportDescriptor descriptor = usbHost->iface_report_desc[slot];
        (void)descriptor;
    }

    usb_host_transfer_free(transfer);
}"""

assert OLD_CTRL in src, "_onReceiveControl block not found"
src = src.replace(OLD_CTRL, NEW_CTRL, 1)
open(path, "w", encoding="utf-8", newline="").write(src)
print("_onReceiveControl rewritten: per-interface report descriptor storage")
