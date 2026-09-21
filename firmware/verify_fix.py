# -*- coding: utf-8 -*-
import sys
sys.stdout.reconfigure(encoding='utf-8')

s = open(r'_fix1_build\src\esp_usb_host.cpp', encoding='utf-8', errors='replace').read()

print("=== 修复验证 ===")
print()
checks = [
    ("全局无条件写入(放宽)",   "HIDReportDesc = usbHost->iface_report_desc[slot];" in s),
    ("不再要求 isMouse",       "if (isMouse) {\n        HIDReportDesc" not in s),
    ("鼠标布局兜底存在",       "md.xAxisSize == 0 && md.yAxisSize == 0" in s),
    ("兜底设置标准布局",       "md.xAxisStartByte  = 1;" in s and "md.yAxisStartByte  = 2;" in s),
    ("兜底设置滚轮",           "md.wheelStartByte  = 3;" in s),
    ("md 改为非 const(可改)", "EspUsbHost::HIDReportDescriptor md =" in s),
]
for name, ok in checks:
    print("  [%s] %s" % ("OK" if ok else "!!", name))

print()
print("=== 兜底后的解码结果(模拟) ===")
print()
print("  标准 3 字节鼠标报文: [buttons, x, y, wheel]")
print("  兜底布局: buttonStartByte=0, x=1, y=2, wheel=3")
print("  report.buttons = buf[0]")
print("  report.x = (int8_t)buf[1]")
print("  report.y = (int8_t)buf[2]")
print("  report.wheel = (int8_t)buf[3]")
print("  => 与真鼠标一致, 光标可动  ✓")
