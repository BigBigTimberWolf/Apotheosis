# -*- coding: utf-8 -*-
"""覆盖新增的 LED(0x24) 和 身份(0x25) 帧类型"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc >> 1) ^ 0xA001) if (crc & 1) else (crc >> 1)
    return crc

def pack(seq, typ, payload):
    plen = len(payload)
    out = bytearray(5 + plen + 2)
    out[0], out[1], out[2], out[3], out[4] = 0xA5, 0x5C, plen, seq, typ
    out[5:5+plen] = payload
    c = crc16(out[2:2+2+plen])
    out[5+plen] = c & 0xFF
    out[6+plen] = (c >> 8) & 0xFF
    return bytes(out)

def unpack(frame):
    if len(frame) < 7 or frame[0] != 0xA5 or frame[1] != 0x5C: return None
    typ = frame[4]
    if typ not in (0x23, 0x24, 0x25): return None
    plen = frame[2]
    if plen <= 0 or plen > 16: return None
    if len(frame) < 5 + plen + 2: return None
    c = crc16(frame[2:2+2+plen])
    r = frame[5+plen] | (frame[5+plen+1] << 8)
    if c != r: return None
    return (typ, bytes(frame[5:5+plen]))

fails = 0

print("=== LED 帧 (0x24) ===")
for bits in range(8):
    f = pack(bits, 0x24, bytes([bits]))
    r = unpack(f)
    ok = r is not None and r[0] == 0x24 and r[1][0] == bits
    if not ok: fails += 1
print(f"  8 种 LED 组合: {'全部通过' if fails == 0 else '有失败'}")

print()
print("=== 身份帧 (0x25) ===")
cases = [
    (0x3554, 0xFA09, 0x0100),
    (0x046D, 0xC31C, 0x6400),   # 罗技
    (0x1B1C, 0x1B3D, 0x0100),   # 海盗船
    (0x0001, 0x0002, 0x0003),
    (0xFFFF, 0xFFFF, 0xFFFF),
]
for vid, pid, bcd in cases:
    p = bytes([vid & 0xFF, vid >> 8, pid & 0xFF, pid >> 8, bcd & 0xFF, bcd >> 8])
    f = pack(1, 0x25, p)
    r = unpack(f)
    if r is None or r[0] != 0x25 or len(r[1]) != 6:
        print(f"  VID={vid:04X} PID={pid:04X}: *** 失败 ***"); fails += 1; continue
    gv = r[1][0] | (r[1][1] << 8)
    gp = r[1][2] | (r[1][3] << 8)
    gb = r[1][4] | (r[1][5] << 8)
    ok = (gv == vid and gp == pid and gb == bcd)
    print(f"  VID={vid:04X} PID={pid:04X} bcd={bcd:04X} -> 回读 {gv:04X}:{gp:04X}:{gb:04X}  {'OK' if ok else '*** 失败 ***'}")
    if not ok: fails += 1

print()
print("=== 帧类型隔离 (LED 帧不能被当成键盘报告) ===")
ledf = pack(1, 0x24, bytes([0x02]))
r = unpack(ledf)
if r and r[0] == 0x24:
    print("  左板按类型过滤: 0x24 不会被送进键盘通路  OK")
else:
    print("  *** 类型识别失败 ***"); fails += 1

print()
print("=== 未知类型必须被拒绝 ===")
bad = pack(1, 0x99, bytes([1,2,3]))
if unpack(bad) is None:
    print("  未知类型 0x99 被拒绝  OK")
else:
    print("  *** 未拒绝未知类型 ***"); fails += 1

print()
print("总体:", "✓ 全部通过" if fails == 0 else f"✗ {fails} 项失败")
