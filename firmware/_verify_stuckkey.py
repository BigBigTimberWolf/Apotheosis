# -*- coding: utf-8 -*-
"""
卡键回归测试: 模拟"USB 端点偶发抢不到"时, 主机最终看到什么状态。

背景(用户实测): 敲一串键, l/h/d 等整段卡住无限重复 —— PC 认为那些键一直按着。

根因: 键盘是【快照】语义, 主机只认最后一帧。抬键帧一旦被静默丢弃, 主机就永远
认为那个键还按着。而抬键帧恰恰是最容易被丢的那一帧, 因为它是"最后一阵打字"的
收尾帧: 按住期间接收器会不停重发同一状态(丢一帧无所谓), 但松开之后接收器就安静
了 —— 那一帧丢了就没有下一帧来纠正。

对比两个模型:
  旧: 收到一帧就立刻发一次, 端点忙就丢那一帧          -> 抬键帧丢掉 = 永久卡键
  新: 收到一帧只更新 desired; 每 1ms tick 把 desired
      发到【确认送出】为止(失败不记账)                -> 状态必然收敛

USB 端点模型: 一帧从排队到被主机取走需要一个轮询周期, 所以两次成功发送之间至少
隔 ENDPOINT_GAP_MS 毫秒; 该间隔内的发送尝试一律失败(对应 waitEndpointReady()
等待 1ms 后返回 false)。
"""

TICKS = 500
ENDPOINT_GAP_MS = 2      # 两次成功发送之间的最小间隔(轮询周期 + 余量)
REAFFIRM_MS = 100        # 新模型里周期性重申的间隔


def build_reports(events):
    """展开接收器实际发出的报文流。

    events: [(时刻ms, 8字节状态)] —— 状态变化点。
    规则(贴近真实键盘接收器):
      - 状态非空: 每 1ms 重发一次同一状态(1000Hz 接收器的典型行为,
        按住期间不停重发)
      - 状态为空: 只发一帧, 之后安静(松开后接收器不再发东西)
    """
    out = []
    ev = sorted(events)
    for i, (t, st) in enumerate(ev):
        end = ev[i + 1][0] if i + 1 < len(ev) else TICKS
        if any(st):
            t2 = t
            while t2 < min(end, TICKS):
                out.append((t2, st))
                t2 += 1
        else:
            if t < TICKS:
                out.append((t, st))          # 只有这一帧, 丢了就没人纠正
    return out


class Endpoint:
    """每 gap ms 才接受一次发送"""
    def __init__(self, gap):
        self.gap = gap
        self.last_ok = -999
        self.host = bytes(8)        # 主机当前看到的状态
        self.accepts = 0
        self.rejects = 0

    def try_send(self, t, rep):
        if t - self.last_ok < self.gap:
            self.rejects += 1
            return False
        self.host = rep            # 端点接受 == 主机下一次轮询取走
        self.last_ok = t
        self.accepts += 1
        return True


def old_model(all_reports):
    """旧: 收到一帧就立刻发一次, 失败即丢"""
    ep = Endpoint(ENDPOINT_GAP_MS)
    for t, rep in all_reports:
        ep.try_send(t, rep)
    return ep


def new_model(all_reports):
    """新: desired 更新 + 每 tick 重试到确认送出"""
    ep = Endpoint(ENDPOINT_GAP_MS)
    by_tick = {}
    for t, rep in all_reports:
        by_tick.setdefault(t, []).append(rep)

    desired, desired_valid = bytes(8), False
    sent, sent_valid = bytes(8), False
    last_send = -999

    for t in range(TICKS):
        # 报文到达 -> 只更新 desired (按到达顺序, 最后一帧为准)
        for rep in by_tick.get(t, []):
            desired, desired_valid = rep, True

        # 1ms tick -> 尝试把 desired 送到确认为止
        need = desired_valid and (not sent_valid or sent != desired
                                 or (t - last_send) >= REAFFIRM_MS)
        if not need:
            continue
        if ep.try_send(t, desired):
            sent, sent_valid, last_send = desired, True, t
        # 失败不记账 -> 下一 tick 继续重试
    return ep


def main():
    A, S, D, Z, X, C, L, H = 0x04, 0x16, 0x07, 0x1D, 0x1B, 0x06, 0x0F, 0x0B

    def st(*keys, mod=0):
        b = bytearray(8)
        b[0] = mod
        for i, k in enumerate(keys[:6]):
            b[2 + i] = k
        return bytes(b)

    # 用户自述的那串: asdzxczxcasd... 中间 l/h/d 被卡住重复
    events = [
        (0,   st()),
        (5,   st(A)),
        (25,  st(A, S)),
        (45,  st(A, S, D)),
        (70,  st(A, S, D, Z)),
        (95,  st(A, S, D, X)),
        (120, st(A, S, D, X, C)),
        (150, st(L)),                # 前一段收尾, 只按 l
        (300, st(L, H)),
        (420, st(H)),
        (470, st()),                 # ★ 全部松开的收尾帧 —— 唯一一帧, 丢了就卡死
    ]
    reports = build_reports(events)

    expected = bytes(8)

    def fmt(b):
        return "mod=%02X keys=%s" % (b[0], " ".join("%02X" % x for x in b[2:]))

    def stuck(b):
        s = ["mod=%02X" % b[0]] if b[0] else []
        s += ["key=%02X" % x for x in b[2:] if x]
        return s

    old_ep = old_model(reports)
    new_ep = new_model(reports)

    print("接收器实际发出 %d 帧 (端点最小间隔 %dms, 共 %dms)"
          % (len(reports), ENDPOINT_GAP_MS, TICKS))
    print()
    print("旧模型(每帧立刻发, 忙则丢):")
    print("  发送成功 %d / 被拒 %d" % (old_ep.accepts, old_ep.rejects))
    print("  主机最终 = %s" % fmt(old_ep.host))
    st_old = stuck(old_ep.host)
    print("  卡住的键: %s" % (", ".join(st_old) if st_old else "无"))
    print()
    print("新模型(desired/sent 分离 + 1ms 补发):")
    print("  发送成功 %d / 被拒 %d (被拒后重试, 状态不丢)"
          % (new_ep.accepts, new_ep.rejects))
    print("  主机最终 = %s" % fmt(new_ep.host))
    st_new = stuck(new_ep.host)
    print("  卡住的键: %s" % (", ".join(st_new) if st_new else "无"))
    print()

    ok = (new_ep.host == expected)
    old_bad = (old_ep.host != expected)
    print("新模型收敛到全松开:            %s" % ("OK" if ok else "失败"))
    print("旧模型确实复现了卡键:          %s" % ("OK" if old_bad else "没复现"))
    return 0 if (ok and old_bad) else 1


if __name__ == "__main__":
    raise SystemExit(main())
