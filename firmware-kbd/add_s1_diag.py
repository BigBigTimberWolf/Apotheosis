# -*- coding: utf-8 -*-
"""
启用 MAKCUNEW 已有的 Serial1 原始字节诊断, 并接进我的心跳日志。

【为什么需要】
  当前 rx move=0 btn=0 kbd=0 —— 右板一个有效帧都没有。
  但要区分两种情况, 必须看 Serial1 的【原始字节】:
    (a) s1bytes=0        -> 右板根本没发 / 串口链路断
    (b) s1bytes 在涨但 rx=0 -> 有字节但帧解析不出来(波特率/格式/协议不匹配)
"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

root = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device'

# ---------- 1) 去掉 serial1RX 里计数器的 FW_DIAG 守卫 ----------
p = root + r'\src\handleCommands.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

old = """#if FW_DIAG
        g_diag_rx1_bytes++;
        {
            uint32_t sl = g_diag_snap_len;
            if (sl < 64) { g_diag_snap[sl] = (uint8_t)byte; g_diag_snap_len = sl + 1; }
        }
#endif"""
new = """        // ★ 不再用 FW_DIAG 守卫: 这是排查"右板到底有没有发"的关键数据,
        //   每次都要看。开销只是两个自增, 可忽略。
        g_diag_rx1_bytes++;
        {
            uint32_t sl = g_diag_snap_len;
            if (sl < 64) { g_diag_snap[sl] = (uint8_t)byte; g_diag_snap_len = sl + 1; }
        }"""
assert old in s, "serial1RX 计数器块未找到"
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print("[1] serial1RX 的字节计数已启用 (不再需要 FW_DIAG)")

# ---------- 2) 心跳日志加入 s1bytes + 首次字节快照 ----------
p2 = root + r'\src\main.cpp'
s2 = open(p2, encoding='utf-8', errors='replace').read()

old_stat = """        Serial0.printf("[STAT] ready=%d mounted=%d | rx move=%lu btn=%lu kbd=%lu | emit ok=%lu fail=%lu moved=%lu\\n",
                       (int)isUsbReadyToTransfer(),
                       (int)tud_mounted(),
                       (unsigned long)g_diagMoveRx,
                       (unsigned long)g_diagBtnRx,
                       (unsigned long)g_diagKbdRx,
                       (unsigned long)g_diagEmitOk,
                       (unsigned long)g_diagEmitFail,
                       (unsigned long)g_diagMoved);"""

new_stat = """        Serial0.printf("[STAT] ready=%d mounted=%d | s1bytes=%lu | rx move=%lu btn=%lu kbd=%lu | emit ok=%lu fail=%lu moved=%lu\\n",
                       (int)isUsbReadyToTransfer(),
                       (int)tud_mounted(),
                       (unsigned long)g_diag_rx1_bytes,
                       (unsigned long)g_diagMoveRx,
                       (unsigned long)g_diagBtnRx,
                       (unsigned long)g_diagKbdRx,
                       (unsigned long)g_diagEmitOk,
                       (unsigned long)g_diagEmitFail,
                       (unsigned long)g_diagMoved);

        // 首次收到 Serial1 数据时, 把头几十字节的十六进制打出来 ——
        // 这样能直接看出右板发的是什么(结构化帧? 原始报告? 还是乱码?)
        static bool s_snapDone = false;
        if (!s_snapDone && g_diag_snap_len > 0) {
            s_snapDone = true;
            Serial0.printf("[S1RAW] %lu 字节: ", (unsigned long)g_diag_snap_len);
            for (uint32_t i = 0; i < g_diag_snap_len && i < 40; ++i) {
                Serial0.printf("%02X ", g_diag_snap[i]);
            }
            Serial0.println();
        }"""

assert old_stat in s2, "STAT 打印块未找到"
s2 = s2.replace(old_stat, new_stat, 1)
open(p2, 'w', encoding='utf-8', newline='').write(s2)
print("[2] 心跳已加入 s1bytes + 首次字节快照")

print()
print("验证:")
s2 = open(p2, encoding='utf-8', errors='replace').read()
print("  s1bytes 已打印 :", 's1bytes=' in s2)
print("  S1RAW 快照已加 :", 'S1RAW' in s2)
