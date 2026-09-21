# -*- coding: utf-8 -*-
"""
静态联调: 按 fw_host 的 serial1SendKeyboardRaw 逐字节造帧,
再按 fw_device 的 proto_parser 逐字节解帧, 确认键盘原样透传这条路能对上。

不碰硬件, 只验"两端对同一个字节流的解释是否一致"。
"""


def crc16_modbus(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc


def host_pack_raw_kbd(iface, report, seq):
    """复刻 fw_host/src/esp_tasks.cpp: serial1SendKeyboardRaw"""
    report = report[:8]
    plen = 1 + len(report)
    f = [0xA5, 0x5C, plen, seq & 0xFF, 0x23, iface] + list(report)
    crc_start, crc_len = 2, 3 + plen
    crc = crc16_modbus(f[crc_start:crc_start + crc_len])
    f += [crc & 0xFF, (crc >> 8) & 0xFF]
    assert len(f) == plen + 7, (len(f), plen + 7)
    assert crc_start + crc_len == 5 + plen
    return bytes(f)


def host_pack_heartbeat(seq):
    """复刻 fw_host/src/esp_tasks.cpp: serial1SendKeyboardHeartbeat
    0x23 帧 + payload=[0xFF]。0xFF 是"心跳"哨兵, 不是任何合法接口号。"""
    plen = 1
    f = [0xA5, 0x5C, plen, seq & 0xFF, 0x23, 0xFF]
    crc_start, crc_len = 2, 3 + plen
    crc = crc16_modbus(f[crc_start:crc_start + crc_len])
    f += [crc & 0xFF, (crc >> 8) & 0xFF]
    assert len(f) == plen + 7 == 8, len(f)
    assert crc_start + crc_len == 5 + plen
    return bytes(f)


def device_parse(frame):
    """复刻 fw_device/src/proto_parser.cpp 的帧解析 + case 0x23 分派"""
    assert frame[0] == 0xA5 and frame[1] == 0x5C
    plen = frame[2]
    total = plen + 7
    assert len(frame) == total, (len(frame), total)
    body = frame[2:2 + 3 + plen]
    crc_rx = frame[5 + plen] | (frame[6 + plen] << 8)
    assert crc16_modbus(body) == crc_rx, "CRC 不匹配"

    cmd = frame[4]
    pl = frame[5:5 + plen]
    length = len(pl)

    if cmd != 0x23:
        return ("other", cmd)

    # ---- case 0x23 ----
    # ★ 心跳必须先判: payload=[0xFF] 只表示"链路活着", 不带按键状态。
    #   若被当成真实报文, 0xFF 会被当接口号(>7)而落到旧版 6 字节分支 ——
    #   那会把 mod 设成 0xFF 并清零所有键码, 也就是"凭空释放全部按键"。
    if length == 1 and pl[0] == 0xFF:
        return ("HEARTBEAT",)

    if length >= 9 and pl[0] <= 7:
        raw = pl[1:]
        return ("RAW", pl[0], bytes(raw))
    keys = [0] * 6
    if length >= 1:
        n = min(length - 1, 6)
        for i in range(n):
            keys[i] = pl[1 + i]
    return ("LEGACY", pl[0], keys)


def main():
    seq = 0
    cases = [
        # (说明, 原始 8 字节报文)
        ("松开全部",      bytes([0x00, 0x00, 0, 0, 0, 0, 0, 0])),
        ("按住 A",        bytes([0x00, 0x00, 0x04, 0, 0, 0, 0, 0])),
        ("Shift+A",       bytes([0x02, 0x00, 0x04, 0, 0, 0, 0, 0])),
        ("多键 A+S+D",    bytes([0x00, 0x00, 0x04, 0x16, 0x07, 0, 0, 0])),
        ("松开 A",        bytes([0x00, 0x00, 0x00, 0, 0, 0, 0, 0])),
        ("屏蔽时补发全空", bytes([0x00, 0x00, 0, 0, 0, 0, 0, 0])),
    ]

    ok = True
    for name, rep in cases:
        frame = host_pack_raw_kbd(0, rep, seq)
        seq = (seq + 1) & 0xFF
        got = device_parse(frame)
        good = (got[0] == "RAW" and got[2] == rep)
        ok &= good
        print("%-16s -> %-5s iface=%s raw=%s  %s"
              % (name, got[0], got[1], got[2].hex(" "), "OK" if good else "MISMATCH"))

    # 旧版 6 字节兼容路径必须仍然走得通(左板向后兼容旧右板)
    legacy = bytes([0xA5, 0x5C, 0x07, 0x01, 0x23, 0x02, 0x04, 0x16, 0x07, 0, 0, 0])
    body = legacy[2:2 + 3 + 7]
    c = crc16_modbus(body)
    legacy = legacy + bytes([c & 0xFF, (c >> 8) & 0xFF])
    got = device_parse(legacy)
    good = (got[0] == "LEGACY" and got[1] == 0x02 and got[2][:3] == [0x04, 0x16, 0x07])
    ok &= good
    print("%-16s -> %-5s mod=%s keys=%s  %s"
          % ("旧版兼容帧", got[0], hex(got[1]), got[2], "OK" if good else "MISMATCH"))

    # 心跳帧必须被识别成"心跳", 不能被当成真实报文
    hb = host_pack_heartbeat(seq)
    got = device_parse(hb)
    good = (got[0] == "HEARTBEAT")
    ok &= good
    print("%-16s -> %-9s %s" % ("心跳帧", got[0], "OK" if good else "MISMATCH"))

    print()
    print("结果:", "全部通过" if ok else "存在不一致")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
