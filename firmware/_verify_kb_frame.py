"""
Independent verification of the 0x23 KB_REPORT frame format.

Re-implements both sides (fw_host sender + fw_device parser) from the source
semantics and checks that a keyboard snapshot round-trips exactly.
"""

def crc16_modbus(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF


def host_build_kb_frame(modifiers, keys, seq):
    """Mirror of EspUsbHost::serial1SendKeyboardBinary"""
    key_count = min(len(keys), 6)
    ln = 1 + key_count

    f = [0] * (ln + 7)
    f[0] = 0xA5
    f[1] = 0x5C
    f[2] = ln
    f[3] = seq & 0xFF
    f[4] = 0x23                      # CMD_KB_REPORT
    f[5] = modifiers
    for i in range(key_count):
        f[6 + i] = keys[i]

    crc_start = 2
    crc_len = 3 + ln
    crc = crc16_modbus(f[crc_start:crc_start + crc_len])
    f[crc_start + crc_len] = crc & 0xFF
    f[crc_start + crc_len + 1] = (crc >> 8) & 0xFF
    return f


def device_parse_a5(raw):
    """Mirror of proto::Parser A5 5C path"""
    assert raw[0] == 0xA5 and raw[1] == 0x5C, "bad header"
    ln = raw[2]
    need = 3 + ln + 2 - 1
    body = raw[3:3 + need]
    assert len(body) == need, f"short frame: {len(body)} != {need}"

    calc = crc16_modbus(raw[2:2 + 3 + ln])
    rx = raw[5 + ln] | (raw[6 + ln] << 8)
    if calc != rx:
        return None, "crc mismatch"

    seq = raw[3]
    cmd = raw[4]
    payload = raw[5:5 + ln]

    if cmd == 0x23:
        keys = [0] * 6
        if ln >= 1:
            n = min(ln - 1, 6)
            for i in range(n):
                keys[i] = payload[1 + i]
        return {"seq": seq, "cmd": cmd, "mod": payload[0], "keys": keys}, None
    return {"seq": seq, "cmd": cmd, "payload": payload}, None


def main():
    ok = True

    cases = [
        ("plain 'a'",        0x00, [0x04, 0, 0, 0, 0, 0]),
        ("Shift+A",          0x02, [0x04, 0, 0, 0, 0, 0]),
        ("Ctrl+Alt+Del",     0x05, [0x4C, 0, 0, 0, 0, 0]),
        ("6 keys (NKRO)",    0x00, [0x04, 0x05, 0x06, 0x07, 0x08, 0x09]),
        ("all released",     0x00, [0, 0, 0, 0, 0, 0]),
        ("mod only (Shift)", 0x02, [0, 0, 0, 0, 0, 0]),
        ("0x00 in key slot", 0x00, [0x04, 0x00, 0x1E, 0, 0, 0]),
        ("key 0xFF edge",    0x00, [0xFF, 0, 0, 0, 0, 0]),
    ]

    for name, mod, keys in cases:
        frame = host_build_kb_frame(mod, keys, seq=7)
        got, err = device_parse_a5(frame)
        if err:
            print(f"  FAIL {name}: {err}")
            ok = False
            continue
        exp_keys = list(keys) + [0] * (6 - len(keys))
        if got["mod"] != mod or got["keys"] != exp_keys:
            print(f"  FAIL {name}: mod={got['mod']:#x} keys={got['keys']} "
                  f"expected mod={mod:#x} keys={exp_keys}")
            ok = False
        else:
            print(f"  ok   {name}: frame={len(frame)}B mod={got['mod']:#04x} "
                  f"keys={[hex(k) for k in got['keys']]}")

    # Corrupted CRC must be rejected
    f = host_build_kb_frame(0x02, [0x04, 0, 0, 0, 0, 0], seq=1)
    f[-1] ^= 0xFF
    got, err = device_parse_a5(f)
    if err == "crc mismatch":
        print("  ok   corrupted CRC rejected")
    else:
        print(f"  FAIL corrupted CRC accepted: {got}")
        ok = False

    # Truncated frame must be detected
    f = host_build_kb_frame(0x00, [0x04], seq=1)
    try:
        device_parse_a5(f[:5])
        print("  FAIL truncated frame accepted")
        ok = False
    except AssertionError:
        print("  ok   truncated frame rejected")

    print()
    print("ALL PASS" if ok else "FAILURES PRESENT")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
