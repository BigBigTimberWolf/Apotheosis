# -*- coding: utf-8 -*-
"""
handleCommands.cpp 手术:
  把 Arduino 的 Mouse.xxx / Kbd.press|release 换成直写 TinyUSB 的发送函数。

【设计】键盘输出统一走 kbdEmitMerged() 的完整快照路径:
  kbdEmitMerged 已经把 real ∪ inj ∪ tap 合并好了, 是唯一权威状态源。
  原来 handleKeyTapCmd / tap 到期处各自 Kbd.press/release 是【差分】写法,
  改成调用 kbdEmitMerged() —— 状态已在锁内更新, 快照自然正确。
  这样避免"两个地方各自维护按键状态"的隐患。
"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\src\handleCommands.cpp'
s = open(p, encoding='utf-8', errors='replace').read()
n0 = len(s)

# ---------- 1) include ----------
assert '#include "dual_hid.h"' in s, "dual_hid.h include 未找到"
s = s.replace('#include "dual_hid.h"',
              '#include "usb_desc.h"   // 直写 TinyUSB: kbdUsbSendKeyboard / kbdUsbSendMouse', 1)

# ---------- 2) 鼠标三个出口 ----------
old_move = """static void emitMouseMove(int8_t x, int8_t y) {
    if (sendClonedMouseReport(x, y, 0, 0)) return;
    Mouse.move(x, y);
}

static void emitMouseWheel(int8_t w) {
    if (sendClonedMouseReport(0, 0, w, 0)) return;
    Mouse.move(0, 0, w);
}

static void emitMouseButtons(uint8_t mask) {
    if (sendClonedMouseReport(0, 0, 0, mask)) return;

    static const uint8_t kBitMap[5] = {
        MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE,
        MOUSE_BUTTON_FORWARD, MOUSE_BUTTON_BACKWARD };
    for (int i = 0; i < 5; ++i) {
        uint8_t b = 1 << i;
        if (mask & b) Mouse.press(kBitMap[i]);
        else          Mouse.release(kBitMap[i]);
    }
}"""

new_move = """// 鼠标按键状态。
// 原来由 Arduino 的 Mouse 对象内部维护; 现在直写 TinyUSB, 需要自己记。
// 位序与报告描述符一致: bit0=L bit1=R bit2=M bit3=S1(前) bit4=S2(后)
static uint8_t s_mouseBtnState = 0;

static void emitMouseMove(int8_t x, int8_t y) {
    if (sendClonedMouseReport(x, y, 0, 0)) return;
    // 无 Report ID, 一次发全量(按键 + 位移 + 滚轮)
    kbdUsbSendMouse(s_mouseBtnState, x, y, 0);
}

static void emitMouseWheel(int8_t w) {
    if (sendClonedMouseReport(0, 0, w, 0)) return;
    kbdUsbSendMouse(s_mouseBtnState, 0, 0, w);
}

static void emitMouseButtons(uint8_t mask) {
    if (sendClonedMouseReport(0, 0, 0, mask)) return;
    // 传入的 mask 位序与描述符一致 -> 直接采用, 无需逐键 press/release
    s_mouseBtnState = (uint8_t)(mask & 0x1F);
    kbdUsbSendMouse(s_mouseBtnState, 0, 0, 0);
}"""

assert old_move in s, "鼠标三出口块未找到"
s = s.replace(old_move, new_move, 1)

# ---------- 3) 键盘主出口 (kbdEmitMerged 尾部) ----------
old_kbd = """    // ---- 优先走第二个 HID 接口(纯键盘接口, instance 1) ----
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

new_kbd = """    // ===== 直写 TinyUSB: 发【完整快照】 =====
    //
    // 只有一个键盘接口 (Boot Keyboard, 无 Report ID), 所以直接发 8 字节:
    //     byte0 = modifier, byte1 = 保留, byte2..7 = 6 个键码
    //
    // 【为什么发快照而不是差分】
    //   boot keyboard 无 Report ID, 主机每次期望的就是完整状态。
    //   真键盘也是按键一变就重发全量, 所以这与真实行为一致。
    //
    // mergedKeys 是本函数锁内算好的最终状态 (real ∪ inj ∪ tap), 直接用。
    if (kbdUsbReady()) {
        uint8_t rep[8];
        rep[0] = mod;
        rep[1] = 0;
        memcpy(rep + 2, mergedKeys, 6);
        kbdUsbSendKeyboard(rep);
    }

    // 差分的中间量不再使用(保留计算是为了不动锁内逻辑), 显式标记避免告警
    (void)pressMod; (void)releaseMod;
    (void)nPress; (void)nRelease;
    (void)pressKeys; (void)releaseKeys;
}"""

assert old_kbd in s, "键盘主出口块未找到"
s = s.replace(old_kbd, new_kbd, 1)

# ---------- 4) tap 到期释放 (差分 -> 快照) ----------
old_rel = """    // 锁外: 真正写 HID
    for (uint8_t i = 0; i < nRel; ++i) Kbd.release(relKeys[i]);
    for (uint8_t m = 0; m < 8; ++m) {
        if (relMod & (1 << m)) Kbd.release((uint8_t)(0xE0 + m));
    }
}"""
new_rel = """    // 锁外: 状态已在锁内改好(占用计数/槽位已释放),
    // 重新算一次合并快照即可 —— 不再逐键 release, 避免与 kbdEmitMerged 打架。
    (void)nRel; (void)relKeys; (void)relMod;
    kbdEmitMerged();
}"""
assert old_rel in s, "tap 释放块未找到"
s = s.replace(old_rel, new_rel, 1)

# ---------- 5) tap 按下 (差分 -> 快照) ----------
old_press = """    for (uint8_t m = 0; m < 8; ++m) {
        if (mod & (1 << m)) Kbd.press(0xE0 + m);
    }
    Kbd.press(key);
}"""
new_press = """    // 槽位已在锁内登记, 由 kbdEmitMerged 统一算快照发出
    kbdEmitMerged();
}"""
assert old_press in s, "tap 按下块未找到"
s = s.replace(old_press, new_press, 1)

# ---------- 校验: 剥掉注释后检查, 避免被文档文字误判 ----------
def strip_comments(text):
    out = []
    for line in text.split('\n'):
        i = line.find('//')
        if i >= 0:
            line = line[:i]
        out.append(line)
    return '\n'.join(out)

code = strip_comments(s)

bad = []
for pat in ('Mouse.move(', 'Mouse.press(', 'Mouse.release(',
            'Kbd.press(', 'Kbd.release(', 'dualHid'):
    if pat in code:
        bad.append(pat)

assert not bad, f"代码里仍有残留调用: {bad}"

open(p, 'w', encoding='utf-8', newline='').write(s)
print(f"handleCommands.cpp 手术完成: {n0} -> {len(s)} 字节")
print()
print("  真实代码里 Mouse./Kbd. 已清零 :", not bad)
print("  kbdUsbSendMouse   调用次数 :", s.count('kbdUsbSendMouse'))
print("  kbdUsbSendKeyboard 调用次数:", s.count('kbdUsbSendKeyboard'))
print("  kbdEmitMerged()   调用次数 :", s.count('kbdEmitMerged()'))
