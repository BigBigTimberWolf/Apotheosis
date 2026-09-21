# 1) 头文件: 声明 serial1SendKeyboardBinary
h = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\include\EspUsbHost.h"
s = open(h, encoding="utf-8", errors="replace").read()

if "serial1SendKeyboardBinary" not in s:
    # 插入到 onKeyboardSnapshot 声明之后
    anchor = "    virtual void onKeyboardSnapshot(uint8_t mod, const uint8_t *keys);"
    assert anchor in s, "onKeyboardSnapshot anchor missing"
    s = s.replace(anchor, anchor + """

    // 经 Serial1 发送二进制键盘透传帧(0x23 KB_REPORT)。
    void serial1SendKeyboardBinary(uint8_t modifiers, const uint8_t *keys, uint8_t keyCount);""", 1)
    open(h, "w", encoding="utf-8", newline="").write(s)
    print("header: serial1SendKeyboardBinary declared")
else:
    print("header: already present")

# 2) cpp: 实现 onKeyboardSnapshot
c = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\src\esp_usb_host.cpp"
s2 = open(c, encoding="utf-8", errors="replace").read()

if "EspUsbHost::onKeyboardSnapshot" not in s2:
    anchor2 = """void EspUsbHost::_onReceive(usb_transfer_t *transfer)"""
    assert anchor2 in s2, "_onReceive anchor missing"

    impl = r'''// 键盘透传: 把真实键盘的键码快照发给设备侧。
//
// 为什么需要去重: 键盘报文是"全量快照", 真实键盘会在状态没变时也周期性
// 重发同样的内容。不去重的话, 打字/按住键时串口上行会被无意义的重复快照
// 占满, 设备侧还要白跑一遍 HID 发送。所以内容不变就整帧丢弃。
//
// 去重缓存用 static: 本函数只在 _onReceive 里被调用(单线程), 且不跨设备
// 复用(HIDReportDesc 在设备插入时会重置, 这里也一并作废)。
void EspUsbHost::onKeyboardSnapshot(uint8_t mod, const uint8_t *keys)
{
    if (!deviceMouseReady) {
        return;     // 设备侧尚未就绪, 先不透传(与鼠标通路保持一致的时机)
    }

    // ---- 去重 ----
    static uint8_t lastMod = 0xFF;      // 初值取不可能的值, 保证第一帧一定发出
    static uint8_t lastKeys[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    bool changed = (mod != lastMod);
    for (int i = 0; i < 6 && !changed; ++i) {
        if (keys[i] != lastKeys[i]) changed = true;
    }
    if (!changed) {
        return;                         // 内容没变: 丢掉这一帧
    }

    lastMod = mod;
    for (int i = 0; i < 6; ++i) {
        lastKeys[i] = keys[i];
    }

    // ---- 透传 ----
    // 走二进制 0x23 KB_REPORT: 打字时每秒可能十几帧, 二进制仅 14 字节且
    // 设备侧零解析(ASCII 形式要 30+ 字节并 sscanf 解析)。
    serial1SendKeyboardBinary(mod, lastKeys, 6);
}


'''
    s2 = s2.replace(anchor2, impl + anchor2, 1)
    open(c, "w", encoding="utf-8", newline="").write(s2)
    print("cpp: onKeyboardSnapshot implemented")
else:
    print("cpp: already present")
