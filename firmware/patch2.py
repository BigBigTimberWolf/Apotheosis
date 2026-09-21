path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\include\EspUsbHost.h"
src = open(path, encoding="utf-8", errors="replace").read()

anchor = "    virtual void onMouseMove(hid_mouse_report_t report);"
assert anchor in src, "anchor not found"

addition = anchor + """

    // 键盘透传(复合 HID 设备支持)。
    //
    // 老版完全没有键盘通路: 键盘接口的报文进 _onReceive 后因为 protocol
    // 判定不匹配而被静默丢弃。现在按接口类别分派, 键盘接口的报文会走到
    // 这里, 再把键码快照透传给设备侧。
    //
    // 参数即标准 HID 键盘报告的语义:
    //   mod     = 修饰键位图(bit0 L_Ctrl ... bit7 R_GUI)
    //   keys[6] = 当前按下的 6 个键码(0 表示空位)
    virtual void onKeyboardSnapshot(uint8_t mod, const uint8_t *keys);"""

src = src.replace(anchor, addition, 1)
open(path, "w", encoding="utf-8", newline="").write(src)
print("header patched")
