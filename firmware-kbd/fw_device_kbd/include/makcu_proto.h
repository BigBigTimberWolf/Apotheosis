// ============================================================================
// makcu_proto.h —— 与 Apotheosis 上位机对接的协议 (CH343 串口)
//
// 本文件与 Apotheosis\makcu_proto.h 【保持完全一致】:
//   魔数 0xA5 0x5C / len / seq / cmd / CRC16-MODBUS(低字节在前) / 总长 = len+7
//
// 【为什么可以直接用】
//   板间协议(kbd_wire.h)当初无意中做成了同样的帧格式, 所以:
//     - 上位机发来的帧, 和右板发来的帧, 结构完全相同
//     - 只是 cmd/type 的取值范围不同, 可以靠这个区分来源
//
// 【命令一览 (来自 Apotheosis)】
//   鼠标类:
//     0x01 MOVE          int16 dx, int16 dy
//     0x02 MOVE_RAW      原始位移
//     0x03 MOVE_BATCH    批量(固件内求和后只输出一次)
//     0x04 MOVETO        绝对移动
//     0x05 MOVE_CANCEL   清空积压(停火)
//     0x10 BUTTON_MASK   u8 mask            绝对态
//     0x12 CLICK         u8 btn, u16 down_ms 固件内定时弹起
//     0x20 WHEEL         int8 delta
//   键盘类:
//     0x21 KEY_MASK      u8 mod + u8 keys[6] 绝对态
//     0x22 KEY_TAP       u8 mod + u8 key + u16 hold_ms  固件内自清
//   控制类:
//     0x40 GET_VERSION   0x41 GET_STATS   0x42 SET_BAUD
//     0x43 GHOST_MODE    ★ 屏蔽真实输入
//     0x44 PANIC         0x45 REBOOT      0x48 SUB_ASYNC
//   上报:
//     0x84 ASYNC_BUTTON  u8 real_mask + u8 inj_mask
// ============================================================================

#ifndef MAKCU_PROTO_H
#define MAKCU_PROTO_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// 帧常量
// ---------------------------------------------------------------------------
#define MAKCU_MAGIC0        0xA5
#define MAKCU_MAGIC1        0x5C
#define MAKCU_MAX_PAYLOAD   32
#define MAKCU_MAX_FRAME     (5 + MAKCU_MAX_PAYLOAD + 2)

// ---------------------------------------------------------------------------
// 命令号 (与 Apotheosis makcu_proto.h 完全一致)
// ---------------------------------------------------------------------------
enum MakcuCmd : uint8_t {
    CMD_MOVE            = 0x01,
    CMD_MOVE_RAW        = 0x02,
    CMD_MOVE_BATCH      = 0x03,
    CMD_MOVETO          = 0x04,
    CMD_MOVE_CANCEL     = 0x05,
    CMD_BUTTON_MASK     = 0x10,
    CMD_BUTTON_MASK_EX  = 0x11,
    CMD_CLICK           = 0x12,
    CMD_WHEEL           = 0x20,
    CMD_KEY_MASK        = 0x21,
    CMD_KEY_TAP         = 0x22,
    CMD_GET_VERSION     = 0x40,
    CMD_GET_STATS       = 0x41,
    CMD_SET_BAUD        = 0x42,
    CMD_GHOST_MODE      = 0x43,
    CMD_PANIC           = 0x44,
    CMD_REBOOT          = 0x45,
    CMD_SUB_ASYNC       = 0x48,

    CMD_ACK             = 0x80,
    CMD_NAK             = 0x81,
    CMD_VERSION_RESP    = 0x82,
    CMD_STATS_RESP      = 0x83,
    CMD_ASYNC_BUTTON    = 0x84,
};

// ---------------------------------------------------------------------------
// 鼠标按键位 (与 Apotheosis 一致)
// ---------------------------------------------------------------------------
enum MakcuBtn : uint8_t {
    MAKCU_BTN_L  = 1 << 0,
    MAKCU_BTN_R  = 1 << 1,
    MAKCU_BTN_M  = 1 << 2,
    MAKCU_BTN_S1 = 1 << 3,
    MAKCU_BTN_S2 = 1 << 4,
};

// ---------------------------------------------------------------------------
// CRC16/MODBUS (与 Apotheosis 完全一致)
// ---------------------------------------------------------------------------
static inline uint16_t makcuCrc16(const uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001)
                            : (uint16_t)(crc >> 1);
    }
    return crc;
}

// ---------------------------------------------------------------------------
// 解包: 校验魔数 / 长度 / CRC
//
// 返回 payload 长度(>=0), -1 表示无效。
// ★ 注意 payload 长度可以是 0 (例如 PANIC / MOVE_CANCEL 无参数),
//   这与板间协议不同 —— 板间协议的 payload 恒 > 0。
// ---------------------------------------------------------------------------
static inline int makcuUnpack(const uint8_t *f, int flen,
                              uint8_t *cmdOut, const uint8_t **payloadOut)
{
    if (!f || flen < 7) return -1;
    if (f[0] != MAKCU_MAGIC0 || f[1] != MAKCU_MAGIC1) return -1;

    const int plen = f[2];
    if (plen < 0 || plen > MAKCU_MAX_PAYLOAD) return -1;
    if (flen < 5 + plen + 2) return -1;

    // ★ CRC 覆盖范围必须是 3 + plen: LEN, SEQ, CMD, 以及【整个】payload.
    //
    //   这里原本写的是 2 + plen —— 少算了一个字节, 代价是致命的:
    //   按协议(docs/makcu_proto.h 的 build_frame, 以及 MAKCUNEW
    //   fw_device/src/proto_parser.cpp 的 crc16_modbus(.., 3 + _len))算出来的
    //   CRC 与本函数算出的不一致, 于是【上位机发来的每一帧】都被判为 CRC 错误,
    //   静默计入 s_crcErr 后丢弃 —— 握手(ASCII)照样成功, 所以看起来"连上了",
    //   但所有二进制命令(屏蔽/注入/鼠标)全部无效。
    //   plen=1 时(例如 GHOST_MODE) 2+plen 甚至只覆盖到 CMD, 连 payload 都不校验。
    const uint16_t calc = makcuCrc16(&f[2], 3 + plen);
    const uint16_t recv = (uint16_t)f[5 + plen] |
                          ((uint16_t)f[5 + plen + 1] << 8);
    if (calc != recv) return -1;

    if (cmdOut)     *cmdOut = f[4];
    if (payloadOut) *payloadOut = &f[5];
    return plen;
}

// ---- 小端读取辅助 ----
static inline int16_t  makcuI16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static inline uint16_t makcuU16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t makcuU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif // MAKCU_PROTO_H
