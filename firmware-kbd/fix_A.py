# -*- coding: utf-8 -*-
"""
A 修复 + 诊断计数器

【A 修的是什么】
  我把 Arduino 的 Mouse.move()/Kbd.press() 换成直写 TinyUSB 时, 丢掉了原来
  「等报文真正发完」的语义:
    - Arduino SendReport(): 调 tud_hid_n_report 后【等信号量】确认发完
    - 我的 kbdUsbSendMouse(): 调一次就返回, 端点忙就 false
  而调用方又丢掉了返回值 -> 位移被静默吞掉, 还照旧记账。

  修法:
    1) usb_hid.cpp: 发送前【等端点空闲】(带超时), 失败才算真失败
    2) handleCommands.cpp: 出口返回 bool; 失败时把位移【还回队列】, 不记账

【同时加诊断计数器】
  左板是链路下游, 装计数器就能一次定位断点在右板还是在发送口。
"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

cmds = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_device\src\handleCommands.cpp'
s = open(cmds, encoding='utf-8', errors='replace').read()
n0 = len(s)

# ===========================================================================
# 1) 三个出口改为返回 bool, 并加计数
# ===========================================================================
old_emit = """static void emitMouseMove(int8_t x, int8_t y) {
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

new_emit = """// ===== 诊断计数器 (供 main.cpp 心跳打印) =====
// 左板是链路下游: 同时能看到「右板送来多少」和「本级发出多少」,
// 因此这三个数就能定位断点在右板还是在发送口。
uint32_t volatile g_diagMoveRx   = 0;   // 收到真实鼠标位移帧数
uint32_t volatile g_diagBtnRx    = 0;   // 收到真实鼠标按键帧数
uint32_t volatile g_diagKbdRx    = 0;   // 收到真实键盘报文数
uint32_t volatile g_diagEmitOk   = 0;   // 成功发出的 HID 报文数
uint32_t volatile g_diagEmitFail = 0;   // 发送失败(超时/端点忙)次数
uint32_t volatile g_diagMoved    = 0;   // 累计已发出的位移量(绝对值)

// ★ 出口改为返回 bool
//
// 【为什么必须返回】
//   原来 void 版本丢掉发送结果, 调用方一律按"已发出"记账。
//   而直写 TinyUSB 之后, 发送【可能在端点忙时失败】—— 于是位移被静默吞掉,
//   km.getpos / km.moveto 的坐标也跟着错。失败必须让调用方知道。
static bool emitMouseMove(int8_t x, int8_t y) {
    if (sendClonedMouseReport(x, y, 0, 0)) { g_diagEmitOk++; return true; }
    // 无 Report ID, 一次发全量(按键 + 位移 + 滚轮)
    const bool ok = kbdUsbSendMouse(s_mouseBtnState, x, y, 0);
    if (ok) { g_diagEmitOk++; g_diagMoved += (uint32_t)((x < 0 ? -x : x) + (y < 0 ? -y : y)); }
    else    { g_diagEmitFail++; }
    return ok;
}

static bool emitMouseWheel(int8_t w) {
    if (sendClonedMouseReport(0, 0, w, 0)) { g_diagEmitOk++; return true; }
    const bool ok = kbdUsbSendMouse(s_mouseBtnState, 0, 0, w);
    if (ok) g_diagEmitOk++; else g_diagEmitFail++;
    return ok;
}

static bool emitMouseButtons(uint8_t mask) {
    if (sendClonedMouseReport(0, 0, 0, mask)) { g_diagEmitOk++; return true; }
    // 传入的 mask 位序与描述符一致 -> 直接采用, 无需逐键 press/release
    s_mouseBtnState = (uint8_t)(mask & 0x1F);
    const bool ok = kbdUsbSendMouse(s_mouseBtnState, 0, 0, 0);
    if (ok) g_diagEmitOk++; else g_diagEmitFail++;
    return ok;
}"""

assert old_emit in s, "三个出口块未找到"
s = s.replace(old_emit, new_emit, 1)

# ===========================================================================
# 2) handleMove: 失败时把位移还回队列, 不记账
# ===========================================================================
old_fast = """    if (x >= -127 && x <= 127 && y >= -127 && y <= 127) {
        emitMouseMove(static_cast<int8_t>(x), static_cast<int8_t>(y));
        sentX = x; sentY = y;
    } else {"""
new_fast = """    if (x >= -127 && x <= 127 && y >= -127 && y <= 127) {
        // ★ 只有真正发出去才记账; 发不出去就还回队列, 由 mouseMoveTask 稍后重发
        if (emitMouseMove(static_cast<int8_t>(x), static_cast<int8_t>(y))) {
            sentX = x; sentY = y;
        } else {
            s_pending_dx.fetch_add(x, std::memory_order_relaxed);
            s_pending_dy.fetch_add(y, std::memory_order_relaxed);
            if (mouseMoveTaskHandle != NULL) xTaskNotifyGive(mouseMoveTaskHandle);
            return;
        }
    } else {"""
assert old_fast in s, "handleMove 快路径未找到"
s = s.replace(old_fast, new_fast, 1)

old_step = """            int8_t stepX = (remainingX > 127) ? 127 : ((remainingX < -127) ? -127 : (int8_t)remainingX);
            int8_t stepY = (remainingY > 127) ? 127 : ((remainingY < -127) ? -127 : (int8_t)remainingY);
            emitMouseMove(stepX, stepY);
            sentX += stepX; sentY += stepY;
            remainingX -= stepX; remainingY -= stepY;"""
new_step = """            int8_t stepX = (remainingX > 127) ? 127 : ((remainingX < -127) ? -127 : (int8_t)remainingX);
            int8_t stepY = (remainingY > 127) ? 127 : ((remainingY < -127) ? -127 : (int8_t)remainingY);
            if (!emitMouseMove(stepX, stepY)) {
                // ★ 发不出去: 把剩下的余额(含本步)全还给队列, 不记账
                s_pending_dx.fetch_add(remainingX, std::memory_order_relaxed);
                s_pending_dy.fetch_add(remainingY, std::memory_order_relaxed);
                if (mouseMoveTaskHandle != NULL) xTaskNotifyGive(mouseMoveTaskHandle);
                break;
            }
            sentX += stepX; sentY += stepY;
            remainingX -= stepX; remainingY -= stepY;"""
assert old_step in s, "handleMove 多步拆分未找到"
s = s.replace(old_step, new_step, 1)

# ===========================================================================
# 3) handleMouseWheel: 失败时余量回队列
# ===========================================================================
old_wheel = """        int step = (wheelMovement > 127) ? 127 : ((wheelMovement < -127) ? -127 : wheelMovement);
        emitMouseWheel(static_cast<int8_t>(step));
        int32_t rest = wheelMovement - step;
        if (rest != 0) {"""
new_wheel = """        int step = (wheelMovement > 127) ? 127 : ((wheelMovement < -127) ? -127 : wheelMovement);
        if (!emitMouseWheel(static_cast<int8_t>(step))) {
            // ★ 发不出去 -> 整份(含本步)还回队列
            s_pending_wheel.fetch_add(wheelMovement, std::memory_order_relaxed);
            if (mouseMoveTaskHandle != NULL) xTaskNotifyGive(mouseMoveTaskHandle);
            return;
        }
        int32_t rest = wheelMovement - step;
        if (rest != 0) {"""
assert old_wheel in s, "handleMouseWheel 快路径未找到"
s = s.replace(old_wheel, new_wheel, 1)

# ===========================================================================
# 4) 接收侧计数 (真实设备链路)
# ===========================================================================
old_onmove = """    g_proto1.onMove = [](int16_t dx, int16_t dy) {"""
new_onmove = """    g_proto1.onMove = [](int16_t dx, int16_t dy) {
        g_diagMoveRx++;"""
assert old_onmove in s, "g_proto1.onMove 未找到"
s = s.replace(old_onmove, new_onmove, 1)

old_onbtn = """    g_proto1.onButtonMask = [](uint8_t mask) {"""
new_onbtn = """    g_proto1.onButtonMask = [](uint8_t mask) {
        g_diagBtnRx++;"""
assert old_onbtn in s, "g_proto1.onButtonMask 未找到"
s = s.replace(old_onbtn, new_onbtn, 1)

old_onkb = """    g_proto1.onKbReport = [](uint8_t mod, const uint8_t keys6[6]) {
        kbdApplyRealReport(mod, keys6);"""
new_onkb = """    g_proto1.onKbReport = [](uint8_t mod, const uint8_t keys6[6]) {
        g_diagKbdRx++;
        kbdApplyRealReport(mod, keys6);"""
assert old_onkb in s, "g_proto1.onKbReport 未找到"
s = s.replace(old_onkb, new_onkb, 1)

# ===========================================================================
# 5) 键盘出口也计数
# ===========================================================================
old_kbd_emit = """    if (kbdUsbReady()) {
        uint8_t rep[8];
        rep[0] = mod;
        rep[1] = 0;
        memcpy(rep + 2, mergedKeys, 6);
        kbdUsbSendKeyboard(rep);
    }"""
new_kbd_emit = """    if (kbdUsbReady()) {
        uint8_t rep[8];
        rep[0] = mod;
        rep[1] = 0;
        memcpy(rep + 2, mergedKeys, 6);
        if (kbdUsbSendKeyboard(rep)) g_diagEmitOk++;
        else                         g_diagEmitFail++;
    } else {
        g_diagEmitFail++;
    }"""
assert old_kbd_emit in s, "键盘出口块未找到"
s = s.replace(old_kbd_emit, new_kbd_emit, 1)

open(cmds, 'w', encoding='utf-8', newline='').write(s)
print(f"handleCommands.cpp: {n0} -> {len(s)} 字节")
print()
print("验证:")
print("  出口返回 bool      :", s.count('static bool emitMouse'))
print("  失败还回队列       :", s.count('s_pending_dx.fetch_add'))
print("  诊断计数器         :", s.count('g_diag'))
