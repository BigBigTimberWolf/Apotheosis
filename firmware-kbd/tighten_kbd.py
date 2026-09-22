# -*- coding: utf-8 -*-
"""收紧键盘判定: 只认标准 8 字节 boot keyboard 报文; 固定 6 键码"""
import sys, subprocess, os
sys.stdout.reconfigure(encoding='utf-8')

p = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_host\src\esp_usb_host.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

# 1) 收紧判定
OLD1 = "        epKnown && has_data && (reportLen >= 3) &&"
NEW1 = "        epKnown && has_data && (reportLen == 8) &&   // 只认标准 8 字节 boot keyboard"
if OLD1 in s:
    s = s.replace(OLD1, NEW1, 1); print("[1] 收紧为 reportLen == 8")
else:
    print("[1] 未找到 (检查实际文本)")

# 2) 固定 6 键码
OLD2 = """        uint8_t n = (reportLen >= 8) ? 6 : (uint8_t)(reportLen - 2);
        if (n > 6) n = 6;"""
NEW2 = """        // 固定 6 个键码: 8 字节 boot keyboard 布局固定, 不再按长度推算
        const uint8_t n = 6;"""
if OLD2 in s:
    s = s.replace(OLD2, NEW2, 1); print("[2] 固定 n = 6")
else:
    print("[2] 未找到")

# 3) 右板调试日志 (确认接口号与长度)
OLD3 = "        usbHost->onKeyboard(kr);"
NEW3 = """        if (usbHost->debugModeActive) {
            ESP_LOGI("EspUsbHost", "kbdRaw iface=%u len=%d", (unsigned)ifaceNum, reportLen);
        }
        usbHost->onKeyboard(kr);"""
if OLD3 in s and 'kbdRaw iface' not in s:
    s = s.replace(OLD3, NEW3, 1); print("[3] 已加调试日志")
else:
    print("[3] 跳过")

open(p, 'w', encoding='utf-8', newline='').write(s)

root = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source'
env = dict(os.environ)
env['PATH'] = root + r'\piocore\packages\toolchain-xtensa-esp32s3\bin;' + env['PATH']
env['PYTHONPATH'] = root + r'\.piopkgs'
env['PLATFORMIO_CORE_DIR'] = root + r'\piocore'
r = subprocess.run(['python','-c','from platformio.__main__ import main; main()','run','-e','DIAG'],
                   cwd=root + r'\fw_host', env=env, capture_output=True, text=True)
out = (r.stdout or '') + (r.stderr or '')
print("编译:", "SUCCESS" if 'SUCCESS' in out else "FAILED")
for l in out.split('\n'):
    if 'error:' in l: print("  " + l.strip())
