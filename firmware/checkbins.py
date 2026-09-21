import struct

desk = r"C:\Users\Administrator\Desktop"

files = [
    "修复版5_右芯片_V1_2.bin",
    "修复版4_右芯片_V1_2.bin",
    "最初版_右板_V1_2.bin",
    "工作正常原版_右芯片_V1_2.bin",
]

for f in files:
    d = open(desk + "\\" + f, "rb").read()
    print("%-40s len=%d" % (f, len(d)))
    boot = d[0:8]
    part = d[0x8000:0x8004]
    app = d[0x10000:0x10008]
    entry = struct.unpack("<I", app[4:8])[0]
    print("   boot magic=0x%02x segs=%d  mode=0x%02x size=0x%02x" % (boot[0], boot[1], boot[2], boot[3]))
    print("   part magic=0x%02x 0x%02x" % (part[0], part[1]))
    print("   app  magic=0x%02x segs=%d entry=0x%x" % (app[0], app[1], entry))
    print()
