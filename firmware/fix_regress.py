# -*- coding: utf-8 -*-
"""
修复鼠标回归 (老版修复6 右板)

根因: 修复6 把全局 HIDReportDesc 的写入条件收紧为 isMouse==true,
      而 isMouse 依赖描述符字节精确匹配 '05 01 09 02'。
      若匹配失败 -> 全局全零 -> 鼠标用零偏移解码 -> 灯闪但不移动。

修法(三重保险):
  1) 该接口描述符解析成功就写全局(不再要求 isMouse)
  2) 鼠标解码时做"合理性检查": 若拿到的描述符布局明显不可用
     (xAxisSize==0 且 yAxisSize==0), 回退到标准 3 字节布局
  3) 保留按接口取值(复合设备的正确做法)

这样无论描述符解析成功与否, 鼠标都能动 —— 修掉回归。
"""

import sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'_fix1_build\src\esp_usb_host.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

# ---------- 1) 放宽全局写入条件 ----------
OLD1 = """    // 兼容老路径: 鼠标的那份同时写全局 HIDReportDesc。
    // 只有鼠标接口的描述符才允许写全局 —— 键盘/厂商接口的布局与鼠标不同,
    // 写进去会把鼠标的偏移量带偏(这正是老版本鼠标偶发失灵的原因之一)。
    if (isMouse) {
        HIDReportDesc = usbHost->iface_report_desc[slot];
    }
"""
NEW1 = """    // 兼容老路径: 同时写全局 HIDReportDesc。
    //
    // 【严格性调整】原来只在 isMouse==true 时才写。但 isMouse 依赖描述符字节
    // 精确匹配 '05 01 09 02', 一旦某设备用不同编码(分开写 Usage Page/Usage、
    // 或带 Report ID 前缀)就会漏判, 结果全局保持全零 -> 鼠标位移解码全错
    // (实测: 亮灯但光标不动)。
    //
    // 现在改为: 只要该接口的描述符【解析成功】就写全局。
    // 理由: 能走到这里的都是本设备的 HID 接口描述符; 对单接口鼠标设备而言
    //       它就是鼠标描述符。复合设备下鼠标解码仍优先走 iface_report_desc
    //        (按接口取值), 全局只作为兜底, 所以放宽不会带偏鼠标布局。
    HIDReportDesc = usbHost->iface_report_desc[slot];

    // 记录"哪个接口是鼠标", 供诊断与兜底使用
    if (isMouse) {
        ESP_LOGI("EspUsbHost", "iface %u identified as MOUSE", slot);
    }
"""
assert OLD1 in s, "global write block not found"
s = s.replace(OLD1, NEW1, 1)
print("[1] 全局 HIDReportDesc 写入条件已放宽")

# ---------- 2) 鼠标解码加合理性兜底 ----------
OLD2 = """        const EspUsbHost::HIDReportDescriptor &md =
            ((ifaceValid && ifaceOfFrame < EspUsbHost::kMaxReportDescIfaces &&
              usbHost->iface_descValid[ifaceOfFrame])
                 ? usbHost->iface_report_desc[ifaceOfFrame]
                 : usbHost->HIDReportDesc);"""

NEW2 = """        // 取本接口的描述符; 取不到就用全局那份。
        EspUsbHost::HIDReportDescriptor md =
            ((ifaceValid && ifaceOfFrame < EspUsbHost::kMaxReportDescIfaces &&
              usbHost->iface_descValid[ifaceOfFrame])
                 ? usbHost->iface_report_desc[ifaceOfFrame]
                 : usbHost->HIDReportDesc);

        // ---- 合理性检查(修回归的关键) ----
        // 若拿到的布局明显不可用(两个轴位宽都是 0), 说明描述符没解析出来,
        // 此时用它解码会得到全零偏移 -> 位移异常/光标不动。
        // 退回标准 3 字节鼠标布局: buttons@0, x@1, y@2, wheel@3 (8 位轴)。
        // 这保证即使描述符请求失败, 鼠标仍然可用(宁可按标准布局动作)。
        if (md.xAxisSize == 0 && md.yAxisSize == 0) {
            static bool warnedOnce = false;
            if (!warnedOnce) {
                ESP_LOGW("EspUsbHost", "mouse layout unusable (xSize=%d ySize=%d) -> fallback to standard 3-byte layout",
                         md.xAxisSize, md.yAxisSize);
                warnedOnce = true;
            }
            md.reportId        = 0;
            md.buttonStartByte = 0;
            md.buttonSize      = 8;
            md.xAxisSize       = 8;
            md.yAxisSize       = 8;
            md.xAxisStartByte  = 1;
            md.yAxisStartByte  = 2;
            md.wheelSize       = 8;
            md.wheelStartByte  = 3;
        }"""

assert OLD2 in s, "mouse descriptor selection block not found"
s = s.replace(OLD2, NEW2, 1)
print("[2] 鼠标解码已加标准布局兜底")

open(p, 'w', encoding='utf-8', newline='').write(s)
print("done")
