# -*- coding: utf-8 -*-
"""
键盘发射改走 instance 1 (第二个 HID 接口)

原逻辑: kbdEmitMerged() 算出差分后逐个调用 Kbd.press()/Kbd.release()。
        Kbd 走 Arduino 的 USBHID SendReport -> tud_hid_n_report(0, ...)
        => 报文全部落在【接口0(Arduino 拼接描述符)】上。

新逻辑: 在调用 Kbd.* 之前, 先尝试用 dualHidSendKeyboard() 把【完整快照】
        经【接口1(纯键盘接口)】发出去。成功则直接返回, 不再走 Kbd.*。

为什么发完整快照而不是差分:
        接口1 的报告描述符是标准 boot keyboard(无 Report ID, 8 字节),
        主机期望每次收到完整状态(modifier + 6 键码), 而不是增量。
        这与真键盘的行为一致 —— 真键盘每次按键变化都重发完整快照。
"""

import sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'fw_device\src\handleCommands.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

OLD = """    if (sendClonedKbdReport(mod, nullptr)) return;

    for (uint8_t m = 0; m < 8; ++m) {
        if (pressMod   & (1 << m)) Kbd.press((uint8_t)(0xE0 + m));
        if (releaseMod & (1 << m)) Kbd.release((uint8_t)(0xE0 + m));
    }
    for (uint8_t i = 0; i < nRelease; ++i) Kbd.release(releaseKeys[i]);
    for (uint8_t i = 0; i < nPress;   ++i) Kbd.press(pressKeys[i]);
}"""

NEW = """    if (sendClonedKbdReport(mod, nullptr)) return;

    // ---- 优先走第二个 HID 接口(纯键盘接口, instance 1) ----
    //
    // 【为什么】Arduino 的 Kbd.press() 最终是 tud_hid_n_report(0, ...), 报文
    // 全部落在【接口0】—— 那是鼠标+键盘拼接的描述符。被控机对真键盘接收器的
    // 期望结构是【多接口复合设备】(usbccgp), 接口0 那种拼接结构不被识别。
    //
    // 本板现在多了一个接口1(见 dual_hid.cpp), 其报告描述符是标准 boot keyboard。
    // 把【完整快照】发到接口1, 结构与真设备一致。
    //
    // 【为什么发快照而不是差分】接口1 无 Report ID, 主机按 boot keyboard 处理,
    // 每次都期望完整状态(modifier + 6 键码)。真键盘也是这样发全量的。
    // keys 数组在上面已经合并好(real ∪ inj), 直接用即可。
    if (dualHidKeyboardReady()) {
        uint8_t snap[6] = {0, 0, 0, 0, 0, 0};
        // keys[] 是本函数内合并后的最终状态, 定义在上面锁内
        memcpy(snap, mergedKeys, 6);
        if (dualHidSendKeyboard(mod, snap)) {
            return;     // 已从接口1发出, 不再走接口0
        }
    }

    // ---- 回退: 接口1 不可用时走 Arduino 的接口0(差分式) ----
    for (uint8_t m = 0; m < 8; ++m) {
        if (pressMod   & (1 << m)) Kbd.press((uint8_t)(0xE0 + m));
        if (releaseMod & (1 << m)) Kbd.release((uint8_t)(0xE0 + m));
    }
    for (uint8_t i = 0; i < nRelease; ++i) Kbd.release(releaseKeys[i]);
    for (uint8_t i = 0; i < nPress;   ++i) Kbd.press(pressKeys[i]);
}"""

assert OLD in s, "kbdEmitMerged tail not found"
s = s.replace(OLD, NEW, 1)

# 需要在锁内把合并结果留到锁外使用 -> 新增 mergedKeys
OLD2 = """        // 提交: prev 与新状态一致
        memcpy(s_kbd_keys_prev, keys, 6);
        s_kbd_mod_prev = mod;
        committed = true;
        xSemaphoreGive(s_kbd_mtx);"""
NEW2 = """        // 提交: prev 与新状态一致
        memcpy(s_kbd_keys_prev, keys, 6);
        s_kbd_mod_prev = mod;
        committed = true;

        // 把合并结果带出临界区, 供锁外从接口1发完整快照用(见下方)
        memcpy(mergedKeys, keys, 6);

        xSemaphoreGive(s_kbd_mtx);"""

assert OLD2 in s, "commit block not found"
s = s.replace(OLD2, NEW2, 1)

# 声明 mergedKeys
OLD3 = """    uint8_t  nPress = 0, nRelease = 0;
    uint8_t  mod = 0;
    bool     committed = false;"""
NEW3 = """    uint8_t  nPress = 0, nRelease = 0;
    uint8_t  mod = 0;
    bool     committed = false;
    uint8_t  mergedKeys[6] = {0,0,0,0,0,0};   // 合并后的最终键码状态(供接口1发快照)"""

assert OLD3 in s, "locals block not found"
s = s.replace(OLD3, NEW3, 1)

# include
if 'dual_hid.h' not in s:
    s = s.replace('#include "USBSetup.h"', '#include "USBSetup.h"\n#include "dual_hid.h"', 1)

open(p, 'w', encoding='utf-8', newline='').write(s)
print("kbdEmitMerged 已改走接口1 (完整快照), 并保留接口0 回退")
