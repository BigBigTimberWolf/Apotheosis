# -*- coding: utf-8 -*-
"""
正确解析: 必须保留 RID_KEYBOARD / RID_MOUSE 这两个宏, 它们是真实的字节。
上一版的正则只抓 0xNN, 把宏名整个丢掉了 -> 字节流错位 -> 误报。
"""
import re, sys
sys.stdout.reconfigure(encoding='utf-8')

path = r'C:\Users\Administrator\Desktop\KBD_PASSTHROUGH\fw_device_kbd\include\kbd_desc.h'
src = open(path, encoding='utf-8').read()

# 先取出宏
rids_def = dict(re.findall(r'#define\s+(RID_\w+)\s+(\d+)', src))
print("宏定义:", rids_def)

m = re.search(r'#define\s+KBD_REPORT_DESC\s+(.*?)(?=\n//\s*由|\n#define\s+KBD_REPORT_DESC_LEN)', src, re.S)
body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)

# 关键: 同时匹配 0xNN 和宏名, 保持顺序
tokens = re.findall(r'0x[0-9A-Fa-f]{2}|RID_\w+', body)
vals = []
for t in tokens:
    if t.startswith('0x'):
        vals.append(int(t, 16))
    else:
        vals.append(int(rids_def[t]))

print(f"总字节数 = {len(vals)}")
print()

MAIN_TAG = {0x8:'Input', 0x9:'Output', 0xA:'Collection', 0xB:'Feature', 0xC:'EndCollection'}
GLOBAL_TAG = {0x0:'UsagePage', 0x1:'LogicalMin', 0x2:'LogicalMax', 0x7:'ReportSize',
              0x8:'ReportID', 0x9:'ReportCount'}

i, depth = 0, 0
collections, rids = [], []
while i < len(vals):
    b = vals[i]
    size = b & 0x03
    if size == 3: size = 4
    typ = (b >> 2) & 0x03
    tag = (b >> 4) & 0x0F
    data = vals[i+1:i+1+size]
    i += 1 + size

    if typ == 0 and tag == 0xA:
        depth += 1
        collections.append((depth, data[0] if data else None))
    elif typ == 0 and tag == 0xC:
        depth -= 1
    elif typ == 1 and tag == 0x8 and data:
        rids.append(data[0])

print(f"消耗字节 = {i}  {'OK' if i == len(vals) else '*** 不一致 ***'}")
print(f"Collection 数 = {len(collections)}, 最终深度 = {depth}  (应为 2 / 0)")
for d, t in collections:
    print(f"   深度{d}: collection type = 0x{t:02x} (1=Application)")
print(f"Report ID = {rids}  (应为 [1, 2])")
print()

ok = (i == len(vals)) and (depth == 0) and (len(collections) == 3) and (rids == [1, 2])
print("总体:", "✓ 描述符结构正确" if ok else "✗ 仍有问题")
