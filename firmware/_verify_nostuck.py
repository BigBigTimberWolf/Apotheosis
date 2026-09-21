# -*- coding: utf-8 -*-
"""
端到端"不再永久卡键"验证。

链路模型:
  真实接收器 --USB--> [右板] --Serial1--> [左板] --USB--> 主机

两条链路的可靠性完全不同, 这是整个设计的根据:
  * 接收器 -> 右板 走 USB: 可靠总线, IN 端点靠 NAK/ACK 重试, 设备发过的报文不会丢。
    => 右板记下的 s_kbdRawLast 就是设备的【真实状态】。
  * 右板 -> 左板 走 Serial1: 普通串口, 【没有重传】。丢帧是真实存在的。

右板每 50ms 做两件事(kbdLinkTick):
  (1) 心跳: 0x23 帧 + payload=[0xFF], 【不带按键状态】, 只表示"链路活着"
  (2) 状态重申: 把 s_kbdRawLast 再送一遍
  (1) 让左板能安全地区分"链路死了"和"用户一直按着":
        心跳在来   -> 链路活着, 收不到新帧只是因为一直按着 => 绝不能释放(会打断长按)
        心跳也没了 -> 链路死了                                  => 可以安全地全部释放
  (2) 兜住 Serial1 丢帧 —— 因为右板的状态是可信的, 重申不会维持错误状态。

左板(kbdTick):
  - 确认送出 + 1ms 补发: s_kbd_sent 只在发送成功后才更新, 没送出去就一直重试
  - 链路判死: 心跳消失 400ms -> 清零真实侧并写一帧

必须同时成立:
  不能永久卡键            -> A/B/C/D/F/G 最终都回到全松开
  不能误释放按住的键      -> E(一直按住 + 链路正常)必须保持按住
"""

HEARTBEAT_MS = 50     # 右板心跳/重申间隔
TIMEOUT_MS = 400      # 左板判死阈值 (8 倍余量, 容忍连续丢 7 个心跳)
EP_GAP_MS = 2         # 左板 USB 端点两次成功发送之间的最小间隔
TICKS = 3000


KA, KS, KD = 0x04, 0x16, 0x07


def st(*keys):
    b = bytearray(8)
    for i, k in enumerate(keys[:6]):
        b[2 + i] = k
    return bytes(b)


ZERO = bytes(8)


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc


class RightBoard:
    """USB 侧可靠; Serial1 侧可丢帧; 每 50ms 心跳 + 状态重申"""

    def __init__(self, s1_loss=0.0, iface_switch_at=None, link_dead_at=None):
        self.s1_loss = s1_loss
        self.iface_switch_at = iface_switch_at
        self.link_dead_at = link_dead_at
        self.state = ZERO
        self.iface = -1
        self.next_hb = 0
        self.wire = []            # (t, 是否心跳, 状态)
        self.switch_cleared = None

    def _put(self, t, rep, is_hb=False):
        if self.link_dead_at is not None and t >= self.link_dead_at:
            return                                  # 链路彻底死掉: 什么都到不了左板
        if self.s1_loss and not is_hb:
            # 用状态内容做确定性"丢帧判定", 保证可复现
            if (crc16(bytes([t & 0xFF]) + rep) % 100) < int(self.s1_loss * 100):
                return
        self.wire.append((t, is_hb, rep))

    def on_report(self, t, rep):
        # 接口切换: 先补全空清残留, 再切
        iface = 1 if (self.iface_switch_at is not None
                      and t >= self.iface_switch_at) else 0
        if self.iface >= 0 and self.iface != iface:
            self._put(t, ZERO)
            self.switch_cleared = t
        self.iface = iface
        self.state = rep
        self._put(t, rep)

    def tick(self, t):
        if t < self.next_hb:
            return
        self.next_hb = t + HEARTBEAT_MS
        self._put(t, ZERO, is_hb=True)              # 心跳: 不带状态
        if self.iface >= 0:
            self._put(t, self.state)                # 状态重申


class LeftBoard:
    def __init__(self):
        self.real = ZERO
        self.link_ms = 0
        self.link_seen = False
        self.desired, self.desired_valid = ZERO, False
        self.sent, self.sent_valid = ZERO, False
        self.last_send = -999
        self.host = ZERO
        self.rejects = 0
        self.forced = None

    def on_state(self, t, rep):
        self.real = rep
        self.link_ms = t
        self.link_seen = True
        self.desired, self.desired_valid = rep, True

    def on_heartbeat(self, t):
        self.link_ms = t
        self.link_seen = True

    def tick(self, t):
        if self.link_seen and (t - self.link_ms) >= TIMEOUT_MS:
            self.link_seen = False
            if any(self.real):
                self.real = ZERO
                self.desired, self.desired_valid = ZERO, True
                self.forced = t
        if not self.desired_valid:
            return
        need = (not self.sent_valid or self.sent != self.desired
                or (t - self.last_send) >= 100)
        if not need:
            return
        if t - self.last_send < EP_GAP_MS:
            self.rejects += 1
            return                                   # 不记账 -> 下个 tick 重试
        self.host = self.desired
        self.sent, self.sent_valid, self.last_send = self.desired, True, t


def run(events, s1_loss=0.0, iface_switch_at=None, link_dead_at=None,
        ticks=TICKS, continuous_repeat=False):
    rb = RightBoard(s1_loss=s1_loss, iface_switch_at=iface_switch_at,
                    link_dead_at=link_dead_at)
    lb = LeftBoard()

    arrivals = {t: [] for t in range(ticks)}
    ev = sorted(events)
    for i, (t, s) in enumerate(ev):
        end = ev[i + 1][0] if i + 1 < len(ev) else ticks
        if continuous_repeat and any(s):
            for t2 in range(t, min(end, ticks)):
                arrivals[t2].append(s)
        elif t < ticks:
            arrivals[t].append(s)

    for t in range(ticks):
        for rep in arrivals[t]:
            rb.on_report(t, rep)
        rb.tick(t)
        while rb.wire and rb.wire[0][0] <= t:
            _, is_hb, rep = rb.wire.pop(0)
            if is_hb:
                lb.on_heartbeat(t)
            else:
                lb.on_state(t, rep)
        lb.tick(t)
    return rb, lb


def fmt(b):
    return "mod=%02X keys=%s" % (b[0], " ".join("%02X" % x for x in b[2:]))


def keys_of(b):
    return " ".join("%02X" % x for x in b[2:] if x)


def main():
    ok = True
    print("心跳/重申 %dms | 判死 %dms | 左板端点间隔 %dms\n"
          % (HEARTBEAT_MS, TIMEOUT_MS, EP_GAP_MS))

    basic = [(0, ZERO), (5, st(KA)), (60, st(KA, KD)), (150, ZERO),
             (400, st(KS)), (460, ZERO)]
    cases = [
        ("A 正常打字",              dict()),
        ("B Serial1 丢帧 20%",      dict(s1_loss=0.20)),
        ("C Serial1 丢帧 20%+设备连续重发", dict(s1_loss=0.20, continuous_repeat=True)),
        ("D 转发接口中途切换",       dict(iface_switch_at=700)),
    ]
    for name, kw in cases:
        ev = [(0, ZERO), (5, st(KA)), (300, st(KA, KS)), (600, st(KA)), (900, ZERO)] \
             if "接口" in name else basic
        rb, lb = run(ev, **kw)
        good = (lb.host == ZERO)
        ok &= good
        extra = "  清残留@%s" % (rb.switch_cleared,) if rb.switch_cleared else ""
        print("  %-30s 主机最终=%s 卡住=%-6s %s%s"
              % (name, fmt(lb.host), keys_of(lb.host) or "无",
                 "OK" if good else "!! 仍卡键", extra))

    # ---- E: 一直按住 + 链路正常 -> 不能被误释放 ----
    #   设备只在变化时发帧(按一下之后就没有新帧了), 心跳正常。
    #   左板绝不能因为"收不到新帧"而释放 —— 那会打断长按。
    rb, lb = run([(0, ZERO), (10, st(KA))], ticks=2500)
    good = (lb.host == st(KA)) and lb.forced is None
    ok &= good
    print("  %-30s 主机最终=%s 保持=%-6s %s (误释放@%s)"
          % ("E 一直按住(链路正常)", fmt(lb.host), keys_of(lb.host) or "无",
             "OK" if good else "!! 被误释放", lb.forced))

    # ---- F: 链路彻底死掉 -> 必须在阈值内释放 ----
    rb, lb = run([(0, ZERO), (10, st(KA))], link_dead_at=100, ticks=2000)
    good = (lb.host == ZERO and lb.forced is not None
            and lb.forced <= 100 + TIMEOUT_MS + 2)
    ok &= good
    print("  %-30s 断开@100ms 强制释放@%sms 主机最终=%s %s"
          % ("F 链路彻底死掉", lb.forced, fmt(lb.host),
             "OK" if good else "!! 未按时释放"))

    # ---- G: 抬键帧在 Serial1 丢掉, 且设备随后安静 ----
    #   这是"状态重申"存在的理由: 右板侧状态是可信的(全松开), 重申把它补上。
    #   同时做一个【反向对照】: 关掉重申 -> 必须复现永久卡键, 否则说明重申
    #   不是承重的, 这段逻辑就是装饰。
    def run_G(with_reaffirm):
        rb2 = RightBoard()
        lb2 = LeftBoard()
        rb2.reaffirm = with_reaffirm
        orig_tick = rb2.tick

        def tick(t, _orig=orig_tick):
            if t < rb2.next_hb:
                return
            rb2.next_hb = t + HEARTBEAT_MS
            rb2._put(t, ZERO, is_hb=True)                 # 心跳始终有(链路活着)
            if rb2.reaffirm and rb2.iface >= 0:
                rb2._put(t, rb2.state)                    # 状态重申
        rb2.tick = tick

        for t in range(1200):
            if t == 5:
                rb2.on_report(t, st(KA))
            elif t == 200:
                rb2.state = ZERO          # USB 可靠: 右板确实拿到了抬键
                # 但 Serial1 丢掉这一帧 -> 故意不放进 wire
            rb2.tick(t)
            while rb2.wire and rb2.wire[0][0] <= t:
                _, is_hb, rep = rb2.wire.pop(0)
                if is_hb:
                    lb2.on_heartbeat(t)
                else:
                    lb2.on_state(t, rep)
            lb2.tick(t)
        return lb2

    on = run_G(True)
    off = run_G(False)
    good = (on.host == ZERO) and (off.host != ZERO)
    ok &= good
    print("  %-30s 加重申=%s %s   不加重申=%s %s"
          % ("G 抬键帧在Serial1丢掉",
             "全松开" if on.host == ZERO else "卡键",
             "OK" if on.host == ZERO else "!! 失败",
             "全松开" if off.host == ZERO else "卡键(%s)" % keys_of(off.host),
             "OK(证明重申承重)" if off.host != ZERO else "!! 重申没起作用"))

    print()
    print("结果:", "全部通过" if ok else "存在不一致")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
