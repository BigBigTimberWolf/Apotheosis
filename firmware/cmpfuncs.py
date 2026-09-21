import re, os

cur = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_host\src\esp_usb_host.cpp"
old = os.path.expandvars(r"%TEMP%\orig0910\MAKCUNEW_firmware_source\fw_host\src\esp_usb_host.cpp")

def read(p):
    return open(p, encoding="utf-8", errors="replace").read().split("\n")

o = read(old)
n = read(cur)

print("老版 %d 行 / 新版 %d 行\n" % (len(o), len(n)))

funcs = [
    "get_device_status",
    "suspend_device",
    "resume_device",
    "onConfig",
    "_clientEventCallback",
    "_onReceiveControl",
    "onMouseButtons",
    "onMouseMove",
    "onKeyboard",
    "submitControl",
    "parseHIDReportDescriptor",
    "_onReceive",
]

print("%-34s %8s %8s" % ("函数", "老版行", "新版行"))
print("-" * 54)
for f in funcs:
    ol = [i + 1 for i, l in enumerate(o) if f in l and ("EspUsbHost::" + f) in l]
    nl = [i + 1 for i, l in enumerate(n) if f in l and ("EspUsbHost::" + f) in l]
    oo = ol[0] if ol else "-"
    nn = nl[0] if nl else "-"
    print("%-34s %8s %8s" % (f, oo, nn))
