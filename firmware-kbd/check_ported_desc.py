# -*- coding: utf-8 -*-
"""校验移植后的报告描述符: 结构 / 无 RID / 长度"""
import re, sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\src\usb_desc.cpp'
src = open(p, encoding='utf-8').read()

def grab(name):
    m = re.search(r'static const uint8_t ' + name + r'\[\]\s*=\s*\{(.*?)\};', src, re.S)
    body = re.sub(r'//[^\n]*', '', m.group(1))
    return [int(v, 16) for v in re.findall(r'0x[0-9A-Fa-f]{2}', body)]

def check(name, vals, declared):
    i, depth, coll, rid = 0, 0, 0, 0
    while i < len(vals):
        b = vals[i]
        size = b & 0x03
        if size == 3: size = 4
        typ = (b >> 2) & 0x03
        tag = (b >> 4) & 0x0F
        if typ == 0 and tag == 0xA: depth += 1; coll += 1
        if typ == 0 and tag == 0xC: depth -= 1
        if typ == 1 and tag == 0x8: rid += 1
        i += 1 + size

    ok_len  = (i == len(vals))
    ok_def  = (len(vals) == declared)
    ok_col  = (depth == 0)
    ok_rid  = (rid == 0)     # ★ 无 Report ID (方案乙)
    ok_head = (vals[0] == 0x05 and vals[1] == 0x01)
    ok_tail = (vals[-1] == 0xC0)

    print(f"--- {name} ---")
    print(f"  字节数      : {len(vals)} (声明 {declared})  {'OK' if ok_def else '*** 不符 ***'}")
    print(f"  解析消耗    : {i}  {'OK' if ok_len else '*** 错位 ***'}")
    print(f"  Collection  : {coll} 个, 嵌套深度归零 {'OK' if ok_col else '*** 不平衡 ***'}")
    print(f"  Report ID   : {rid} 个  {'OK (方案乙: 无 RID)' if ok_rid else '*** 仍有 RID ***'}")
    print(f"  首尾        : {vals[0]:02X} {vals[1]:02X} ... {vals[-1]:02X}  {'OK' if ok_head and ok_tail else '*** 异常 ***'}")
    print()
    return ok_len and ok_def and ok_col and ok_rid and ok_head and ok_tail

k = grab('s_descKeyboard')
m = grab('s_descMouse')

a = check('键盘描述符', k, 63)
b = check('鼠标描述符', m, 52)

print("总体:", "✓ 两个描述符都正确" if (a and b) else "✗ 有问题")

# 附带: 确认接口协议常量用对了
print()
print("=== 接口协议 (Boot Keyboard / Boot Mouse) ===")
for pat, label in [('HID_ITF_PROTOCOL_KEYBOARD', '键盘 = Boot Keyboard(SubClass 01/Prot 01)'),
                   ('HID_ITF_PROTOCOL_MOUSE',    '鼠标 = Boot Mouse(SubClass 01/Prot 02)')]:
    print(f"  {label}: {'OK' if pat in src else '*** 缺失 ***'}")
