# -*- coding: utf-8 -*-
"""
静态对拍: 把 fw_host 的接口判定从 C++ 逐条搬到 Python, 对比"改之前"和"改之后"
在四种真实场景下把报文分派到哪条路。

不碰硬件。目的是证明两件事:
  1) 键盘单板的键盘报文确实能到键盘路(改之前到不了 -> "按键完全没反应")
  2) 鼠标单板的鼠标报文仍然到鼠标路(不能因为修键盘把鼠标搞回归)

接口参数取自实测:
  键盘接口: class=HID, protocol=NONE(0x00), 报文 8 字节, 描述符 isKeyboard
  鼠标接口: class=HID, protocol=MOUSE(0x02), 报文 5 字节 (bSz=5, x@1/16b, y@3/16b, wh@4)
"""

HID = 3
PROTO_NONE = 0
PROTO_KBD = 1
PROTO_MOUSE = 2


class Iface:
    def __init__(self, num, proto, desc_parsed, is_kbd, is_mouse):
        self.num = num
        self.cls = HID
        self.proto = proto
        # desc_parsed 对应 descPerIfaceValid[] && descPerIface[].valid
        self.desc_parsed = desc_parsed
        self.is_kbd = is_kbd
        self.is_mouse = is_mouse


def decide_old(iface, report_len, keyboard_iface, mouse_iface):
    """改之前的判定 (epIsKeyboardCandidate 里带了 !epIsFallbackMouse)"""
    ep_known = True
    has_data = report_len > 0
    layout_usable = iface.desc_parsed
    iface_is_hid = iface.cls == HID
    proto_looks_mouse = iface.proto in (PROTO_MOUSE, PROTO_NONE)

    ep_is_selected_mouse = (ep_known and mouse_iface is not None and
                            iface.num == mouse_iface and iface_is_hid and layout_usable)
    ep_is_fallback_mouse = (has_data and not layout_usable and iface_is_hid and
                            proto_looks_mouse and report_len >= 3)

    ep_is_kbd_candidate = (ep_known and has_data and report_len == 8 and
                           iface_is_hid and
                           not ep_is_selected_mouse and not ep_is_fallback_mouse)

    if has_data and (ep_is_selected_mouse or ep_is_fallback_mouse):
        return "鼠标"
    if ep_is_kbd_candidate:
        return "键盘"
    return "丢弃"


def decide_new(iface, report_len, keyboard_iface, mouse_iface, with_report_id=False):
    """改之后的判定 (键盘优先 + 鼠标兜底排除键盘)"""
    ep_known = True
    has_data = report_len > 0
    layout_usable = iface.desc_parsed
    iface_is_hid = iface.cls == HID
    proto_looks_mouse = iface.proto in (PROTO_MOUSE, PROTO_NONE)

    # ifaceDescKbd
    iface_desc_kbd = iface.desc_parsed and iface.is_kbd

    # frameLooksBootKbd: 8 字节, 或 9 字节且描述符声明了 Report ID
    frame_looks_boot_kbd = False
    if iface_is_hid and has_data:
        if report_len == 8:
            frame_looks_boot_kbd = True
        elif report_len == 9 and with_report_id and iface.desc_parsed:
            frame_looks_boot_kbd = True

    iface_is_kbd_iface = iface_is_hid and (
        (iface.num == keyboard_iface) if keyboard_iface is not None
        else (iface_desc_kbd or
              (frame_looks_boot_kbd and iface.proto != PROTO_MOUSE and
               (mouse_iface is None or iface.num != mouse_iface)))
    )

    ep_is_selected_mouse = (ep_known and mouse_iface is not None and
                            iface.num == mouse_iface and iface_is_hid and layout_usable)
    ep_is_fallback_mouse = (has_data and not layout_usable and iface_is_hid and
                            proto_looks_mouse and report_len >= 3 and
                            not iface_is_kbd_iface)          # ★ 新增的排除

    ep_is_kbd_raw = (iface_is_kbd_iface and has_data and
                     not ep_is_selected_mouse and not ep_is_fallback_mouse)

    if ep_is_kbd_raw:
        return "键盘"
    if has_data and (ep_is_selected_mouse or ep_is_fallback_mouse):
        return "鼠标"
    return "丢弃"


def run(title, iface, report_len, kbd_iface, mouse_iface, expect_new,
        with_report_id=False):
    old = decide_old(iface, report_len, kbd_iface, mouse_iface)
    new = decide_new(iface, report_len, kbd_iface, mouse_iface, with_report_id)
    ok = (new == expect_new)
    print("  %-34s len=%-2d  改前=%-3s -> 改后=%-3s  %s"
          % (title, report_len, old, new, "OK" if ok else "!! 期望 " + expect_new))
    return ok


def main():
    ok = True
    print("【场景 1】键盘单板: 键盘描述符已解析成功")
    kbd = Iface(0, PROTO_NONE, True, True, False)
    ok &= run("键盘接口 敲键", kbd, 8, kbd_iface=0, mouse_iface=None, expect_new="键盘")
    ok &= run("键盘接口 松开", kbd, 8, kbd_iface=0, mouse_iface=None, expect_new="键盘")

    print()
    print("【场景 2】键盘单板: 键盘描述符【没解析出来】(故障现场)")
    kbd_nod = Iface(0, PROTO_NONE, False, False, False)
    ok &= run("键盘接口 敲键", kbd_nod, 8, kbd_iface=None, mouse_iface=None, expect_new="键盘")

    print()
    print("【场景 3】鼠标单板: 鼠标描述符已解析成功")
    mou = Iface(0, PROTO_MOUSE, True, False, True)
    ok &= run("鼠标接口 移动", mou, 5, kbd_iface=None, mouse_iface=0, expect_new="鼠标")

    print()
    print("【场景 4】鼠标单板: 鼠标描述符没解析出来(靠兜底)")
    mou_nod = Iface(0, PROTO_NONE, False, False, False)
    ok &= run("鼠标接口 移动", mou_nod, 5, kbd_iface=None, mouse_iface=None, expect_new="鼠标")

    print()
    print("【场景 5】鼠标单板: 复合接收器, 键盘接口也在(只插鼠标)")
    mou2 = Iface(0, PROTO_MOUSE, True, False, True)
    ok &= run("鼠标接口 移动", mou2, 5, kbd_iface=1, mouse_iface=0, expect_new="鼠标")

    print()
    print("【场景 6】键盘单板: 键盘接口带 1 字节 Report ID 前缀(9 字节帧)")
    kbd_rid = Iface(0, PROTO_NONE, True, True, False)
    ok &= run("键盘接口 敲键(带ID)", kbd_rid, 9, kbd_iface=0, mouse_iface=None,
              expect_new="键盘", with_report_id=True)

    print()
    print("结果:", "全部通过" if ok else "存在不一致")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
