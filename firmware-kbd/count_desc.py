# -*- coding: utf-8 -*-
"""精确计算单接口双Collection的报告描述符长度"""
import re, sys
sys.stdout.reconfigure(encoding='utf-8')

path = r'C:\Users\Administrator\Desktop\KBD_PASSTHROUGH\fw_device_kbd\include\kbd_desc.h'
src = open(path, encoding='utf-8').read()

m = re.search(r'#define\s+KBD_REPORT_DESC\s+(.*?)(?=\n//\s*由|\n#define\s+KBD_REPORT_DESC_LEN)', src, re.S)
body = m.group(1)
body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)

# RID_KEYBOARD / RID_MOUSE 是宏名, 需要替换成数值
body = body.replace('RID_KEYBOARD', '1').replace('RID_MOUSE', '2')

vals = re.findall(r'0x[0-9A-Fa-f]{2}', body)
n = len(vals)

print(f"报告描述符字节数 = {n}")
print()

# 结构校验: 解析每个 HID item
i = 0
depth = 0
items = []
while i < len(vals):
    b = int(vals[i], 16)
    if b == 0xFE:  # long item, 不用
        i += 1; continue
    size = b & 0x03
    if size == 3: size = 4
    typ = (b >> 2) & 0x03
    tag = (b >> 4) & 0x0F
    data = vals[i+1:i+1+size]
    items.append((b, typ, tag, data))
    i += 1 + size

print(f"解析出 {len(items)} 个 item, 消耗 {i} 字节 (应等于 {n})")
print(f"{'OK' if i == n else '*** 不匹配! 描述符结构有问题 ***'}")
print()

# 统计 Collection 与 Report ID
coll = [x for x in items if x[1]==0 and x[2]==0xA]
endc = [x for x in items if x[1]==0 and x[2]==0xC]
rid  = [x for x in items if x[1]==1 and x[2]==8]
print(f"Collection 数: {len(coll)}  (应为 2: 键盘 + 鼠标)")
print(f"EndCollection: {len(endc)}")
print(f"Report ID 数: {len(rid)}, 值 = {[d[0] for _,_,_,d in rid]}  (应为 1 和 2)")
print()

# 校验深度平衡
depth = 0
for b,typ,tag,data in items:
    if typ==0 and tag==0xA: depth += 1
    if typ==0 and tag==0xC: depth -= 1
print(f"Collection 嵌套平衡: {'OK (归零)' if depth==0 else '*** 不平衡: %d ***' % depth}")

# 写回长度
new_src = re.sub(r'(#define\s+KBD_REPORT_DESC_LEN\s+)\d+', r'\g<1>' + str(n), src)
if new_src != src:
    open(path, 'w', encoding='utf-8', newline='\n').write(new_src)
    print(f"\n已回填 KBD_REPORT_DESC_LEN = {n}")
