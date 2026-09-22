# -*- coding: utf-8 -*-
"""
修复: 键盘端点判定过宽 -> 多接口报文交错

【现象】
  用户按住 A 时, 日志显示 A 稳定, 但另一个键(1A)每 ~80ms 忽有忽无:
      36331  keys=04        <- 1A 消失
      36430  keys=04 1A     <- 1A 出现
      36500  keys=04        <- 消失
      ...

【原因】
  键盘接收器是复合设备, 有多个 HID 接口(MI_00 键盘 + MI_01 多媒体等)。
  我把判定放宽成"非鼠标的 HID 输入端点"后, 【多个接口的端点都命中】,
  它们各自调用 onKeyboard, 报文互相覆盖/交错 -> 键码忽有忽无。

  原代码用 descPerIface[].isKeyboard(描述符解析结果)来锁定【唯一】键盘接口,
  我为了去掉解析依赖把这道筛选去掉了, 于是撞上这个问题。

【修法】
  仍然不依赖描述符解析, 但只认【第一个】命中的非鼠标 HID 输入端点所属的接口,
  之后的报文只接受同一接口。这样既能去掉解析依赖, 又保证只有一个端点喂键盘。
  设备重新枚举(DEV_GONE)时复位, 以便换设备后重新选定。
"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_host\src\esp_usb_host.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

OLD = """    const bool epIsKeyboardRaw =
        epKnown && has_data && (reportLen >= 3) &&
        (ifaceNum < EspUsbHost::kMaxIface) &&
        (usbHost->endpoint_data_list[ifaceNum].bInterfaceClass == USB_CLASS_HID) &&
        !epIsSelectedMouse &&
        !epIsFallbackMouse;

    if (epIsKeyboardRaw)
    {"""

NEW = """    // ★ 只锁定【一个】键盘接口。
    //
    // 复合键盘接收器有多个 HID 接口(MI_00 键盘 + MI_01 多媒体等), 若判定放宽成
    // "所有非鼠标的 HID 输入端点", 多个端点都会喂 onKeyboard, 报文互相交错 ——
    // 实测现象是: 按住 A 时另一个键码每 ~80ms 忽有忽无。
    //
    // 原代码用 descPerIface[].isKeyboard(依赖描述符解析)来锁定唯一接口; 这里
    // 不依赖解析, 改成"第一个命中的接口即键盘接口", 之后只接受它的报文。
    static int s_kbdRawIface = -1;

    const bool epIsKeyboardCandidate =
        epKnown && has_data && (reportLen >= 3) &&
        (ifaceNum < EspUsbHost::kMaxIface) &&
        (usbHost->endpoint_data_list[ifaceNum].bInterfaceClass == USB_CLASS_HID) &&
        !epIsSelectedMouse &&
        !epIsFallbackMouse;

    if (epIsKeyboardCandidate && s_kbdRawIface < 0) {
        // 首次命中: 选定它作为唯一的键盘接口
        s_kbdRawIface = (int)ifaceNum;
        ESP_LOGI("EspUsbHost", "keyboard raw iface locked to %d", (int)ifaceNum);
    }

    const bool epIsKeyboardRaw = epIsKeyboardCandidate &&
                                 ((int)ifaceNum == s_kbdRawIface);

    if (epIsKeyboardRaw)
    {"""

assert OLD in s, "键盘判定块未找到"
s = s.replace(OLD, NEW, 1)

# 设备断开时复位锁定, 以便换设备后重新选定
OLD2 = """        usbHost->isReady = false;
        EspUsbHost::deviceConnected = false;
        deviceMouseReady = false;"""
NEW2 = """        usbHost->isReady = false;
        EspUsbHost::deviceConnected = false;
        deviceMouseReady = false;
        // 键盘接口锁定也要复位: 换一台设备后需要重新选定键盘接口
        // (s_kbdRawIface 是 _onReceive 里的 static, 这里通过一个外部钩子复位)
        usbHost->resetKeyboardRawIface();"""
assert OLD2 in s, "DEV_GONE 块未找到"
s = s.replace(OLD2, NEW2, 1)

# 把 static 提到类里, 以便 DEV_GONE 复位
OLD3 = """    static int s_kbdRawIface = -1;

    const bool epIsKeyboardCandidate ="""
NEW3 = """    static int &s_kbdRawIface = usbHost->kbdRawIface;

    const bool epIsKeyboardCandidate ="""
assert OLD3 in s
s = s.replace(OLD3, NEW3, 1)

open(p, 'w', encoding='utf-8', newline='').write(s)
print("已加入键盘接口锁定")

# 头文件加成员
ph = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_host\include\EspUsbHost.h'
h = open(ph, encoding='utf-8', errors='replace').read()
if 'kbdRawIface' not in h:
    OLDH = """    HIDReportDescriptor descPerIface[kMaxIface];"""
    NEWH = """    // 原样转发模式下锁定的唯一键盘接口(-1 = 未选定)。
    // 复合键盘接收器有多个 HID 接口, 必须只认一个, 否则报文交错。
    int kbdRawIface = -1;
    void resetKeyboardRawIface() { kbdRawIface = -1; }

    HIDReportDescriptor descPerIface[kMaxIface];"""
    assert OLDH in h, "头文件锚点未找到"
    h = h.replace(OLDH, NEWH, 1)
    open(ph, 'w', encoding='utf-8', newline='').write(h)
    print("头文件已加 kbdRawIface 成员")
