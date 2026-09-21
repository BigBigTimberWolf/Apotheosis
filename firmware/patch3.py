path = r"C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\_fix1_build\src\esp_tasks.cpp"
src = open(path, encoding="utf-8", errors="replace").read()

# 把发送器插在 include 之后
marker = '#include "freertos/task.h"'
assert marker in src, "include marker not found"

addition = marker + r'''

// ============================================================================
// Serial1 二进制帧发送器(复合 HID 设备透传用)
//
// 帧格式: A5 5C | LEN | SEQ | CMD | PAYLOAD | CRC16(2B, MODBUS, 低字节先)
//   LEN = CMD + PAYLOAD 的总字节数(不含 A5 5C / LEN / SEQ / CRC)
//   CRC 覆盖范围 = LEN 起、共 (3 + LEN) 字节(LEN, SEQ, CMD, PAYLOAD)
//
// 与设备侧 proto_parser 的解析严格对应(见 fw_device/include/proto_parser.h)。
// 键盘帧 CMD = 0x23 KB_REPORT, PAYLOAD = [modifiers][key0..key5]
// ============================================================================
static uint8_t s_tx_seq = 0;
static char    s_tx_frame[24];      // 最大帧 = 2+1+1+1+1+6+2 = 14, 24 足够

void EspUsbHost::serial1SendKeyboardBinary(uint8_t modifiers, const uint8_t *keys, uint8_t keyCount)
{
    if (keyCount > 6) keyCount = 6;
    const uint8_t len = (uint8_t)(1 + keyCount);

    s_tx_frame[0] = (char)0xA5;
    s_tx_frame[1] = (char)0x5C;
    s_tx_frame[2] = (char)len;
    s_tx_frame[3] = (char)(s_tx_seq++);
    s_tx_frame[4] = (char)0x23;                      // CMD_KB_REPORT
    s_tx_frame[5] = (char)modifiers;
    for (uint8_t i = 0; i < keyCount; ++i) {
        s_tx_frame[6 + i] = (char)keys[i];
    }

    const int crcStart = 2;
    const int crcLen   = 3 + len;
    uint16_t crc = 0xFFFF;
    for (int i = crcStart; i < crcStart + crcLen; ++i) {
        crc ^= (uint8_t)s_tx_frame[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    s_tx_frame[crcStart + crcLen]     = (char)(crc & 0xFF);
    s_tx_frame[crcStart + crcLen + 1] = (char)((crc >> 8) & 0xFF);

    Serial1.write((const uint8_t *)s_tx_frame, (size_t)(len + 7));
}
'''

src = src.replace(marker, addition, 1)
open(path, "w", encoding="utf-8", newline="").write(src)
print("esp_tasks.cpp patched with serial1SendKeyboardBinary")
