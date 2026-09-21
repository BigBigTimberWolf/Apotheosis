path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\src\esp_usb_host.cpp"
src = open(path, encoding="utf-8", errors="replace").read()

OLD = """    HIDReportDesc = localHIDReportDesc;

    // Log final variable values
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "Final parsed values:");
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "reportId: %d", HIDReportDesc.reportId);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "buttonSize: %d", HIDReportDesc.buttonSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "xAxisSize: %d", HIDReportDesc.xAxisSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "yAxisSize: %d", HIDReportDesc.yAxisSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "wheelSize: %d", HIDReportDesc.wheelSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "buttonStartByte: %d", HIDReportDesc.buttonStartByte);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "xAxisStartByte: %d", HIDReportDesc.xAxisStartByte);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "yAxisStartByte: %d", HIDReportDesc.yAxisStartByte);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "wheelStartByte: %d", HIDReportDesc.wheelStartByte);

    return HIDReportDesc;
}"""

NEW = """    // ===== 不再无条件覆盖全局 HIDReportDesc =====
    //
    // 【老代码的 bug】这里原本是 `HIDReportDesc = localHIDReportDesc;`。
    // 复合设备(键盘+鼠标接收器)会依次送来【每个接口】的报告描述符, 于是
    // 全局那份被反复覆盖, 最终留下的是"最后一个到达的接口"的布局。
    // 若最后到达的是键盘接口(布局与鼠标完全不同), 鼠标解码就会拿键盘的
    // 偏移量去读位移 -> 位移错乱或恒为 0。
    //
    // 现在解析结果由调用方(_onReceiveControl)按接口存进 iface_report_desc[],
    // 全局 HIDReportDesc 只作为"最近一次解析结果"的兼容副本保留 ——
    // 鼠标接口的描述符到达时会被赋值为鼠标那份, 供只认全局的老路径使用。
    //
    // 因此这里【直接返回局部结果】, 由调用方决定是否写全局。
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "Final parsed values:");
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "reportId: %d", localHIDReportDesc.reportId);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "buttonSize: %d", localHIDReportDesc.buttonSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "xAxisSize: %d", localHIDReportDesc.xAxisSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "yAxisSize: %d", localHIDReportDesc.yAxisSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "wheelSize: %d", localHIDReportDesc.wheelSize);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "buttonStartByte: %d", localHIDReportDesc.buttonStartByte);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "xAxisStartByte: %d", localHIDReportDesc.xAxisStartByte);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "yAxisStartByte: %d", localHIDReportDesc.yAxisStartByte);
    ESP_LOGI("EspUsbHost::parseHIDReportDescriptor", "wheelStartByte: %d", localHIDReportDesc.wheelStartByte);

    return localHIDReportDesc;
}"""

assert OLD in src, "tail of parseHIDReportDescriptor not found"
src = src.replace(OLD, NEW, 1)
open(path, "w", encoding="utf-8", newline="").write(src)
print("parseHIDReportDescriptor no longer clobbers global")
