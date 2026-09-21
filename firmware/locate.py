import io

path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\src\esp_usb_host.cpp"
lines = open(path, encoding="utf-8", errors="replace").read().split("\n")

# locate block: from the comment "// Process the HID report if it's a mouse report"
# through the end of the double loop (the line "    }" closing the for at depth)
start = None
for i, l in enumerate(lines):
    if "// Process the HID report if it's a mouse report" in l:
        start = i
        break
assert start is not None, "start not found"

# find the end: the loop closes right before "    // Handle transfer status"
end = None
for j in range(start, len(lines)):
    if "// Handle transfer status" in lines[j]:
        end = j
        break
assert end is not None, "end not found"

print("block lines %d .. %d (%d lines)" % (start + 1, end, end - start))
print("--- first 3 ---")
for l in lines[start:start + 3]:
    print(repr(l))
print("--- last 6 before end ---")
for l in lines[end - 6:end]:
    print(repr(l))
