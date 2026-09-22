# -*- coding: utf-8 -*-
"""
独立版（键盘马克）"屏蔽 + 注入"端到端验证。

把 fw_device_kbd 的 makcu_link.cpp 逐条搬到 Python:
  - handleCommand()  : 0x21 KEY_MASK / 0x22 KEY_TAP / 0x43 GHOST_MODE
  - makcuSetMask() / makcuClearMask() / 定时解除
  - makcuLinkTick()  : 1ms 取合并快照 -> USB 发给被控机
以及 ascii_cmd.cpp 的 km.mask(N) / km.maskoff 入口。

"主机看到的状态" = 最后一次发射的 8 字节键盘报文。

要证明的（也就是用户提的需求）:
  1) 上位机注入能到被控机                       -> A
  2) 注入和真实输入能共存(不冲突)                -> B
  3) 屏蔽真键后, 被控机看不到真键                 -> C
  4) 屏蔽期间注入照样能到                        -> D
  5) 屏蔽是【短时间】的: N 毫秒后自动恢复          -> E
  6) 屏蔽开始那一刻按着的键会被立刻释放           -> C'
  7) 屏蔽结束后, 一直按着的键会自己回来            -> E'
  8) KEY_TAP 在固件内定时自动抬起(上位机崩了也不卡键) -> F
"""

REPORT_INTERVAL_MS = 1
MASK_MAX_MS = 2000


def st(mod=0, keys=()):
    b = bytearray(8)
    b[0] = mod
    for i, k in enumerate(keys[:6]):
        b[2 + i] = k
    return bytes(b)


ZERO = st()

A, S, D = 0x04, 0x16, 0x07


class Device:
    """复刻 fw_device_kbd: makcu_link.cpp"""

    def __init__(self):
        self.real = ZERO
        self.inj = ZERO
        self.tap = ZERO
        self.tap_until = 0
        self.ghost = False
        self.mask_until = 0

        self.host = ZERO
        self.now = 0
        self.tx_log = []          # (t, 主机看到的状态)
        self._last_emit = -999

    # ---------------- 命令入口 ----------------
    def cmd(self, c, payload=b""):
        """二进制帧命令 (makcuLinkFeedPc -> handleCommand)"""
        if c == 0x21:                                   # KEY_MASK
            if len(payload) >= 7:
                b = bytearray(8)
                b[0] = payload[0]
                b[2:8] = payload[1:7]
                self.inj = bytes(b)
        elif c == 0x22:                                 # KEY_TAP
            if len(payload) >= 4:
                hold = payload[2] | (payload[3] << 8)
                self.tap = st(payload[0], (payload[1],))
                self.tap_until = self.now + hold
        elif c == 0x43:                                 # GHOST_MODE
            if len(payload) >= 1:
                self.ghost = (payload[0] != 0)
                if self.ghost:
                    self.real = ZERO
        elif c == 0x44:                                 # PANIC
            self.inj = ZERO
            self.tap = ZERO
            self.tap_until = 0

    def set_mask(self, ms):
        """ASCII km.mask(N)"""
        if ms <= 0:
            self.mask_until = 0
            return
        ms = min(ms, MASK_MAX_MS)
        self.mask_until = self.now + ms
        if self.mask_until == 0:
            self.mask_until = 1

    def clear_mask(self):
        self.mask_until = 0

    # ---------------- 真实报文 ----------------
    def on_real(self, rep):
        self.real = rep

    # ---------------- 周期发射 ----------------
    def tick(self, t):
        self.now = t
        if t - self._last_emit < REPORT_INTERVAL_MS:
            return
        self._last_emit = t

        # 屏蔽到期自动解除 (解除屏蔽的唯一保证路径)
        if self.mask_until != 0 and (t - self.mask_until) >= 0:
            self.mask_until = 0

        # KEY_TAP 到期自动弹起
        if self.tap_until and (t - self.tap_until) >= 0:
            self.tap_until = 0
            self.tap = ZERO

        drop_real = self.ghost or (self.mask_until != 0)

        # 合并: 注入 + tap + (非屏蔽时)真实
        out = bytearray(self.inj)
        for src in ((self.tap,) if True else ()):
            out[0] |= src[0]
            for i in range(6):
                k = src[2 + i]
                if k and k not in out[2:8]:
                    for j in range(6):
                        if out[2 + j] == 0:
                            out[2 + j] = k
                            break
        if not drop_real:
            out[0] |= self.real[0]
            for i in range(6):
                k = self.real[2 + i]
                if k and k not in out[2:8]:
                    for j in range(6):
                        if out[2 + j] == 0:
                            out[2 + j] = k
                            break

        self.host = bytes(out)
        self.tx_log.append((t, self.host))


def keys_of(b):
    return [hex(x) for x in b[2:] if x]


def press(b, t):
    """主机在 t 之后是否曾经看到 b 所描述的按键全部按下"""
    for tt, h in G.tx_log:
        if tt >= t and all((h[2 + i] == b[2 + i] or b[2 + i] == 0) for i in range(6)) \
           and (h[0] & b[0]) == b[0]:
            return True
    return False


def saw_after(t, pred):
    return [(tt, h) for tt, h in G.tx_log if tt >= t and pred(h)]


ok = True


def check(name, cond, extra=""):
    global ok
    ok &= bool(cond)
    print("  %-44s %s%s" % (name, "OK" if cond else "!! 失败", extra))


def run(fn, ticks=400):
    global G
    G = Device()
    fn(G)
    for t in range(ticks):
        G.tick(t)
    return G


def main():
    print("发射周期 %dms / 屏蔽上限 %dms\n" % (REPORT_INTERVAL_MS, MASK_MAX_MS))

    # ---- A: 纯注入 ----
    def a(g):
        g.on_real(ZERO)
        g.cmd(0x21, bytes([0x00, A, 0, 0, 0, 0, 0]))
    g = run(a)
    check("A 注入 0x21 按下 A -> 被控机看到 A",
          g.host == st(0, (A,)), "  最终=%s" % keys_of(g.host))

    # ---- B: 真实 + 注入 共存 ----
    def b(g):
        g.on_real(st(0, (S,)))
        g.cmd(0x21, bytes([0x00, A, 0, 0, 0, 0, 0]))
    g = run(b)
    check("B 真键S + 注入A 同时存在",
          sorted(keys_of(g.host)) == sorted([hex(A), hex(S)]),
          "  最终=%s" % sorted(keys_of(g.host)))

    # ---- C: 屏蔽真键 -> 真键消失; C': 屏蔽瞬间按着的键立刻释放 ----
    def c(g):
        g.on_real(st(0, (S,)))          # 先按住 S
        g.tick(0)
        g.tick(1)
        held_before = g.host
        g.set_mask(200)                 # 此刻开始屏蔽
    g = run(c, ticks=100)
    # 屏蔽开始那几毫秒内 S 必须被释放
    released = [tt for tt, h in g.tx_log if tt >= 2 and S not in h[2:8]]
    check("C' 屏蔽开始瞬间按着的 S 被释放",
          len(released) > 0 and released[0] <= 4,
          "  释放于 t=%s" % (released[0] if released else "从未"))

    # ---- C: 屏蔽期间新按的真键看不到 ----
    def c2(g):
        g.on_real(ZERO)
        g.tick(0)
        g.set_mask(200)
        g.on_real(st(0, (A,)))          # 屏蔽期间用户真按了 A
    g = run(c2, ticks=150)
    seen = [tt for tt, h in g.tx_log if A in h[2:8]]
    check("C  屏蔽期间的真键 A 完全不出现",
          len(seen) == 0, "  出现次数=%d" % len(seen))

    # ---- E': 屏蔽结束后真实按键自动回来 ----
    #
    #   场景: 用户全程按住 S 不放(接收器持续重发 S), 中途屏蔽 100ms。
    #   期望: 窗口内看不到 S, 窗口结束后 S 立刻恢复 —— 因为屏蔽【不】清空
    #   s_realKbd, 真实报文仍在更新它。这正是"不 clear()"这个设计决策要保证的:
    #   若清空了, 而接收器又是"只在变化时发帧"的类型, 这个键就再也回不来了。
    G = Device()

    def e_prime(g):
        g.on_real(st(0, (S,)))
        g.tick(0)
        g.set_mask(100)                 # 从 t=0 起屏蔽 100ms
    e_prime(G)
    for t in range(1, 400):
        G.on_real(st(0, (S,)))          # 用户一直按着, 接收器持续重发
        G.tick(t)

    during = [tt for tt, h in G.tx_log if 5 <= tt < 100 and S in h[2:8]]
    after = [tt for tt, h in G.tx_log if tt >= 100 and S in h[2:8]]
    check("E' 屏蔽结束后一直按着的 S 自动恢复",
          len(during) == 0 and len(after) > 0 and after[0] <= 102,
          "  窗口内出现%d次, 恢复于 t=%s" % (len(during),
                                            after[0] if after else "从未"))

    # ---- D: 屏蔽期间注入照样生效 ----
    def d(g):
        g.on_real(st(0, (S,)))
        g.tick(0)
        g.set_mask(200)
        g.cmd(0x21, bytes([0x00, A, 0, 0, 0, 0, 0]))   # 屏蔽期间注入 A
    g = run(d, ticks=100)
    # 应该看到 A 出现, 而 S 不出现
    a_seen = [tt for tt, h in g.tx_log if A in h[2:8]]
    s_seen = [tt for tt, h in g.tx_log if S in h[2:8] and tt >= 5]
    check("D  屏蔽期间注入 A 生效", len(a_seen) > 0)
    check("D' 同期真键 S 仍被丢弃", len(s_seen) == 0)

    # ---- E: 屏蔽自动解除 (无解除命令) ----
    def e2(g):
        g.on_real(st(0, (S,)))
        g.tick(0)
        g.set_mask(50)
    g = run(e2, ticks=200)
    # 50ms 之后 S 应该回来 (真实报文一直在更新)
    back = [tt for tt, h in g.tx_log if tt > 50 and S in h[2:8]]
    check("E  km.mask(50) 到点自动解除", len(back) > 0,
          "  恢复于 t=%s" % (back[0] if back else "从未"))

    # ---- F: KEY_TAP 固件内定时抬键 ----
    def f(g):
        g.on_real(ZERO)
        g.cmd(0x22, bytes([0x00, A, 50, 0]))     # hold 50ms
    g = run(f, ticks=200)
    down = [tt for tt, h in g.tx_log if A in h[2:8]]
    up = [tt for tt, h in g.tx_log if tt > 0 and A not in h[2:8]]
    check("F  0x22 KEY_TAP 按下后自动抬起",
          len(down) > 0 and len(up) > 0 and up[0] >= 48,
          "  按下@%s 抬起@%s (hold=50)" % (down[0] if down else "-",
                                          up[0] if up else "-"))

    # ---- G: GNOST(0x43) 持续屏蔽仍然可用 ----
    def gg(g):
        g.on_real(st(0, (S,)))
        g.tick(0)
        g.cmd(0x43, bytes([1]))
    g = run(gg, ticks=100)
    s_seen = [tt for tt, h in g.tx_log if S in h[2:8] and tt >= 5]
    check("G  0x43 GHOST_MODE 持续屏蔽真键", len(s_seen) == 0)

    print()
    print("结果:", "全部通过" if ok else "存在不一致")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
