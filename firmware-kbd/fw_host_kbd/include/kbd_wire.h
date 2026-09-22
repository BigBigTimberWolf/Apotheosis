// ============================================================================
// kbd_wire.h —— 板间键盘透传协议 (原样字节透传)
//
// 设计目标: 把真键盘的 HID Report 【原封不动】送到被控机。
//
//   真键盘 ──USB──> 右板(USB Host) ──Serial1──> 左板(USB Device) ──USB──> 被控机
//                    只读字节                     原样重发
//
// 为什么原样透传:
//   1) 不做按键解析 -> 不可能解析错
//   2) 保留多媒体键、组合键、厂商自定义键
//   3) 被控机收到的就是真键盘的原始报文, 与真设备无异
//
// 帧格式 (小端):
//   偏移  长度  含义
//   0     1     魔数 0xA5
//   1     1     魔数 0x5C
//   2     1     payload 长度 (不含帧头/CRC)
//   3     1     序号 (回绕, 用于丢帧检测)
//   4     1     类型: 0x23 = 键盘报告
//   5..N  可变  键盘原始报文 (8 字节: modifiers + reserved + key0..key5)
//   N+1   2     CRC16/MODBUS (对偏移 2 起的 len+2 字节)
//
// 与现有右板固件的差别:
//   现有固件把 Report 解析成 modifiers + 6 keys 再重组, 长度固定 8。
//   这里直接透传原始字节, payload 长度可变(兼容带 Report ID 或厂商扩展的键盘)。
// ============================================================================

#ifndef KBD_WIRE_H
#define KBD_WIRE_H

#include <stdint.h>
#include <string.h>

// ---- 帧常量 ----
#define KBD_MAGIC0        0xA5
#define KBD_MAGIC1        0x5C
#define KBD_TYPE_REPORT   0x23      // 键盘原始报告 (右板 -> 左板)
#define KBD_TYPE_LED      0x24      // 键盘 LED 状态 (左板 -> 右板)
#define KBD_TYPE_ID       0x25      // 真键盘设备身份 (右板 -> 左板)

// 键盘原始报文的最大长度。
// 标准 boot keyboard 是 8 字节; 带 Report ID 的是 9; 留足余量给厂商扩展。
#define KBD_MAX_REPORT    16

// 身份帧 payload 长度: idVendor(2) + idProduct(2) + bcdDevice(2) + role(1)
#define KBD_ID_LEN        7
// LED 帧 payload 长度: 1 字节位图 (已废弃, 保留常量以兼容旧固件解析)
#define KBD_LED_LEN       1

// 设备角色 (身份帧最后一个字节)
#define KBD_ROLE_UNKNOWN  0
#define KBD_ROLE_KEYBOARD 1
#define KBD_ROLE_MOUSE    2

// 最大帧长 = 头5 + payload + CRC2
#define KBD_MAX_FRAME     (5 + KBD_MAX_REPORT + 2)

// ---- CRC16/MODBUS ----
static inline uint16_t kbdCrc16(const uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

// ---- 打包 (通用: 任意帧类型) ----
// 返回帧总长度; payload 为 nullptr 或 plen<=0 时返回 0。
//
// 帧布局 (plen = payload 长度):
//   下标 0..4     : A5 5C len seq type      <- 头 5 字节
//   下标 5..4+plen: payload                  <- plen 字节
//   下标 5+plen..6+plen: CRC16 (低字节在前)   <- 2 字节
//   总长度 = 5 + plen + 2 = plen + 7
static inline int kbdPackFrameEx(uint8_t *out, uint8_t seq, uint8_t type,
                                 const uint8_t *payload, int plen)
{
    if (!out || !payload || plen <= 0 || plen > KBD_MAX_REPORT) return 0;

    out[0] = KBD_MAGIC0;
    out[1] = KBD_MAGIC1;
    out[2] = (uint8_t)plen;
    out[3] = seq;
    out[4] = type;
    memcpy(&out[5], payload, (size_t)plen);

    // CRC 覆盖: len, seq, type, payload  (即下标 2 起, 共 2+plen 字节)
    const int crcStart = 2;
    const int crcLen   = 2 + plen;
    const uint16_t crc = kbdCrc16(&out[crcStart], crcLen);

    // ★ CRC 紧跟在 payload 之后, 即从下标 5+plen 开始。
    //   (crcStart + crcLen == 2 + 2 + plen == plen + 4, 而 payload 已占到
    //    下标 4+plen, 所以必须用 5+plen, 不能直接用 crcStart+crcLen —— 那会
    //    少 1 字节并把 CRC 写进 payload 尾部, 导致对端永远校验失败。)
    const int crcPos = 5 + plen;
    out[crcPos]     = (uint8_t)(crc & 0xFF);
    out[crcPos + 1] = (uint8_t)((crc >> 8) & 0xFF);

    return crcPos + 2;      // = plen + 7
}

// ---- 打包: 键盘原始报告 (右板 -> 左板) ----
static inline int kbdPackFrame(uint8_t *out, uint8_t seq,
                               const uint8_t *payload, int plen)
{
    return kbdPackFrameEx(out, seq, KBD_TYPE_REPORT, payload, plen);
}

// ---- 打包: 键盘 LED 状态 (左板 -> 右板) ----
static inline int kbdPackLed(uint8_t *out, uint8_t seq, uint8_t ledBits)
{
    return kbdPackFrameEx(out, seq, KBD_TYPE_LED, &ledBits, 1);
}

// ---- 打包: 真键盘/鼠标设备身份 + 角色 (右板 -> 左板) ----
// 左板靠 role 决定枚举【纯键盘】还是【纯鼠标】描述符。
static inline int kbdPackIdEx(uint8_t *out, uint8_t seq,
                              uint16_t vid, uint16_t pid, uint16_t bcd,
                              uint8_t role)
{
    uint8_t p[KBD_ID_LEN];
    p[0] = (uint8_t)(vid & 0xFF); p[1] = (uint8_t)(vid >> 8);
    p[2] = (uint8_t)(pid & 0xFF); p[3] = (uint8_t)(pid >> 8);
    p[4] = (uint8_t)(bcd & 0xFF); p[5] = (uint8_t)(bcd >> 8);
    p[6] = role;
    return kbdPackFrameEx(out, seq, KBD_TYPE_ID, p, KBD_ID_LEN);
}

// 兼容旧签名 (role = 未知)
static inline int kbdPackId(uint8_t *out, uint8_t seq,
                            uint16_t vid, uint16_t pid, uint16_t bcd)
{
    return kbdPackIdEx(out, seq, vid, pid, bcd, KBD_ROLE_UNKNOWN);
}

// ---- 解包 (通用) ----
// 返回 payload 长度(>0 成功), 0 表示校验失败。
// typeOut 回传帧类型(可为 nullptr)。
static inline int kbdUnpackFrameEx(const uint8_t *frame, int flen,
                                   uint8_t *seqOut, uint8_t *typeOut,
                                   const uint8_t **payloadOut)
{
    if (!frame || flen < 7) return 0;
    if (frame[0] != KBD_MAGIC0 || frame[1] != KBD_MAGIC1) return 0;

    const uint8_t type = frame[4];
    if (type != KBD_TYPE_REPORT && type != KBD_TYPE_LED && type != KBD_TYPE_ID) {
        return 0;
    }

    const int plen = frame[2];
    if (plen <= 0 || plen > KBD_MAX_REPORT) return 0;
    if (flen < 5 + plen + 2) return 0;

    const uint16_t crcCalc = kbdCrc16(&frame[2], 2 + plen);
    const uint16_t crcRecv = (uint16_t)frame[5 + plen] |
                             ((uint16_t)frame[5 + plen + 1] << 8);
    if (crcCalc != crcRecv) return 0;

    if (seqOut)     *seqOut = frame[3];
    if (typeOut)    *typeOut = type;
    if (payloadOut) *payloadOut = &frame[5];
    return plen;
}

// 兼容旧签名: 只接受键盘报告帧
static inline int kbdUnpackFrame(const uint8_t *frame, int flen,
                                 uint8_t *seqOut,
                                 const uint8_t **payloadOut)
{
    uint8_t type = 0;
    const int n = kbdUnpackFrameEx(frame, flen, seqOut, &type, payloadOut);
    if (n > 0 && type != KBD_TYPE_REPORT) return 0;
    return n;
}

#endif // KBD_WIRE_H
