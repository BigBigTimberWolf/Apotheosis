# -*- coding: utf-8 -*-
"""
右板键盘改为「原样转发」(与单独开发的 KBD_PASSTHROUGH 一致)

【改动前】
  epIsSelectedKeyboard 依赖一长串异步解析结果:
      descPerIfaceValid[] && descPerIface[].valid && [].isKeyboard
  且每帧按描述符里的偏移(k.modifierByte / k.keyArrayStartByte)解码。
  => 既脆弱(解析失败就完全没有键盘数据), 又慢(每帧都要走这一套)。

【改动后】
  判定只看两件事: 该端点属于 HID 接口, 且【不是鼠标接口】。
  取字节用【固定偏移】: byte0 = 修饰键, byte2..7 = 6 个键码
  (标准 boot keyboard 布局, 与真键盘一致)。
  不做任何解码/查表/描述符依赖。

【与左板完全兼容】
  0x23 帧的 payload 格式不变([mod][k0..k5]), 所以左板一行都不用改。
  左板已有的 kbdApplyRealReport 本来就是直发。
"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_host\src\esp_usb_host.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

OLD = """    const bool epIsSelectedKeyboard =
        epKnown &&
        (usbHost->keyboardIface >= 0) &&
        (ifaceNum == (uint8_t)usbHost->keyboardIface) &&
        (ifaceNum < EspUsbHost::kMaxIface) &&
        (usbHost->endpoint_data_list[ifaceNum].bInterfaceClass == USB_CLASS_HID) &&
        usbHost->descPerIfaceValid[ifaceNum] &&
        usbHost->descPerIface[ifaceNum].valid &&
        usbHost->descPerIface[ifaceNum].isKeyboard;

    if (has_data && epIsSelectedKeyboard)
    {
        const EspUsbHost::HIDReportDescriptor &k = usbHost->descPerIface[ifaceNum];

        // 键码数组必须完整落在本帧内 —— 同样宁可丢帧也不越界读。
        const bool kbdLayoutOk =
            (k.keyArrayCount > 0) &&
            ((int)k.keyArrayStartByte + (int)k.keyArrayCount <= reportLen) &&
            (!k.hasModifierByte || (int)k.modifierByte < reportLen);

        // Report ID 过滤(键盘接口常带自己的 ID)
        const bool kbdIdOk =
            !k.hasReportId || (reportLen > 0 && transfer->data_buffer[0] == k.reportId);

        if (kbdLayoutOk && kbdIdOk)
        {
            EspUsbHost::KeyboardReport kr = {};
            const uint8_t *db = transfer->data_buffer;

            if (k.hasModifierByte) {
                kr.modifiers = db[k.modifierByte];
            }

            uint8_t n = k.keyArrayCount;
            if (n > sizeof(kr.keys)) n = (uint8_t)sizeof(kr.keys);
            for (uint8_t i = 0; i < n; i++) {
                kr.keys[i] = db[k.keyArrayStartByte + i];
            }
            kr.keyCount = n;

            usbHost->onKeyboard(kr);
        }"""

NEW = """    // ===== 键盘: 原样转发 (不做任何解析) =====
    //
    // 【判定只依赖两件事】
    //   1) 该端点已被描述符登记, 且属于 HID 接口
    //   2) 它【不是鼠标端点】—— 本板 USB-C 只插一个设备, 所以"非鼠标的 HID
    //      输入端点"就是键盘
    //
    // 【为什么去掉描述符解析依赖】
    //   原判据要求 descPerIfaceValid[] && valid && isKeyboard。这三者都要等
    //   "onConfig -> 异步 GET_DESCRIPTOR(0x22) -> 回调 -> 解析" 这条长链走完
    //   才会为真; 任何一环出问题就【完全没有键盘数据】, 而且不报错。
    //   实测这正是鼠标曾经失效的同款故障(见鼠标段的说明)。
    //
    // 【取字节用固定偏移, 不查表】
    //   标准 boot keyboard 报文: byte0=修饰键, byte1=保留, byte2..7=6 个键码。
    //   这与真键盘直插时主机看到的一致, 也是用户要的"原样透传"。
    //   每帧省掉一整套描述符查表 + 边界推算 -> 回调更快返回 -> 重提交更早
    //   -> 主机下一次轮询更及时(延迟的直接来源)。
    const bool epIsKeyboardRaw =
        epKnown && has_data && (reportLen >= 3) &&
        (ifaceNum < EspUsbHost::kMaxIface) &&
        (usbHost->endpoint_data_list[ifaceNum].bInterfaceClass == USB_CLASS_HID) &&
        !epIsSelectedMouse &&
        !epIsFallbackMouse;

    if (epIsKeyboardRaw)
    {
        const uint8_t *db = transfer->data_buffer;

        EspUsbHost::KeyboardReport kr = {};
        kr.modifiers = db[0];

        // 键码个数: 报文一般 8 字节(6 个键位), 短报文按实际长度裁剪
        uint8_t n = (reportLen >= 8) ? 6 : (uint8_t)(reportLen - 2);
        if (n > 6) n = 6;
        for (uint8_t i = 0; i < n; i++) {
            kr.keys[i] = db[2 + i];
        }
        kr.keyCount = n;

        usbHost->onKeyboard(kr);"""

assert OLD in s, "键盘解码块未找到"
s = s.replace(OLD, NEW, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print("右板键盘已改为原样转发 (固定偏移, 不依赖描述符解析)")
print()
print("验证:")
print("  epIsKeyboardRaw 存在 :", 'epIsKeyboardRaw' in s)
print("  已去掉 isKeyboard 依赖:", 'descPerIface[ifaceNum].isKeyboard' not in s)
