# -*- coding: utf-8 -*-
import sys
sys.stdout.reconfigure(encoding='utf-8')

print("=" * 74)
print("日志证据: 左板收到字节了, 但 CRC 全部失败")
print("=" * 74)
print()
print("  [KBD-DEV] usb=READY frames=0 sent=0 drop=0 crc=9")
print()
print("  frames=0  -> 没有一帧通过校验")
print("  crc=9     -> 有 9 次进入了 CRC 校验但失败了")
print()
print("关键推论:")
print("  CRC 校验失败说明【左板确实凑齐了一帧的长度】并做了校验 ——")
print("  这意味着字节流是通的, 波特率也对(否则收到的会是乱码, 连帧头都凑不齐)")
print()
print("=" * 74)
print("那么 CRC 为什么失败? 检查两端的算法是否一致")
print("=" * 74)
print()

# 复现左板的 CRC 算法
def crc16_modbus(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc >> 1) ^ 0xA001) if (crc & 1) else (crc >> 1)
    return crc

# 右板打包
def pack(seq, payload):
    plen = len(payload)
    out = [0xA5, 0x5C, plen, seq, 0x23] + list(payload)
    crc_start = 2
    crc_len = 2 + plen
    crc = crc16_modbus(out[crc_start:crc_start+crc_len])
    out += [crc & 0xFF, (crc >> 8) & 0xFF]
    return bytes(out)

# 左板解包
def unpack(frame):
    if frame[0] != 0xA5 or frame[1] != 0x5C: return "帧头错"
    if frame[4] != 0x23: return "类型错"
    plen = frame[2]
    crc_calc = crc16_modbus(frame[2:4+plen])
    crc_recv = frame[5+plen] | (frame[5+plen+1] << 8)
    if crc_calc != crc_recv:
        return f"CRC错 calc={crc_calc:04X} recv={crc_recv:04X}"
    return "OK"

# 标准 8 字节键盘报文
payload = bytes([0x00, 0x00, 0x04, 0,0,0,0,0])
frame = pack(1, payload)
print("右板发出的帧:")
print("  " + " ".join(f"{b:02X}" for b in frame))
print(f"  长度 = {len(frame)} 字节  (头5 + payload8 + CRC2 = 15)")
print()
print("左板解包结果:", unpack(frame))
print()

print("=" * 74)
print("现在检查: 左板的帧长计算是否正确")
print("=" * 74)
print()
print("  左板 feedByte 里:")
print("     need = 5 + plen + 2")
print("  右板 kbdPackFrame 里:")
print("     返回 crcStart + crcLen + 2 = 2 + (2+plen) + 2 = plen + 6")
print()
print("  ★ 发现问题! 两端长度定义不一致:")
print(f"    左板认为整帧 = 5 + plen + 2 = {5+8+2} 字节 (plen=8)")
print(f"    右板实际发出 = plen + 6     = {8+6} 字节")
print()
print("  差 1 字节! 左板会多等一个字节才校验, 于是 CRC 位置错位 -> 必然失败")
print()
print("=" * 74)
print("根因确认")
print("=" * 74)
print()
print("  右板 kbdPackFrame 的返回值算错了:")
print("      out[0..4]  = 头 5 字节")
print("      out[5..]   = payload plen 字节")
print("      CRC 2 字节")
print("      总长应为 5 + plen + 2 = plen + 7")
print()
print("      但代码写的是 crcStart + crcLen + 2 = 2 + (2+plen) + 2 = plen + 6")
print("      -> 少算 1 字节")
print()
print("  这解释了全部现象: frames=0 (永远校验不过), crc 持续增长")
