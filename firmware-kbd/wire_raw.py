# -*- coding: utf-8 -*-
"""补: onKbRaw 回调声明 + 接线"""
import sys, subprocess, os
sys.stdout.reconfigure(encoding='utf-8')
root = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source'

# ---- 1) 头文件加声明 ----
ph = root + r'\fw_device\include\proto_parser.h'
h = open(ph, encoding='utf-8', errors='replace').read()
OLD = "    void (*onKbReport)(uint8_t mod, const uint8_t keys6[6]) = nullptr;"
assert OLD in h, "声明锚点未找到"
if 'onKbRaw' not in h:
    NEW = OLD + "\n    // 键盘原样字节回调: raw = 端点原始报文(>=8 字节), 直接发 USB 不做解析\n    void (*onKbRaw)(const uint8_t *raw, uint8_t len) = nullptr;"
    h = h.replace(OLD, NEW, 1)
    open(ph, 'w', encoding='utf-8', newline='').write(h)
    print("[1] proto_parser.h: 已加 onKbRaw 声明")

# ---- 2) handleCommands.cpp 接线 ----
pc = root + r'\fw_device\src\handleCommands.cpp'
c = open(pc, encoding='utf-8', errors='replace').read()
OLD2 = """    g_proto1.onKbReport = [](uint8_t mod, const uint8_t keys6[6]) {"""
if 'g_proto1.onKbRaw' not in c:
    # 在 onKbReport 赋值之前插入 onKbRaw 赋值
    NEW2 = """    // ★ 键盘原样转发: fw_host 送来的端点原始字节直接发 USB, 不解析不合并。
    //   用户实测(独立版)这套最流畅。
    g_proto1.onKbRaw = [](const uint8_t *raw, uint8_t len) {
        g_diagKbdRx++;
        kbdApplyRealRaw(raw, len);
    };

""" + OLD2
    assert OLD2 in c
    c = c.replace(OLD2, NEW2, 1)
    open(pc, 'w', encoding='utf-8', newline='').write(c)
    print("[2] handleCommands.cpp: 已接线 g_proto1.onKbRaw")

# ---- 3) 编译 ----
env = dict(os.environ)
env['PATH'] = root + r'\piocore\packages\toolchain-xtensa-esp32s3\bin;' + env['PATH']
env['PYTHONPATH'] = root + r'\.piopkgs'
env['PLATFORMIO_CORE_DIR'] = root + r'\piocore'
for name, cwd, ev in [("左板", root + r'\fw_device', 'LEFT'), ("右板", root + r'\fw_host', 'DIAG')]:
    r = subprocess.run(['python','-c','from platformio.__main__ import main; main()','run','-e',ev],
                       cwd=cwd, env=env, capture_output=True, text=True)
    out = (r.stdout or '') + (r.stderr or '')
    print(f"  {name}({ev}):", "SUCCESS" if 'SUCCESS' in out else "FAILED")
    for l in out.split('\n'):
        if 'error:' in l or 'undefined reference' in l:
            print("    " + l.strip())
