# -*- coding: utf-8 -*-
"""Fix A: 右板无条件重提交传输   Fix B: 消除发送洪水 + 缩短阻塞"""
import sys
sys.stdout.reconfigure(encoding='utf-8')
root = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source'

# ===========================================================================
# FIX A —— 右板: 传输重提交去掉 deviceSuspended 门控
# ===========================================================================
pA = root + r'\fw_host\src\esp_usb_host.cpp'
s = open(pA, encoding='utf-8', errors='replace').read()
OLD_A = """    // Resubmit the transfer if the device is not suspended
    if (!usbHost->deviceSuspended)
    {
        esp_err_t err = usb_host_transfer_submit(transfer);
        if (err != ESP_OK)
        {
            ESP_LOGE("EspUsbHost", "Failed to resubmit transfer: err=0x%x, Endpoint=0x%x", err, transfer->bEndpointAddress);
        }
    }"""
NEW_A = """    // ===== 无条件重提交 (关键修复) =====
    //
    // 【为什么去掉 deviceSuspended 门控】
    //   USB Host 读 IN 端点必须"每收完一帧就重新提交", 否则该端点永久静默。
    //   原来这一步被 `if (!deviceSuspended)` 门控: 一旦这个标志被置位,
    //   重提交就永久停止, 而且【没有任何东西会把它清回来】——
    //   实测现象: 键盘用一阵子后突然完全没反应, 必须重新上电才恢复,
    //   而日志里 conn/ready/xfer 全部正常, 只有 lastAct 冻结不再变化。
    //
    //   deviceSuspended 只是"当前是否挂起"的状态描述, 不该当作"是否继续读取"
    //   的开关。挂起期间提交会被 USB 栈自行处理; 真正需要停止读取时应该
    //   在别处显式停, 而不是靠这里静默失效。
    {
        esp_err_t err = usb_host_transfer_submit(transfer);
        if (err != ESP_OK)
        {
            ESP_LOGW("EspUsbHost", "Failed to resubmit transfer: err=0x%x, Endpoint=0x%x",
                     err, transfer->bEndpointAddress);
        }
    }"""
assert OLD_A in s, "Fix A 锚点未找到"
s = s.replace(OLD_A, NEW_A, 1)
open(pA, 'w', encoding='utf-8', newline='').write(s)
print("[A] 右板: 传输重提交已改为无条件")

# ===========================================================================
# FIX B1 —— 左板: emitMouseButtons 加"掩码未变则不重发"守卫
# ===========================================================================
pB = root + r'\fw_device\src\handleCommands.cpp'
s = open(pB, encoding='utf-8', errors='replace').read()

OLD_B1 = """static bool emitMouseButtons(uint8_t mask) {
    if (sendClonedMouseReport(0, 0, 0, mask)) { g_diagEmitOk++; return true; }
    // 传入的 mask 位序与描述符一致 -> 直接采用, 无需逐键 press/release
    s_mouseBtnState = (uint8_t)(mask & 0x1F);
    const bool ok = kbdUsbSendMouse(s_mouseBtnState, 0, 0, 0);
    if (ok) g_diagEmitOk++; else g_diagEmitFail++;
    return ok;
}"""
NEW_B1 = """static bool emitMouseButtons(uint8_t mask) {
    if (sendClonedMouseReport(0, 0, 0, mask)) { g_diagEmitOk++; return true; }

    // ★ 掩码没变就别重发。
    //
    // 本函数会被 ClickTick 以 1ms 周期调用(按键重申看门狗)。原来这里无条件
    // 调 kbdUsbSendMouse, 于是形成 ~1000 次/秒 的发送洪水 —— 而 USB 端点
    // 每 1ms 只能送【一个】报文, 真实键盘报文只能在这股洪水里排队抢机会,
    // 表现为"按了键有时候完全没反应"。
    //
    // 实测: emit ok 以 1500/秒增长, 而同期真实键盘报文只有 41 个。
    //
    // 状态没变就不发是安全的: 看门狗的目的是"把丢失的抬键补回来", 而一旦
    // 掩码与本地影子状态一致, 说明上一次已经按该掩码发过, 无需再发。
    const uint8_t m = (uint8_t)(mask & 0x1F);
    if (m == s_mouseBtnState) return true;

    s_mouseBtnState = m;
    const bool ok = kbdUsbSendMouse(s_mouseBtnState, 0, 0, 0);
    if (ok) g_diagEmitOk++; else g_diagEmitFail++;
    return ok;
}"""
assert OLD_B1 in s, "Fix B1 锚点未找到"
s = s.replace(OLD_B1, NEW_B1, 1)
open(pB, 'w', encoding='utf-8', newline='').write(s)
print("[B1] 左板: 按键发送已加去重守卫 (消除 1000/s 洪水)")

# ===========================================================================
# FIX B2 —— 左板: waitEndpointReady 阻塞从 5ms 缩到 1ms
# ===========================================================================
pC = root + r'\fw_device\src\usb_hid.cpp'
s = open(pC, encoding='utf-8', errors='replace').read()
OLD_B2 = "#define KBD_TX_WAIT_MS   5"
NEW_B2 = """// 等端点空闲的最长时间。
//   USB 端点的轮询周期是 1ms, 所以等一个周期就够; 原来设 5ms 会让每次
//   抢不到端点的调用白白阻塞 5ms, 把后续按键报文越推越后(卡顿来源之一)。
#define KBD_TX_WAIT_MS   1"""
assert OLD_B2 in s, "Fix B2 锚点未找到"
s = s.replace(OLD_B2, NEW_B2, 1)
open(pC, 'w', encoding='utf-8', newline='').write(s)
print("[B2] 左板: 端点等待 5ms -> 1ms")

print()
print("=== 编译两板 ===")
import subprocess, os
env = dict(os.environ)
env['PATH'] = root + r'\piocore\packages\toolchain-xtensa-esp32s3\bin;' + env['PATH']
env['PYTHONPATH'] = root + r'\.piopkgs'
env['PLATFORMIO_CORE_DIR'] = root + r'\piocore'
for name, cwd, ev in [("右板", root + r'\fw_host', 'DIAG'), ("左板", root + r'\fw_device', 'LEFT')]:
    r = subprocess.run(['python', '-c', 'from platformio.__main__ import main; main()', 'run', '-e', ev],
                       cwd=cwd, env=env, capture_output=True, text=True)
    out = (r.stdout or '') + (r.stderr or '')
    bad = [l for l in out.split('\n') if 'error:' in l or 'undefined reference' in l]
    ok  = 'SUCCESS' in out
    print(f"  {name}({ev}): {'SUCCESS' if ok else 'FAILED'}")
    for l in bad[:6]:
        print("    " + l.strip())
