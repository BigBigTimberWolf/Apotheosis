#pragma once
// ============================================================
// proto_parser.h - 私有二进制与ASCII混合协议解析器 (Device侧)
// 支持:
// 1. 标准 A5 5C 私有二进制协议
// 2. DE AD 硬件级协议 (如 Apotheosis 动态高速切波特率 0xDE 0xAD 0x05 0x00 0xA5 [baud])
// 3. 全套 ASCII 协议 (km.move, km.buttons, km.version, cmd#id 回显)
// 纯透传高性能直通模式 (双路汇总: 真实物理鼠标 + 上位机注入)
// ============================================================
#include <cstdint>
#include <cstddef>

namespace proto {

struct Stats {
    uint32_t rx_frames = 0;
    uint32_t crc_errors = 0;
    uint32_t unknown_cmds = 0;
};

class Parser {
public:
    void begin();

    void feed(const uint8_t* data, size_t len);

    // 解析器当前是否停在"帧边界"(未处于任何帧的中间, 也未处于 ASCII 行中)。
    // 供 Serial1 的双格式路由判断"这一帧是否已经消费完", 从而交还链路。
    bool atFrameBoundary() const {
        return _state == WAIT_HEADER && _ascii_pos == 0;
    }

    const Stats& stats() const { return _stats; }

    // 外部能力回调
    void (*onMove)(int16_t dx, int16_t dy) = nullptr;
    void (*onMoveTo)(int16_t x, int16_t y) = nullptr;
    void (*onButtonMask)(uint8_t mask) = nullptr;
    void (*onClick)(uint8_t btn_bits, uint16_t down_ms) = nullptr;
    void (*onWheel)(int8_t delta) = nullptr;
    void (*onPanic)() = nullptr;
    void (*onMoveCancel)() = nullptr;   // 0x05: 清空设备侧待发出的位移/滚轮积压
    void (*onAsciiLine)(const uint8_t *data, size_t len) = nullptr;  // 旧行协议整行回落
    void (*onSetBaud)(uint32_t baud) = nullptr;
    void (*onReboot)() = nullptr;
    void (*onSubAsync)(bool enable) = nullptr;

    // 键盘相关的回调 (onKeyMask / onKeyTap / onKbReport / onKbRaw / onKbHeartbeat)
    // 已全部删除 —— 本板只做鼠标, 键盘由独立的 KBD_PASSTHROUGH 固件负责。
    // 对应的命令码 0x21/0x22/0x23 在 proto_parser.cpp 里也一并删掉了。

    bool (*sendFrame)(const uint8_t* data, size_t len, void* ctx) = nullptr;
    void* send_ctx = nullptr;

private:
    enum St : uint8_t {
        WAIT_HEADER,
        WAIT_A5_5C,
        WAIT_LEN_A5,
        GET_REST_A5,
        WAIT_DEAD_LEN0,
        WAIT_DEAD_LEN1,
        GET_REST_DEAD
    };

    void dispatchA5(uint8_t seq, uint8_t cmd, const uint8_t* pl, uint8_t len);
    void dispatchDead(uint8_t cmd, const uint8_t* pl, uint16_t len);
    void feed_ascii(uint8_t b);

    St      _state = WAIT_HEADER;
    uint8_t _raw[256];
    uint8_t _pos = 0;
    uint16_t _len = 0;
    uint16_t _need = 0;

    // ASCII 行缓冲
    uint8_t _ascii[512];
    uint16_t _ascii_pos = 0;

    Stats   _stats{};
};

} // namespace proto
