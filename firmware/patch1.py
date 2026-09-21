import io

path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\src\esp_usb_host.cpp"
src = open(path, encoding="utf-8", errors="replace").read()
lines = src.split("\n")

start = next(i for i, l in enumerate(lines)
             if "// Process the HID report if it's a mouse report" in l)
end = next(j for j in range(start, len(lines)) if "// Handle transfer status" in lines[j])

NEW = r'''    // ===== 复合 HID 设备透传 =====
    //
    // 【设计原则】能透传的都尽量原样复制透传; 传输层特性(轮询间隔、刷新率、
    // 时序)不透传 —— 那些由设备侧自己决定, 复制它们没有意义。
    //
    // 一个复合 HID 设备(鼠标 + 键盘 + 多媒体键 + 厂商自定义)有【多个接口】,
    // 每个接口有【自己的 IN 端点】和【自己的报告描述符】。正确做法是:
    // 先由"本帧来自哪个端点"定位它属于哪个接口, 再用【那个接口自己的
    // 描述符】解码 —— 而不是像老代码那样拿一份全局描述符去套所有端点。
    //
    // 【老代码的 bug】它循环遍历 16 个槽位, 对每个匹配
    // (class==HID && protocol ∈ {MOUSE,NONE}) 的槽位都解码一遍并发送。
    // 而 onConfig() 把同一接口信息复制到了两处:
    //     endpoint_data_list[currentInterfaceNumber]   (按接口号)
    //     endpoint_data_list[ep_num]                   (按端点号)
    // 于是同一接口至少被匹配 2 次 -> 位移被重复注入 -> 移动量放大数倍,
    // 快速移动时重复帧在 5Mbps 链路上堆叠 -> 卡顿累积。
    //
    // 【本次改法】先由端点地址定位"本帧唯一的接口", 再按该接口的类别
    // 分派: 鼠标 -> 解码位移; 键盘 -> 透传键码快照。两者互不干扰, 因此
    // "鼠标+键盘"复合设备的两条通路都能各自正确工作。

    // 本帧来自哪个接口?
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

    const bool ifaceValid    = (ifaceOfFrame >= 0);
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

    // ---------------------------------------------------------------
    // 1) 指针类接口 -> 解码位移并透传
    // ---------------------------------------------------------------
    if (ifaceLooksMouse)
    {
        // Report ID 匹配过滤: 若报告描述符含 Report ID, 报文首字节必须匹配。
        // 注意 has_data 检查: 零字节帧没有 data_buffer[0] 可读。
        const bool idOk =
            (usbHost->HIDReportDesc.reportId == 0) ||
            (has_data && transfer->data_buffer[0] == usbHost->HIDReportDesc.reportId);

        if (idOk)
        {
            static uint8_t last_buttons = 0;
            hid_mouse_report_t report = {};
            report.buttons = transfer->data_buffer[usbHost->HIDReportDesc.buttonStartByte];

            if (usbHost->HIDReportDesc.xAxisSize == 12 && usbHost->HIDReportDesc.yAxisSize == 12)
            {
                uint8_t xyOffset = usbHost->HIDReportDesc.xAxisStartByte;
                int16_t xValue = (transfer->data_buffer[xyOffset]) |
                                 ((transfer->data_buffer[xyOffset + 1] & 0x0F) << 8);
                int16_t yValue = ((transfer->data_buffer[xyOffset + 1] >> 4) & 0x0F) |
                                 (transfer->data_buffer[xyOffset + 2] << 4);

                report.x = xValue;
                report.y = yValue;
                uint8_t wheelOffset = usbHost->HIDReportDesc.wheelStartByte;
                report.wheel = transfer->data_buffer[wheelOffset];
            }
            else if (usbHost->HIDReportDesc.xAxisSize == 16 && usbHost->HIDReportDesc.yAxisSize == 16)
            {
                uint8_t xOffset = usbHost->HIDReportDesc.xAxisStartByte;
                uint8_t yOffset = usbHost->HIDReportDesc.yAxisStartByte;
                uint8_t wheelOffset = usbHost->HIDReportDesc.wheelStartByte;

                int16_t xValue = (int16_t)((uint16_t)transfer->data_buffer[xOffset] | ((uint16_t)transfer->data_buffer[xOffset + 1] << 8));
                int16_t yValue = (int16_t)((uint16_t)transfer->data_buffer[yOffset] | ((uint16_t)transfer->data_buffer[yOffset + 1] << 8));

                report.x = xValue;
                report.y = yValue;
                report.wheel = (int8_t)transfer->data_buffer[wheelOffset];
            }
            else
            {
                uint8_t xOffset = usbHost->HIDReportDesc.xAxisStartByte;
                uint8_t yOffset = usbHost->HIDReportDesc.yAxisStartByte;
                uint8_t wheelOffset = usbHost->HIDReportDesc.wheelStartByte;

                report.x = (int8_t)transfer->data_buffer[xOffset];
                report.y = (int8_t)transfer->data_buffer[yOffset];
                report.wheel = (int8_t)transfer->data_buffer[wheelOffset];
            }

            usbHost->onMouse(report, last_buttons);
            if (report.buttons != last_buttons)
            {
                usbHost->onMouseButtons(report, last_buttons);
                last_buttons = report.buttons;
            }
            if (report.x != 0 || report.y != 0 || report.wheel != 0)
            {
                usbHost->onMouseMove(report);
            }
        }
    }

    // ---------------------------------------------------------------
    // 2) 键盘接口 -> 透传键码快照
    // ---------------------------------------------------------------
    // 与鼠标同理: 只有本接口确实是键盘时才走这条路。这样"鼠标+键盘"
    // 复合设备的两个接口都能各自正确透传。
    //
    // 键盘报告标准布局(8 字节):
    //     [0] modifier 位图
    //     [1] 保留
    //     [2..7] 6 个键码
    // 若描述符声明了 Report ID, 则首字节是 ID, 上述布局整体后移一位。
    if (ifaceIsKeyboard && has_data)
    {
        const uint8_t rptId = usbHost->HIDReportDesc.reportId;
        const bool idPresent = (rptId != 0) &&
                               (transfer->actual_num_bytes > 0) &&
                               (transfer->data_buffer[0] == rptId);
        const int base = idPresent ? 1 : 0;

        // 至少要有 modifier + 保留 + 6 键码
        if (transfer->actual_num_bytes >= base + 8)
        {
            uint8_t mod = transfer->data_buffer[base];
            const uint8_t *keys = &transfer->data_buffer[base + 2];
            usbHost->onKeyboardSnapshot(mod, keys);
        }
    }
'''

out = lines[:start] + NEW.split("\n") + lines[end:]
open(path, "w", encoding="utf-8", newline="").write("\n".join(out))
print("replaced lines %d..%d with %d new lines" % (start + 1, end, len(NEW.split("\n"))))
