# -*- coding: utf-8 -*-
"""
ASCII 握手层验证 —— 回答"Apotheosis 到底能不能连上"。

本脚本把两侧都按源码逐条搬过来, 做端到端对拍:
  上位机侧: Apotheosis/Apotheosis/mouse/MakcuNew.cpp
              - probeAscii()      : 发 "km.version\\r\\n", 500ms 内等一行含 tag
              - feedAsciiByte()   : 收字节如何切分成行  ← 决定了应答格式约束
  固件侧:   KBD_PASSTHROUGH/fw_device_kbd/src/ascii_cmd.cpp
              - asciiCmdFeed() / handleLine()
            以及 makcu_link.cpp 里的双通道仲裁 + 定时屏蔽

不碰硬件。目的: 在让你刷之前就确定握手能过。
"""

import re

TAG = "MAKCU-PASSTHROUGH"
VERSION_REPLY = "MAKCU-PASSTHROUGH-1.0.0"
MASK_MAX_MS = 2000
PC_IDLE_RESET_MS = 100


# ============================================================ 上位机侧
class Host:
    """复刻 MakcuNew.cpp 的接收分类 + probeAscii"""

    def __init__(self):
        self.accum = []
        self.line = ""
        self.seq = 0
        self.physical_button_bytes = []      # b < 0x20 的字节会被当成按键上报

    def feed(self, b):
        if b == 0x0A:                        # '\n'
            if self.accum:
                self.line = "".join(self.accum)
                self.accum = []
                self.seq += 1
            return
        if b == 0x0D:                        # '\r' 忽略
            return
        if 0x20 <= b < 0x7F:                 # 可打印 -> 累积
            if len(self.accum) < 256:
                self.accum.append(chr(b))
            return
        if b < 0x20:                         # ★ 控制字符被当作物理按键上报
            self.physical_button_bytes.append(b)
            return
        self.accum = []                      # >= 0x7F -> 清空累积

    def probe(self, firmware_tx, command="km.version", timeout_ms=500):
        """返回 (是否成功, 观察到的行列表)。真实实现是异步等待, 这里用
        '命令写入后固件产生的所有字节' 作为时间窗口内的输入。"""
        base_seq = self.seq
        self.line = ""
        self.accum = []
        self.feed_bytes(firmware_tx)
        if self.seq != base_seq and TAG in self.line:
            return True, [self.line]
        return False, [self.line]

    def feed_bytes(self, data):
        for b in data:
            self.feed(b)


# ============================================================ 固件侧
class Firmware:
    """复刻 ascii_cmd.cpp + makcu_link.cpp"""

    def __init__(self):
        self.line_buf = []
        self.active = False
        self.handshaked = False
        self.tx = bytearray()

        # 双通道仲裁状态 (makcuLinkFeedPc 的 s_pcLen)
        self.pc_len = 0
        self.pc_buf = []
        self.pc_last_ms = 0

        # 屏蔽
        self.mask_until = 0
        self.ghost = False

        self.now = 0

    # ---------------- ASCII 通道 ----------------
    def _send_line(self, s):
        self.tx += s.encode("ascii") + b"\r\n"

    def _send_tracked(self, orig, result):
        h = orig.find("#")
        if h >= 0:
            m = re.match(r"\d+", orig[h + 1:])
            cid = int(m.group()) if m else 0
            self.tx += (">>> #%d:%s\r\n" % (cid, result)).encode("ascii")
        else:
            self._send_line(result)

    def ascii_feed(self, b):
        if b == 0x0A:
            if self.line_buf:
                s = "".join(self.line_buf)
                self.active = True
                self._handle_line(s)
            self.line_buf = []
            return
        if b == 0x0D:
            return
        if 0x20 <= b < 0x7F:
            if not self.line_buf:
                self.active = True
            if len(self.line_buf) < 127:
                self.line_buf.append(chr(b))
            else:
                self.line_buf = []
            return
        self.line_buf = []

    def _handle_line(self, line):
        if line.startswith("km.version"):
            self.handshaked = True
            self._send_tracked(line, VERSION_REPLY)
            return
        if line.startswith("km.maskoff"):
            self.mask_until = 0
            return
        if line.startswith("km.mask("):
            m = re.match(r"-?\d+", line[8:])
            if m:
                ms = int(m.group())
                if ms <= 0:
                    self.mask_until = 0
                else:
                    ms = min(ms, MASK_MAX_MS)
                    self.mask_until = self.now + ms
                    if self.mask_until == 0:
                        self.mask_until = 1
            return
        if line.startswith("km.buttons"):
            self._send_tracked(line, "OK")
            return
        # 其它 km.* 静默忽略

    # ---------------- 双通道仲裁 + 二进制解析 ----------------
    def _frame_need(self):
        if self.pc_len >= 5:
            return 5 + self.pc_buf[2] + 2
        return None

    def feed_pc(self, b):
        # 残帧空闲复位 (100ms)
        if self.pc_len != 0 and (self.now - self.pc_last_ms) > PC_IDLE_RESET_MS:
            self.pc_len = 0
        self.pc_last_ms = self.now

        if self.pc_len == 0:
            self.ascii_feed(b)

        if self.pc_len == 0:
            if b != 0xA5:
                return
            self.pc_buf = [b]
            self.pc_len = 1
            return
        if self.pc_len == 1:
            if b == 0x5C:
                self.pc_buf.append(b)
                self.pc_len = 2
            elif b == 0xA5:
                self.pc_buf = [b]
            else:
                self.pc_len = 0
            return

        self.pc_buf.append(b)
        self.pc_len += 1

        if self.pc_len == 5 and self.pc_buf[2] > 32:
            self.pc_len = 0
            return
        if self.pc_len >= 5:
            need = 5 + self.pc_buf[2] + 2
            if self.pc_len == need:
                self.pc_len = 0        # 这里简化: 不校验 CRC, 只关心仲裁行为
            elif self.pc_len > need:
                self.pc_len = 0

    # ---------------- 时间推进 ----------------
    def tick(self, t):
        self.now = t
        if self.mask_until != 0 and (t - self.mask_until) >= 0:
            self.mask_until = 0

    def take_tx(self):
        d = bytes(self.tx)
        self.tx = bytearray()
        return d


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc


def pack_frame(cmd, payload=b"", seq=0):
    plen = len(payload)
    f = bytearray([0xA5, 0x5C, plen, seq & 0xFF, cmd]) + bytearray(payload)
    crc = crc16(f[2:])
    f += bytes([crc & 0xFF, (crc >> 8) & 0xFF])
    return bytes(f)


# ============================================================ 测试
def main():
    ok = True

    def check(name, cond, extra=""):
        nonlocal ok
        ok &= bool(cond)
        print("  %-46s %s%s" % (name, "OK" if cond else "!! 失败", extra))

    # ---- A: 基本握手 ----
    fw = Firmware()
    for t, ch in enumerate(b"km.version\r\n"):
        fw.now = t
        fw.feed_pc(ch)
    tx = fw.take_tx()
    h = Host()
    good, lines = h.probe(tx)
    check("A km.version -> 一行含 MAKCU-PASSTHROUGH", good, "  reply=%r" % (lines[0],))

    # ---- B: 带追踪 ID 的握手 ----
    fw = Firmware()
    for t, ch in enumerate(b"km.version#7\r\n"):
        fw.now = t
        fw.feed_pc(ch)
    tx = fw.take_tx()
    h = Host()
    good, lines = h.probe(tx)
    check("B km.version#7 -> 追踪应答仍含 tag", good, "  reply=%r" % (lines[0],))

    # ---- C: 前面混入固件日志行, 握手仍要成功 ----
    fw = Firmware()
    logline = b"[KBD-DEV] role=1 cmds=0 real=0 ghost=0/0 mask=0 crc=0\r\n"
    for t, ch in enumerate(logline + b"km.version\r\n"):
        fw.now = t
        fw.feed_pc(ch)
    tx = fw.take_tx()
    h = Host()
    good, lines = h.probe(tx)
    check("C 日志行先到, 握手仍成功", good, "  最后一行=%r" % (lines[0],))

    # ---- D: 应答里不能出现违规字节 ----
    fw = Firmware()
    for t, ch in enumerate(b"km.version\r\nkm.buttons(1)\r\n"):
        fw.now = t
        fw.feed_pc(ch)
    tx = fw.take_tx()
    bad_ctrl = [b for b in tx if b < 0x20 and b not in (0x0A, 0x0D)]
    bad_high = [b for b in tx if b >= 0x7F]
    check("D 应答无 <0x20(除CRLF) 且无 >=0x7F 字节",
          not bad_ctrl and not bad_high,
          "  ctrl=%s high=%s" % (bad_ctrl, bad_high))

    # ---- E: 二进制帧紧接握手, ASCII 通道不能被打断 ----
    fw = Firmware()
    stream = pack_frame(0x01, bytes([0x41, 0x00, 0x42, 0x00])) + b"km.version\r\n"
    for t, ch in enumerate(stream):
        fw.now = t
        fw.feed_pc(ch)
    tx = fw.take_tx()
    h = Host()
    good, _ = h.probe(tx)
    check("E 二进制帧后紧跟握手 -> 仍成功", good)

    # ---- F: 恶意载荷: 帧体内藏 "km.version\\n" 不能被当成命令 ----
    #
    #   这是仲裁设计的关键负向测试。payload 里塞进可打印文本 + 换行。
    #   若没有"帧在途时抑制 ASCII"的仲裁, 固件就会凭空回一条版本应答 ——
    #   真实场景里这会被上位机误判成一次应答。
    fw = Firmware()
    evil = b"km.version\n"
    stream = pack_frame(0x03, evil)          # 批量位移命令, payload 含文本
    for t, ch in enumerate(stream):
        fw.now = t
        fw.feed_pc(ch)
    tx = fw.take_tx()
    check("F 帧体内的 km.version 不触发应答", tx == b"", "  tx=%r" % (tx,))

    # ---- G: 屏蔽: km.mask(N) 定时生效并自动解除 ----
    fw = Firmware()
    for t, ch in enumerate(b"km.mask(500)\r\n"):
        fw.now = t
        fw.feed_pc(ch)
    active_at_100 = (fw.mask_until != 0)
    fw.tick(100)
    still = (fw.mask_until != 0)
    fw.tick(600)
    cleared = (fw.mask_until == 0)
    check("G km.mask(500) 生效/未提前解除/自动解除",
          active_at_100 and still and cleared,
          "  t=100 仍屏蔽=%s, t=600 已解除=%s" % (still, cleared))

    # ---- H: km.maskoff 立即解除 ----
    fw = Firmware()
    for t, ch in enumerate(b"km.mask(2000)\r\n"):
        fw.now = t
        fw.feed_pc(ch)
    on = fw.mask_until != 0
    for t, ch in enumerate(b"km.maskoff\r\n"):
        fw.now = 10 + t
        fw.feed_pc(ch)
    check("H km.maskoff 立即解除", on and fw.mask_until == 0)

    # ---- I: km.mask(N) 上限钳制 ----
    fw = Firmware()
    fw.now = 1000
    for t, ch in enumerate(b"km.mask(99999)\r\n"):
        fw.feed_pc(ch)
    clamped = (fw.mask_until == 1000 + MASK_MAX_MS)
    check("I km.mask(99999) 钳到 %dms" % MASK_MAX_MS, clamped,
          "  until=%d (期望 %d)" % (fw.mask_until, 1000 + MASK_MAX_MS))

    # ---- J: km.mask(0) 视为解除 ----
    fw = Firmware()
    fw.mask_until = 12345
    for ch in b"km.mask(0)\r\n":
        fw.feed_pc(ch)
    check("J km.mask(0) 视为解除", fw.mask_until == 0)

    # ---- K: 残帧不会永久抑制 ASCII 通道 ----
    #
    #   真正能造成"卡住"的形态: 收到 "A5 5C <plen>" 之后字节流就断了。
    #   此时 s_pcLen 停在 5, 而 need = 5+plen+2 > 5, 解析器在等后续字节 ——
    #   若没有空闲复位, ASCII 累积会被【永久】抑制, 表现为"上位机发什么都没
    #   反应"且不报错。
    fw = Firmware()
    fw.now = 0
    for b in (0xA5, 0x5C, 0x05, 0x00, 0x01):
        fw.feed_pc(b)
    stuck = (fw.pc_len == 5)          # 确实卡在半个帧上
    fw.now = 200                      # 超过 100ms 无新字节
    for t, ch in enumerate(b"km.version\r\n"):
        fw.now = 200 + t
        fw.feed_pc(ch)
    tx = fw.take_tx()
    h = Host()
    good, lines = h.probe(tx)
    check("K 半个帧卡住后 ASCII 通道自行恢复", stuck and good,
          "  卡住=%s 应答=%r" % (stuck, lines[0] if lines else ""))

    print()
    print("结果:", "全部通过" if ok else "存在不一致")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
