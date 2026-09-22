# -*- coding: utf-8 -*-
"""诊断: 逐步执行替换并打印状态"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\src\USBSetup.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

print("=== 原始文件里 dualHidBegin 的所有位置 ===")
idx = 0
while True:
    i = s.find('dualHidBegin', idx)
    if i < 0: break
    line = s[:i].count('\n') + 1
    print(f"  行 {line}: {s[max(0,i-60):i+40]!r}")
    idx = i + 1

print()
print("=== 第5步的边界 ===")
start = s.find('    // ===== 注册顺序至关重要 (Arduino 框架的硬限制) =====')
end   = s.find('    s_usb_initialized = true;\n}')
print(f"  start = {start}   (行 {s[:start].count(chr(10))+1 if start>0 else '?'})")
print(f"  end   = {end}     (行 {s[:end].count(chr(10))+1 if end>0 else '?'})")
print()
print("=== 这段里包含的内容 (检查 dualHidBegin 是否在范围内) ===")
if start > 0 and end > start:
    block = s[start:end]
    print(f"  block 长度 = {len(block)}")
    print(f"  含 dualHidBegin : {'dualHidBegin' in block}")
    print(f"  含 dualHidSendKeyboard : {'dualHidSendKeyboard' in block}")
    for i, ln in enumerate(block.split('\n')):
        if 'dualHid' in ln:
            print(f"    范围内 twinHid 行: {ln.strip()}")
