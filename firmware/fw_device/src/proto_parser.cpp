// ============================================================
// proto_parser.cpp - 双协议+ASCII混合解析器实现 (直通透传与双路汇总模式)
// 1. A5 5C | LEN | SEQ | CMD | PAYLOAD[LEN] | CRC16
// 2. DE AD | LEN_LO LEN_HI | CMD | PAYLOAD[LEN-1] (兼容Apotheosis高速切波特率 0xA5)
// 3. ASCII 行回落 (km.* 命令，完美适配上位机 PID 注入)
// ============================================================
#include "proto_parser.h"
#include <cstring>
#include <Arduino.h>

extern volatile uint8_t g_cmd_source;

namespace proto {

static inline uint16_t crc16_modbus(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= *d++;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

void Parser::begin() {
    _state = WAIT_HEADER;
    _pos = 0;
    _len = 0;
    _need = 0;
    _ascii_pos = 0;
    _stats = Stats{};
}

void Parser::feed(const uint8_t* data, size_t len) {
    for (size_t k = 0; k < len; ++k) {
        uint8_t b = data[k];

        switch (_state) {
        case WAIT_HEADER:
            if (b == 0xA5) {
                _state = WAIT_A5_5C;
            } else if (b == 0xDE) {
                _state = WAIT_DEAD_LEN0;
            } else {
                feed_ascii(b);
            }
            break;

        case WAIT_A5_5C:
            if (b == 0x5C) {
                _raw[0] = 0xA5; _raw[1] = 0x5C; _pos = 2;
                _state = WAIT_LEN_A5;
            } else {
                feed_ascii(0xA5); feed_ascii(b);
                _state = WAIT_HEADER;
            }
            break;

        case WAIT_LEN_A5:
            if (b > 244) { _state = WAIT_HEADER; feed_ascii(b); break; }
            _len = b; _raw[_pos++] = b;
            _need = 3 + _len + 2 - 1;   // SEQ+CMD + PAYLOAD + CRC, 减去LEN本身
            _state = GET_REST_A5;
            break;

        case GET_REST_A5:
            _raw[_pos++] = b;
            if (--_need == 0) {
                uint16_t calc = crc16_modbus(_raw + 2, 3 + _len);
                uint16_t rx   = (uint16_t)_raw[5 + _len] |
                                ((uint16_t)_raw[6 + _len] << 8);
                if (calc == rx) {
                    _stats.rx_frames++;
                    dispatchA5(_raw[3], _raw[4], _raw + 5, _len);
                } else {
                    _stats.crc_errors++;
                }
                _state = WAIT_HEADER;
            }
            break;

        case WAIT_DEAD_LEN0:
            if (b == 0xAD) {
                _pos = 0;
                _state = WAIT_DEAD_LEN1;
            } else {
                feed_ascii(0xDE); feed_ascii(b);
                _state = WAIT_HEADER;
            }
            break;

        case WAIT_DEAD_LEN1:
            _len = b;
            _state = GET_REST_DEAD;
            _need = 0; // next byte is high byte of len
            break;

        case GET_REST_DEAD:
            if (_need == 0) {
                // 读取 length 高字节
                _len |= ((uint16_t)b << 8);
                // 用 >= 而非 > : _len == sizeof(_raw) 时, dispatchDead 会以
                // pl = _raw + 1 读最多 4 字节(pt[3]), 即访问 _raw[4], 而最多
                // 写入 _raw[255] —— 当 _len == 256 时 _len-1 == 255 个字节恰好
                // 填满 _raw, 但 dispatchDead 的 pl[3] 会越界读到 _raw[_len]。
                // 原守卫 `_len > sizeof(_raw)` 会放行 256, 故改为 >=。
                if (_len >= sizeof(_raw) || _len == 0) {
                    _state = WAIT_HEADER;
                    break;
                }
                _need = _len;
                _pos = 0;
            } else {
                _raw[_pos++] = b;
                if (--_need == 0) {
                    // _raw 包含: cmd (1 byte) + payload (_len - 1 bytes)
                    _stats.rx_frames++;
                    dispatchDead(_raw[0], _raw + 1, _len - 1);
                    _state = WAIT_HEADER;
                }
            }
            break;
        }
    }
}

// 兼容 0xDE 0xAD 协议 (Apotheosis 切换 4000000 高波特率)
void Parser::dispatchDead(uint8_t cmd, const uint8_t* pl, uint16_t len) {
    g_cmd_source = 0; // 上位机注入
    if (cmd == 0xA5 && len >= 4) { // 切换波特率
        uint32_t baud = (uint32_t)pl[0] | ((uint32_t)pl[1] << 8) |
                        ((uint32_t)pl[2] << 16) | ((uint32_t)pl[3] << 24);
        if (onSetBaud) {
            onSetBaud(baud);
        }
    }
}

void Parser::dispatchA5(uint8_t seq, uint8_t cmd, const uint8_t* pl, uint8_t len) {
    g_cmd_source = 0; // 上位机注入

    auto rd_i16 = [&](int o) -> int16_t {
        return (int16_t)((uint16_t)pl[o] | ((uint16_t)pl[o + 1] << 8));
    };
    auto rd_u16 = [&](int o) -> uint16_t {
        return (uint16_t)pl[o] | ((uint16_t)pl[o + 1] << 8);
    };
    auto rd_u32 = [&](int o) -> uint32_t {
        return (uint32_t)pl[o] | ((uint32_t)pl[o + 1] << 8) |
               ((uint32_t)pl[o + 2] << 16) | ((uint32_t)pl[o + 3] << 24);
    };

    // 注意: sendFrame 在本固件中【从未被赋值】(g_proto 未设, g_proto1 显式 nullptr),
    // 因此 send_resp/ack 全部是空操作 —— 设备不发任何 ACK / 0x82 VERSION / 0x83 STATS。
    // 保留这段是为了协议对称性与将来启用; 上位机必须 ACK-free(见 docs/proto.md §1.1)。
    // 改为栈缓冲: 原实现用 std::vector 在热路径上做堆分配, 而本函数可能由
    // 高频按键事件触发, ESP32-S3 的堆操作需持锁, 会造成延迟抖动与碎片。
    auto send_resp = [&](uint8_t resp_cmd, const uint8_t* p, uint8_t plen) {
        if (!sendFrame) return;
        if ((size_t)plen + 7 > 251) return;      // kMaxPayload(244) + 7
        uint8_t f[251];
        f[0] = 0xA5; f[1] = 0x5C; f[2] = plen; f[3] = seq; f[4] = resp_cmd;
        if (plen) memcpy(f + 5, p, plen);
        uint16_t c = crc16_modbus(f + 2, 3 + plen);
        f[5 + plen] = (uint8_t)(c & 0xFF);
        f[6 + plen] = (uint8_t)((c >> 8) & 0xFF);
        sendFrame(f, (size_t)plen + 7, send_ctx);
    };

    auto ack = [&](uint8_t status) {
        uint8_t p[2] = { cmd, status };
        send_resp(0x80, p, 2);
    };

    switch (cmd) {
    // ---- 移动类: 直通无延迟输出 ----
    case 0x01: // MOVE
    case 0x02: // MOVE_RAW
        if (len >= 4 && onMove) {
            onMove(rd_i16(0), rd_i16(2));
        }
        break;

    case 0x03: // MOVE_BATCH
        if (len >= 4 && len % 4 == 0 && onMove) {
            int32_t sx = 0, sy = 0;
            uint8_t n = len / 4;
            for (uint8_t i = 0; i < n; ++i) {
                sx += rd_i16(i * 4);
                sy += rd_i16(i * 4 + 2);
            }
            // 原实现直接把 int32 累加和截断成 int16, 大批次(最多 61 点)会回绕成反向位移。
            // 按本固件"绝不丢步"的原则, 超出 int16 时拆成多次 onMove, 保证总位移精确。
            // 正常输入(和落在 int16 内)仍然只调用一次 onMove, 与文档 §2.1 描述一致。
            while (sx != 0 || sy != 0) {
                int16_t stepX = (int16_t)(sx > 32767 ? 32767 : (sx < -32768 ? -32768 : sx));
                int16_t stepY = (int16_t)(sy > 32767 ? 32767 : (sy < -32768 ? -32768 : sy));
                onMove(stepX, stepY);
                sx -= stepX;
                sy -= stepY;
            }
        }
        break;

    case 0x04: // MOVETO
        if (len >= 4 && onMoveTo) {
            onMoveTo(rd_i16(0), rd_i16(2));
        }
        break;

    case 0x05: // MOVE_CANCEL: 丢弃设备侧尚未发出的位移/滚轮积压(上位机停火)
        if (onMoveCancel) onMoveCancel();
        break;

    // ---- 按键/点击/滚轮 ----
    case 0x10:
        if (len >= 1 && onButtonMask) {
            onButtonMask(pl[0]);
        }
        break;

    case 0x12:
        if (len >= 3 && onClick) {
            onClick(pl[0], rd_u16(1));
            ack(0);
        }
        break;

    case 0x20:
        if (len >= 1 && onWheel) {
            onWheel((int8_t)pl[0]);
        }
        break;

    // 0x21 KEY_MASK / 0x22 KEY_TAP / 0x23 KB_REPORT 已删除。
    //
    // 本板（MAKCUNEW）现在只做鼠标。键盘由独立的 KBD_PASSTHROUGH 固件负责,
    // 那是另一套代码, 与这里无关。
    // 这三个命令码现在会落到 default 分支被忽略 —— 刻意不在这里"接受但丢弃",
    // 免得看起来像还支持键盘。

    case 0x48:
        if (len >= 1 && onSubAsync) { onSubAsync(pl[0] != 0); ack(0); }
        break;

    case 0x40: {
        uint8_t p[18] = {0};
        strncpy((char*)p, "PASSTHROUGH_V1", 16);
        send_resp(0x82, p, 18);
        break;
    }
    case 0x41: {
        uint8_t p[16];
        memcpy(p,     &_stats.crc_errors, 4);
        memcpy(p + 4, &_stats.rx_frames, 4);
        memset(p + 8, 0, 4);
        uint32_t up = millis() / 1000; memcpy(p + 12, &up, 4);
        send_resp(0x83, p, 16);
        break;
    }
    case 0x42: if (len >= 4 && onSetBaud) onSetBaud(rd_u32(0)); ack(0); break;
    case 0x43: ack(0); break;
    case 0x44: if (onPanic) onPanic(); ack(0); break;
    case 0x45: if (onReboot) onReboot(); break;

    default:
        _stats.unknown_cmds++;
        break;
    }
}

void Parser::feed_ascii(uint8_t b) {
    if (b == '\r') return;

    if (_ascii_pos >= sizeof(_ascii) - 1) { _ascii_pos = 0; return; }
    if (b != '\n') { _ascii[_ascii_pos++] = b; return; }

    _ascii[_ascii_pos] = 0;
    if (onAsciiLine && _ascii_pos > 0)
        onAsciiLine(_ascii, _ascii_pos);
    _ascii_pos = 0;
}

} // namespace proto
