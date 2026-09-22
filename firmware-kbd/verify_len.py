# -*- coding: utf-8 -*-
"""
精确核对帧长度: 我怀疑 kbdPackFrame 的返回值少算 1 字节。
逐字节走一遍代码, 不猜。
"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

# ---- 右板 kbdPackFrame 的代码逻辑 ----
def pack_trace(plen):
    print(f"--- 右板 kbdPackFrame(plen={plen}) 逐行 ---")
    # out[0]=A5  out[1]=5C  out[2]=plen  out[3]=seq  out[4]=type
    # memcpy(&out[5], payload, plen)
    crcStart = 2
    crc_len  = 2 + plen                 # = len, seq, type, payload
    # CRC 写入位置:
    #   out[crcStart + crc_len]     = crc & 0xFF
    #   out[crcStart + crc_len + 1] = crc >> 8
    last_index = crcStart + crc_len + 1  # 最后一个字节的下标
    total = last_index + 1               # 字节数 = 下标 + 1

    print(f"  头 5 字节: 下标 0..4")
    print(f"  payload {plen} 字节: 下标 5..{4+plen}")
    print(f"  CRC 2 字节: 下标 {crcStart+crc_len}..{last_index}")
    print(f"  => 总字节数 = 下标最大 + 1 = {last_index} + 1 = {total}")
    ret = crcStart + crc_len + 2
    print(f"  但代码 return crcStart + crc_len + 2 = {crcStart} + {crc_len} + 2 = {ret}")
    print(f"  {'✓ 一致' if ret == total else f'✗ 少算 {total - ret} 字节!'}")
    return total, ret

total, ret = pack_trace(8)
print()

# ---- 左板 feedByte 的期望长度 ----
print("--- 左板 feedByte 的期望 ---")
plen = 8
need = 5 + plen + 2
print(f"  need = 5 + plen + 2 = 5 + {plen} + 2 = {need}")
print()
print("=" * 60)
if ret != need:
    print(f"两端不一致: 左板等 {need} 字节, 右板只发 {ret} 字节")
    print(f"差 {need - ret} 字节 -> 左板永远凑不齐 -> CRC 必然失败")
else:
    print("两端一致")
print("=" * 60)
