path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\src\USBSetup.cpp"
src = open(path, encoding="utf-8", errors="replace").read()

# 回退 patch19 的对象顺序改动(那个分析是错的, 实例号不是问题所在)
start = src.find("// ===== 全局 HID 对象的声明顺序 = 实例编号 (关键!) =====")
end   = src.find("USBHIDMouse Mouse;", start)
assert start > 0 and end > start, "patch19 block not found"

KEEP = """// 全局 HID 对象。注意: 两者共用同一个 HID 实例, 由 Report ID 区分
// (鼠标=HID_REPORT_ID_MOUSE, 键盘=HID_REPORT_ID_KEYBOARD), 见 USBHID::SendReport。
USBHIDMouse Mouse;
"""
src = src[:start] + KEEP + src[end + len("USBHIDMouse Mouse;\n"):]
open(path, "w", encoding="utf-8", newline="").write(src)
print("patch19 reverted")
