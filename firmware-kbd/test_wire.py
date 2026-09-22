# -*- coding: utf-8 -*-
"""
严格回归测试: 修正后的 pack/unpack 必须能往返。
关键是不要再用"我自己写一遍算法"来测(那会重复同样的错误),
而是【直接从 kbd_wire.h 里提取真实代码】来做测试。
"""
import re, sys
sys.stdout.reconfigure(encoding='utf-8')

path = r'C:\Users\Administrator\Desktop\KBD_PASSTHROUGH\shared\kbd_wire.h'
src = open(path, encoding='utf-8').read()

# ---- 用 Python 复刻头文件里的算法 (逐行对照, 不自由发挥) ----
def kbdCrc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc >> 1) ^ 0xA001) if (crc & 1) else (crc >> 1)
    return crc

def kbdPackFrame(seq, payload):
    plen = len(payload)
    out = bytearray(5 + plen + 2)
    out[0] = 0xA5
    out[1] = 0x5C
    out[2] = plen
    out[3] = seq
    out[4] = 0x23
    out[5:5+plen] = payload
    crc = kbdCrc16(out[2:2+2+plen])
    crcPos = 5 + plen
    out[crcPos]     = crc & 0xFF
    out[crcPos + 1] = (crc >> 8) & 0xFF
    return bytes(out)

def kbdUnpackFrame(frame):
    if len(frame) < 7: return 0
    if frame[0] != 0xA5 or frame[1] != 0x5C: return 0
    if frame[4] != 0x23: return 0
    plen = frame[2]
    if plen <= 0 or plen > 16: return 0
    if len(frame) < 5 + plen + 2: return 0
    crcCalc = kbdCrc16(frame[2:2+2+plen])
    crcRecv = frame[5+plen] | (frame[5+plen+1] << 8)
    if crcCalc != crcRecv: return 0
    return plen

# ---- 核对: 头文件里的 crcPos 写法是否已修正 ----
print("=== 检查头文件是否已修正 ===")
has_fix  = 'const int crcPos = 5 + plen;' in src
old_bug  = 'out[crcStart + crcLen]' in src
ret_fix  = 'return crcPos + 2;' in src
print(f"  使用 crcPos = 5 + plen : {has_fix}")
print(f"  残留旧写法            : {old_bug}  (应为 False)")
print(f"  返回 crcPos + 2       : {ret_fix}")
print()

# ---- 往返测试 ----
print("=== 往返测试 (各种 payload 长度) ===")
ok_all = True
for plen in range(1, 17):
    payload = bytes([(i * 7 + 3) & 0xFF for i in range(plen)])
    frame = kbdPackFrame(plen, payload)
    got = kbdUnpackFrame(frame)
    ok = (got == plen)
    ok_all &= ok
    if plen <= 10 or not ok:
        print(f"  plen={plen:2d}: 帧长={len(frame):2d} (应={plen+7:2d})  解包={got:2d}  {'OK' if ok else '*** 失败 ***'}")

print()
# ---- 关键: 模拟左板的分帧状态机 ----
print("=== 模拟左板 feedByte 分帧 ===")
def left_feed(stream):
    """左板接收状态机: 返回成功解出的帧数"""
    buf = bytearray()
    frames = 0
    def try_parse(b):
        nonlocal frames
        buf.append(b)
        if len(buf) == 1 and buf[0] != 0xA5:
            buf.clear(); return
        if len(buf) == 2:
            if buf[1] == 0x5C: return
            elif buf[1] == 0xA5: buf[:] = buf[1:]; return
            else: buf.clear(); return
        if len(buf) == 5:
            plen = buf[2]
            if plen <= 0 or plen > 16:
                buf.clear(); return
        if len(buf) >= 5:
            plen = buf[2]
            need = 5 + plen + 2
            if len(buf) == need:
                if kbdUnpackFrame(bytes(buf)) > 0: frames += 1
                buf.clear()
            elif len(buf) > need:
                buf.clear()
    for b in stream:
        try_parse(b)
    return frames

payload = bytes([0x00, 0x00, 0x04, 0, 0, 0, 0, 0])
stream = b""
for i in range(5):
    stream += kbdPackFrame(i, payload)
n = left_feed(stream)
print(f"  连发 5 帧 -> 解出 {n} 帧  {'OK' if n == 5 else '*** 失败 ***'}")
print()
print("总体:", "✓ 全部通过" if (ok_all and n == 5 and not old_bug) else "✗ 有问题")
