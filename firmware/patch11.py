path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\src\esp_usb_host.cpp"
src = open(path, encoding="utf-8", errors="replace").read()

# ---- 1) 键盘分支: 用该接口自己的 Report ID, 并放宽"8 字节"硬要求 ----
OLD_KBD = """    if (ifaceIsKeyboard && has_data)
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
    }"""

NEW_KBD = """    if (ifaceIsKeyboard && has_data)
    {
        // 用【本接口自己】的报告描述符取 Report ID, 而不是全局那份
        // (全局那份可能被别的接口覆盖, 见 iface_report_desc 的说明)。
        const uint8_t rptId = (ifaceValid && ifaceOfFrame < EspUsbHost::kMaxReportDescIfaces &&
                               usbHost->iface_descValid[ifaceOfFrame])
                                ? usbHost->iface_report_desc[ifaceOfFrame].reportId
                                : 0;

        const bool idPresent = (rptId != 0) &&
                               (transfer->actual_num_bytes > 0) &&
                               (transfer->data_buffer[0] == rptId);
        const int base = idPresent ? 1 : 0;

        // 标准键盘报文 = modifier(1) + 保留(1) + 6 键码 = 8 字节。
        // 但有些接收器只报 6 字节(modifier + 保留 + 4 键码), 因此不再硬性
        // 要求 8 字节 —— 只要够读 modifier 和至少一个键码槽就按实际长度发。
        const int avail = transfer->actual_num_bytes - base;
        if (avail >= 2)
        {
            uint8_t mod = transfer->data_buffer[base];

            static uint8_t zero6[6] = {0,0,0,0,0,0};
            uint8_t buf6[6] = {0,0,0,0,0,0};
            // 从 base+2 起最多拷 6 个键码, 不足补 0
            const int keyBytes = (avail > 2) ? (avail - 2) : 0;
            const int n = (keyBytes < 6) ? keyBytes : 6;
            for (int i = 0; i < n; ++i) buf6[i] = transfer->data_buffer[base + 2 + i];
            (void)zero6;

            usbHost->onKeyboardSnapshot(mod, buf6);
        }
    }"""

assert OLD_KBD in src, "keyboard branch not found"
src = src.replace(OLD_KBD, NEW_KBD, 1)

# ---- 2) 鼠标解码: 用该接口自己的描述符 ----
OLD_M = """            static uint8_t last_buttons = 0;
            hid_mouse_report_t report = {};"""
assert OLD_M in src, "mouse decode anchor not found"

open(path, "w", encoding="utf-8", newline="").write(src)
print("keyboard branch rewritten (per-iface report id, relaxed length)")
