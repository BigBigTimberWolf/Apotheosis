# -*- coding: utf-8 -*-
"""
把 MAKCUNEW 的键盘改成"独立版"那套纯搬字节架构

【目标架构 (用户要求, 与独立版一致)】
    右板: 端点收到原始字节 ──> 直接打包 ──> Serial1     (只搬字节)
    左板: 收到 ──> 直接发 USB                          (只搬字节)
  外加: 上位机注入 + km.mask 屏蔽 (用户保证注入不与真实输入冲突)

【本次改动】
  1) 右板: 新增 serial1SendKeyboardRaw(), payload = 端点原始字节(最多 8)
  2) 右板: 键盘分支改为直接调它, 不再构造 KeyboardReport / 不再 onKeyboard
     同时【带上接口号】, 让左板能区分来源
  3) 左板: 新增 kbdApplyRealRaw(), 收到后直接发 USB, 不做任何解析/合并
  4) 左板: 0x23 帧解析改为透传原始字节

【帧格式 (0x23, 与旧版兼容性说明)】
  payload = [iface][raw0..rawN-1]
    旧版: [mod][k0..k5]           (无 iface 字段, 长度 1..7)
    新版: [iface][raw0..raw7]     (长度 1..9)
  左板按长度判断: len >= 2 且 len-1 >= 6 -> 当作新版原始字节处理
                 否则退回旧版解析(保证与旧右板固件兼容)
"""
import sys
sys.stdout.reconfigure(encoding='utf-8')
root = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source'

# ===========================================================================
# 1) 右板 esp_tasks.cpp: 新增 serial1SendKeyboardRaw
# ===========================================================================
p1 = root + r'\fw_host\src\esp_tasks.cpp'
s1 = open(p1, encoding='utf-8', errors='replace').read()

ANCHOR = "void EspUsbHost::serial1SendKeyboardBinary(uint8_t modifiers, const uint8_t *keys, uint8_t keyCount)"
assert ANCHOR in s1, "锚点未找到"

RAW_FN = '''// ============================================================================
// 键盘原样转发: payload = [iface][端点原始字节...]
//
// 【为什么带上 iface】
//   复合键盘接收器有多个 HID 接口(键盘 + 多媒体等)。实测多媒体接口的报文
//   也会被当成键盘报文, 导致按键忽有忽无。带上接口号后左板(或诊断)能区分来源。
//
// 【为什么不做任何解析】
//   用户要求与"独立版"一致: 端点收到什么就转什么。省掉描述符查表/边界推算,
//   回调更快返回 -> 重提交更早 -> 主机下一次轮询更及时(延迟的直接来源)。
// ============================================================================
void EspUsbHost::serial1SendKeyboardRaw(uint8_t iface, const uint8_t *report, uint8_t len)
{
    if (len > 8) len = 8;
    const uint8_t plen = (uint8_t)(1 + len);

    s_tx_frame[0] = (char)0xA5;
    s_tx_frame[1] = (char)0x5C;
    s_tx_frame[2] = (char)plen;
    s_tx_frame[3] = (char)(s_tx_seq++);
    s_tx_frame[4] = (char)0x23;                      // CMD_KB_REPORT
    s_tx_frame[5] = (char)iface;
    for (uint8_t i = 0; i < len; ++i) {
        s_tx_frame[6 + i] = (char)report[i];
    }

    const int crcStart = 2;
    const int crcLen   = 3 + plen;
    uint16_t crc = 0xFFFF;
    for (int i = crcStart; i < crcStart + crcLen; ++i) {
        crc ^= (uint8_t)s_tx_frame[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    s_tx_frame[crcStart + crcLen]     = (char)(crc & 0xFF);
    s_tx_frame[crcStart + crcLen + 1] = (char)((crc >> 8) & 0xFF);

    Serial1.write((const uint8_t *)s_tx_frame, (size_t)(plen + 7));
}

''' + ANCHOR

s1 = s1.replace(ANCHOR, RAW_FN, 1)
open(p1, 'w', encoding='utf-8', newline='').write(s1)
print("[1] 右板: 已加 serial1SendKeyboardRaw")

# 头文件声明
ph = root + r'\fw_host\include\EspUsbHost.h'
sh = open(ph, encoding='utf-8', errors='replace').read()
if 'serial1SendKeyboardRaw' not in sh:
    OLDH = "    void serial1SendKeyboardBinary(uint8_t modifiers, const uint8_t *keys, uint8_t keyCount);"
    NEWH = OLDH + "\n    void serial1SendKeyboardRaw(uint8_t iface, const uint8_t *report, uint8_t len);"
    assert OLDH in sh
    sh = sh.replace(OLDH, NEWH, 1)
    open(ph, 'w', encoding='utf-8', newline='').write(sh)
    print("[1b] 头文件已声明")

# ===========================================================================
# 2) 右板 esp_usb_host.cpp: 键盘分支改为原样转发
# ===========================================================================
p2 = root + r'\fw_host\src\esp_usb_host.cpp'
s2 = open(p2, encoding='utf-8', errors='replace').read()

OLD2 = """    if (epIsKeyboardRaw)
    {
        const uint8_t *db = transfer->data_buffer;

        EspUsbHost::KeyboardReport kr = {};
        kr.modifiers = db[0];

        // 键码个数: 报文一般 8 字节(6 个键位), 短报文按实际长度裁剪
        // 固定 6 个键码: 8 字节 boot keyboard 布局固定, 不再按长度推算
        const uint8_t n = 6;
        for (uint8_t i = 0; i < n; i++) {
            kr.keys[i] = db[2 + i];
        }
        kr.keyCount = n;

        usbHost->onKeyboard(kr);"""

NEW2 = """    if (epIsKeyboardRaw)
    {
        // ★ 原样转发: 端点收到什么就转什么, 不做解析。
        //   带上接口号, 让诊断能区分是键盘接口还是多媒体接口。
        usbHost->serial1SendKeyboardRaw(ifaceNum, transfer->data_buffer,
                                        (uint8_t)reportLen);"""

if OLD2 in s2:
    s2 = s2.replace(OLD2, NEW2, 1); print("[2] 右板: 键盘分支已改为原样转发")
else:
    print("[2] 键盘分支未找到, 尝试宽松匹配")
    import re
    m = re.search(r'    if \(epIsKeyboardRaw\)\s*\{.*?\n    \}', s2, re.S)
    assert m, "仍找不到"
    s2 = s2[:m.start()] + NEW2 + s2[m.end():]
    print("[2] (宽松匹配) 已替换")

open(p2, 'w', encoding='utf-8', newline='').write(s2)
print()
print("完成右板改动")
