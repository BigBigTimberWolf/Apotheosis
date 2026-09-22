# -*- coding: utf-8 -*-
"""
最后两处修复:
  B3  kbdEmitMerged 加去重 (原来被 1ms 任务调用, 无条件发 -> ~1000/秒洪水)
  B4  修正我 B1 里的 bug: 状态在【发送前】就记录, 失败会被当成已发 -> 永不重试

关键原则: 【只有发送成功才记录"上次已发"】, 失败必须保留重试机会。
"""
import sys
sys.stdout.reconfigure(encoding='utf-8')
p = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\src\handleCommands.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

# ---------- B3: 键盘快照去重 ----------
OLD = """    if (kbdUsbReady()) {
        uint8_t rep[8];
        rep[0] = mod;
        rep[1] = 0;
        memcpy(rep + 2, mergedKeys, 6);
        if (kbdUsbSendKeyboard(rep)) g_diagEmitOk++;
        else                         g_diagEmitFail++;
    } else {
        g_diagEmitFail++;
    }"""
NEW = """    if (kbdUsbReady()) {
        uint8_t rep[8];
        rep[0] = mod;
        rep[1] = 0;
        memcpy(rep + 2, mergedKeys, 6);

        // ★ 内容没变就不重发。
        //
        // 本函数由 1ms 周期任务(ClickTick)调用, 原来无条件发送 -> ~1000 次/秒
        // 的键盘快照洪水。而 USB 端点每 1ms 只能送一个报文, 真实按键反而要
        // 在洪水里排队 —— 实测 emit ok 约 975/秒, 而真实键盘报文只有 8/秒。
        //
        // 只在【发送成功】后才记录, 失败必须保留重试机会(否则一次失败就
        // 永远不再重发这个状态, 表现为按键卡住)。
        static uint8_t s_lastKbdRep[8] = {0};
        static bool    s_lastKbdValid  = false;
        if (s_lastKbdValid && memcmp(rep, s_lastKbdRep, 8) == 0) return;

        if (kbdUsbSendKeyboard(rep)) {
            memcpy(s_lastKbdRep, rep, 8);
            s_lastKbdValid = true;
            g_diagEmitOk++;
        } else {
            g_diagEmitFail++;
        }
    } else {
        g_diagEmitFail++;
    }"""
assert OLD in s, "B3 锚点未找到"
s = s.replace(OLD, NEW, 1)

# ---------- B4: 修正 B1 的早记录 bug ----------
OLD2 = """    const uint8_t m = (uint8_t)(mask & 0x1F);
    if (m == s_mouseBtnState) return true;

    s_mouseBtnState = m;
    const bool ok = kbdUsbSendMouse(s_mouseBtnState, 0, 0, 0);
    if (ok) g_diagEmitOk++; else g_diagEmitFail++;
    return ok;"""
NEW2 = """    const uint8_t m = (uint8_t)(mask & 0x1F);

    // 与【上次成功发出】的掩码比较。
    // 注意不能拿 s_mouseBtnState 当依据 —— 它在下面被赋值, 若发送失败就会
    // 记成"已发", 之后同样的掩码永远不再重发 => 抬键丢失后按键卡死。
    static uint8_t s_mouseBtnSent  = 0;
    static bool    s_mouseBtnValid = false;
    if (s_mouseBtnValid && m == s_mouseBtnSent) return true;

    s_mouseBtnState = m;                      // 当前状态(位移报文会带上它)
    const bool ok = kbdUsbSendMouse(m, 0, 0, 0);
    if (ok) {
        s_mouseBtnSent  = m;                  // 只有成功才记录
        s_mouseBtnValid = true;
        g_diagEmitOk++;
    } else {
        g_diagEmitFail++;                     // 失败保留重试机会
    }
    return ok;"""
assert OLD2 in s, "B4 锚点未找到"
s = s.replace(OLD2, NEW2, 1)

open(p, 'w', encoding='utf-8', newline='').write(s)
print("[B3] kbdEmitMerged 已加去重 (仅在发送成功后记录)")
print("[B4] 已修正早记录 bug (失败保留重试)")
