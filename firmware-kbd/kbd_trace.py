# -*- coding: utf-8 -*-
"""
加键盘逐报文日志 (排查"按住不重复 / 卡键重复")

【为什么需要】
  用户实测两个症状:
    1) 按住一个键没有自动重复 —— 正常键盘按住会由主机产生重复。
       若主机没看到"键一直按着", 就不会重复 -> 说明中间某个环节发了
       "空报文"把按键提前释放了。
    2) 乱敲后某个键卡住并连续输出 —— 说明该键在快照里一直留着没被清掉。

  这两种都只能靠"看实际发出去的报文序列"来定位。
  本补丁把每一个【内容有变化】的键盘报文打印出来(mod + 6 键码),
  这样一次乱敲就能看出状态机在哪里出错。
"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\src\handleCommands.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

# 在 kbdApplyRealReport 的直通发送处加日志
OLD = """    if (kbdUsbReady()) {
        if (kbdUsbSendKeyboard(rep)) g_diagEmitOk++;
        else                         g_diagEmitFail++;
    } else {
        g_diagEmitFail++;
    }
}"""
NEW = """    // ---- 逐报文日志: 只在内容变化时打印, 用于排查卡键/不重复 ----
    static uint8_t s_dbgPrev[8] = {0};
    static bool    s_dbgPrevValid = false;
    const bool changed = !s_dbgPrevValid || memcmp(rep, s_dbgPrev, 8) != 0;

    if (kbdUsbReady()) {
        const bool ok = kbdUsbSendKeyboard(rep);
        if (ok) g_diagEmitOk++; else g_diagEmitFail++;
        if (changed) {
            memcpy(s_dbgPrev, rep, 8);
            s_dbgPrevValid = true;
            Serial0.printf("[KBD] %s mod=%02X keys=%02X %02X %02X %02X %02X %02X\\n",
                           ok ? "TX " : "TX!",
                           (unsigned)rep[0],
                           (unsigned)rep[2], (unsigned)rep[3], (unsigned)rep[4],
                           (unsigned)rep[5], (unsigned)rep[6], (unsigned)rep[7]);
        }
    } else {
        g_diagEmitFail++;
    }
}"""
assert OLD in s, "kbdApplyRealReport 发送块未找到"
s = s.replace(OLD, NEW, 1)

open(p, 'w', encoding='utf-8', newline='').write(s)
print("已加入 [KBD] 逐报文日志")

import subprocess, os
root = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source'
env = dict(os.environ)
env['PATH'] = root + r'\piocore\packages\toolchain-xtensa-esp32s3\bin;' + env['PATH']
env['PYTHONPATH'] = root + r'\.piopkgs'
env['PLATFORMIO_CORE_DIR'] = root + r'\piocore'
r = subprocess.run(['python', '-c', 'from platformio.__main__ import main; main()', 'run', '-e', 'LEFT'],
                   cwd=root + r'\fw_device', env=env, capture_output=True, text=True)
out = (r.stdout or '') + (r.stderr or '')
print("编译:", "SUCCESS" if 'SUCCESS' in out else "FAILED")
for l in out.split('\n'):
    if 'error:' in l or 'undefined reference' in l:
        print("  " + l.strip())
