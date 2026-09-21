import re
path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\src\esp_usb_host.cpp"
src = open(path, encoding="utf-8", errors="replace").read()

# 1) 把鼠标接口的描述符写入全局(兼容老路径), 在 _onReceiveControl 里替换那段占位
OLD_COMPAT = """    // 兼容老路径: 鼠标的那份同时写全局
    if (isMouse) {
        ESP_LOGI("EspUsbHost", "Mouse device detected, parsing HID report descriptor");
        HIDReportDescriptor descriptor = usbHost->iface_report_desc[slot];
        (void)descriptor;
    }
"""
NEW_COMPAT = """    // 兼容老路径: 鼠标的那份同时写全局 HIDReportDesc。
    // 只有鼠标接口的描述符才允许写全局 —— 键盘/厂商接口的布局与鼠标不同,
    // 写进去会把鼠标的偏移量带偏(这正是老版本鼠标偶发失灵的原因之一)。
    if (isMouse) {
        HIDReportDesc = usbHost->iface_report_desc[slot];
    }
"""
assert OLD_COMPAT in src, "compat block not found"
src = src.replace(OLD_COMPAT, NEW_COMPAT, 1)

# 2) 鼠标解码块: 引入"本接口描述符", 并把 11 处 HIDReportDesc.xxx 换成 md.xxx
start = src.find("    if (ifaceLooksMouse)")
end   = src.find("    // ---------------------------------------------------------------\n    // 2) 键盘接口")
assert start > 0 and end > start, "mouse decode block bounds not found"

block = src[start:end]

# 在该块开头插入 md 定义
inject = """    if (ifaceLooksMouse)
    {
        // 用【本接口自己的】报告描述符解码 —— 复合设备下每个 HID 接口的布局
        // 都不同, 必须各用各的。取不到时才退回全局那份(纯鼠标设备的兼容路径)。
        const EspUsbHost::HIDReportDescriptor &md =
            ((ifaceValid && ifaceOfFrame < EspUsbHost::kMaxReportDescIfaces &&
              usbHost->iface_descValid[ifaceOfFrame])
                 ? usbHost->iface_report_desc[ifaceOfFrame]
                 : usbHost->HIDReportDesc);
"""
block_new = block.replace("    if (ifaceLooksMouse)\n    {\n", inject, 1)
# 把块内的 HIDReportDesc.xxx 替换为 md.xxx
block_new = block_new.replace("usbHost->HIDReportDesc.", "md.")
src = src[:start] + block_new + src[end:]

open(path, "w", encoding="utf-8", newline="").write(src)
print("mouse decode now uses per-interface descriptor")

# 统计
n = len(re.findall(r"\bmd\.", block_new))
print("  -> replaced %d references with md." % n)
