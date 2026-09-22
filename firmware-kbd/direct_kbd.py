# -*- coding: utf-8 -*-
"""
键盘改为「直通转发」(按用户要求)

【模型】
    真键盘 ──> 收到报文【原样直发】   ──┐
                                        ├─> USB ──> 被控机
    上位机注入 ──> 【生成报文直发】     ──┘
  不再做 real ∪ inj 的合并。

【为什么】
  合并路径每一步(取锁/合并/差分/影子状态/去重)都可能出偏差, 结果是
  "键卡住"或"按了没反应"。直通则把真键盘报文原样送过去, 主机看到的
  与直插键盘一致。

【km.mask 不受影响】
  屏蔽是在【右板】做的(fw_host 的 onKeyboard: 屏蔽窗口内真实键盘变化不转发),
  与左板的转发方式完全无关, 因此该功能自动保留。

【取舍(用户已确认接受)】
  真键与注入键若在同一时刻, 后到的完整快照会盖掉先到的。用户的实际用法
  是"注入时不手动按", 所以无影响。
"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\src\handleCommands.cpp'
s = open(p, encoding='utf-8', errors='replace').read()
n0 = len(s)

# ---------- 1) kbdApplyRealReport 改为直通 ----------
OLD = """static void kbdApplyRealReport(uint8_t mod, const uint8_t keys6[6]) {
    if (s_kbd_mtx && xSemaphoreTake(s_kbd_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_kbd_mod_real = mod;
        memcpy(s_kbd_keys_real, keys6, 6);
        xSemaphoreGive(s_kbd_mtx);
    }
    kbdEmitMerged();
}"""
NEW = """static void kbdApplyRealReport(uint8_t mod, const uint8_t keys6[6]) {
    // ===== 直通转发 (不再进合并状态机) =====
    //
    // 真键盘(fw_host 经 Serial1 送来的 0x23 帧)的报文【原样】发出去:
    //   标准 boot keyboard 布局 = modifier + 保留 + 6 个键码
    //
    // 【为什么改成直通】
    //   原来的路径是"写 real 状态 -> 取锁合并 real∪inj -> 差分 -> 发快照"。
    //   每一步都可能出偏差(锁竞争 / 去重误判 / 端点忙导致丢帧), 结果就是
    //   用户实测到的"键卡住""按了没反应"。直通没有这些中间环节。
    //
    // 【km.mask 屏蔽仍然有效】
    //   屏蔽是在【右板】做的: fw_host 的 onKeyboard 在屏蔽窗口内直接不转发
    //   真实键盘变化。所以这里收不到报文就等于被屏蔽了, 与转发方式无关。
    //
    // 【注意: 不再写 s_kbd_mod_real / s_kbd_keys_real】
    //   那两份状态只被 kbdEmitMerged 的合并使用。直通后它们不再参与,
    //   保留写入反而会让注入路径带上过期的真实按键 -> 继续卡键。
    uint8_t rep[8];
    rep[0] = mod;
    rep[1] = 0;
    memcpy(rep + 2, keys6, 6);

    if (kbdUsbReady()) {
        if (kbdUsbSendKeyboard(rep)) g_diagEmitOk++;
        else                         g_diagEmitFail++;
    } else {
        g_diagEmitFail++;
    }
}"""
assert OLD in s, "kbdApplyRealReport 未找到"
s = s.replace(OLD, NEW, 1)

# ---------- 2) 去掉我加的键盘去重 (它引入了卡键) ----------
OLD2 = """        static uint8_t s_lastKbdRep[8] = {0};
        static bool    s_lastKbdValid  = false;
        if (s_lastKbdValid && memcmp(rep, s_lastKbdRep, 8) == 0) return;

        if (kbdUsbSendKeyboard(rep)) {
            memcpy(s_lastKbdRep, rep, 8);
            s_lastKbdValid = true;
            g_diagEmitOk++;
        } else {
            g_diagEmitFail++;
        }"""
NEW2 = """        // ★ 已撤销这里的"内容没变就不发"去重。
        //
        //   原因: USB 是弱可靠链路, 一个报文"发出去了"不等于"主机收到了"。
        //   去重会把"已发过"当成"已收到", 一旦某次释放报文主机没收到, 之后
        //   同样的状态永远不再重发 -> 键永久卡在按下状态, 而且占满 6 个键码
        //   槽位导致其它键也没反应。这正是用户实测到的现象。
        //
        //   注入路径的流量本来就很小(每次按键一次), 不需要去重。
        if (kbdUsbSendKeyboard(rep)) g_diagEmitOk++;
        else                         g_diagEmitFail++;"""
assert OLD2 in s, "键盘去重块未找到"
s = s.replace(OLD2, NEW2, 1)

open(p, 'w', encoding='utf-8', newline='').write(s)
print(f"handleCommands.cpp: {n0} -> {len(s)} 字节")
print()
print("验证:")
print("  真键盘直通      :", 'kbdUsbSendKeyboard(rep)' in s and '不写 s_kbd_mod_real' in s)
print("  去重已撤销      :", 's_lastKbdValid' not in s)
print("  鼠标按钮去重保留:", 's_mouseBtnSent' in s)
