import re

path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_host\src\esp_usb_host.cpp"
lines = open(path, encoding="utf-8", errors="replace").read().split("\n")

depth = 0
in_block_comment = False
for ln, raw in enumerate(lines, 1):
    s = raw
    # crude comment strip
    if in_block_comment:
        if "*/" in s:
            s = s.split("*/", 1)[1]
            in_block_comment = False
        else:
            continue
    s = re.sub(r"/\*.*?\*/", "", s)
    if "/*" in s:
        s = s.split("/*", 1)[0]
        in_block_comment = True
    s = re.sub(r"//.*", "", s)
    s = re.sub(r'"(?:\\.|[^"\\])*"', '""', s)
    s = re.sub(r"'(?:\\.|[^'\\])*'", "''", s)

    before = depth
    for ch in s:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
    # report top-level-ish transitions at function starts
    st = s.strip()
    if before == 1 and depth == 1 and st.endswith("{"):
        pass
    if st.startswith("}") and before == 1:
        pass

# print depth at the end of key functions
depth = 0
in_block_comment = False
for ln, raw in enumerate(lines, 1):
    s = raw
    if in_block_comment:
        if "*/" in s:
            s = s.split("*/", 1)[1]
            in_block_comment = False
        else:
            continue
    s = re.sub(r"/\*.*?\*/", "", s)
    if "/*" in s:
        s = s.split("/*", 1)[0]
        in_block_comment = True
    s = re.sub(r"//.*", "", s)
    s = re.sub(r'"(?:\\.|[^"\\])*"', '""', s)
    s = re.sub(r"'(?:\\.|[^'\\])*'", "''", s)
    for ch in s:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
    st = raw.strip()
    if st.startswith(("void EspUsbHost::", "esp_err_t EspUsbHost::", "bool EspUsbHost::",
                      "EspUsbHost::HIDReportDescriptor", "String EspUsbHost::", "void usbLibraryTask")):
        print("line %5d  depth_before=%-3d  %s" % (ln, depth, st[:70]))
print("FINAL depth =", depth)
