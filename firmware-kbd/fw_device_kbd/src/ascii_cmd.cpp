// ============================================================================
// ascii_cmd.cpp —— ASCII 文本命令通道 (CH343)
//
// 【为什么必须有这个文件】
//   Apotheosis 的 MakcuNewConnection 用【两条通道】跟固件说话:
//
//     通道1: 二进制帧 (makcu_proto.h)  -> 位移/按键/滚轮等高频数据
//     通道2: ASCII 文本行 (本文件)      -> 握手/屏蔽等低频控制
//
//   本工程最初只实现了通道1 —— 结果上位机【连不上】。因为 connect() 的第一步
//   就是通道2: 它打开串口 @115200, 发 "km.version\r\n", 然后在 500ms 内等一行
//   包含 "MAKCU-PASSTHROUGH" 的应答。不回这一行, connect() 直接失败并断开。
//   => 这一行是整个链路的【第一道门】, 没有它后面什么都做不了。
//
// 【握手流程 (摘自 MakcuNew.cpp 的实际行为)】
//   上位机: 打开串口 @115200 (kBootBaud)
//           发 "km.version\r\n"
//           500ms 内等一行含 "MAKCU-PASSTHROUGH" 的文本
//   固件:   必须回一行含该字样的文本, 否则判定连接失败
//
// 【上位机如何切分收到的字节 (MakcuNew.cpp feedAsciiByte) —— 这决定了应答格式】
//     '\n'                 -> 一行结束
//     '\r'                 -> 忽略
//     0x20..0x7E (可打印)  -> 累积进当前行 (上限 256 字节)
//     < 0x20 (其它控制符)  -> 被当成"物理按键上报"处理 (有语义!)
//     >= 0x7F             -> 清空当前累积
//
//   三条结论:
//     1) 应答必须是【纯可打印 ASCII + '\n' 结尾】, 不能混入二进制
//     2) 固件不能往 CH343 上打 < 0x20 的非 \r\n 字节 —— 会被误解成按键上报
//     3) 固件不能往 CH343 上打 >= 0x7F 的字节 —— 会清空上位机的行累积
//        而二进制协议帧里恰好有 0xA5/0x5C 这类字节, 所以【探测期间绝不能
//        发二进制帧】。本工程本来就不主动发二进制帧(ACK-free), 天然满足。
//
// 【应答格式与 MAKCUNEW 完全一致】
//   MAKCUNEW fw_device 的 sendTrackedResponse():
//     命令行不含 '#' -> 回 "<result>\r\n"
//     命令行含 '#'   -> 回 ">>> #<id>:<result>\r\n"   (追踪应答, id=atoi(#之后))
//   这里逐字照抄, 保证上位机无论用哪种形式都能认。
//
// 【km.mask 为什么不在本串口应答】
//   MakcuNewConnection::mask() 明确写着"写完即认为成功, 不等应答", 所以屏蔽
//   命令是即发即忘的。回一行反而会让上位机那边多出一条无法归属的文本行。
//   MAKCUNEW 也只把 "km.mask(N) ok" 回到板间链路, 不回本串口 —— 保持一致。
// ============================================================================

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "ascii_cmd.h"

// ---------------------------------------------------------------------------
// 由 makcu_link.cpp 提供的屏蔽控制
//
// 【为什么屏蔽状态放在 makcu_link.cpp】
//   屏蔽的本质是"合并时丢弃真实输入" —— 那是合并逻辑的一部分, 必须和真实帧
//   的到达路径放在一起, 否则会出现"命令已生效但某一帧仍被合并进去"的窗口。
// ---------------------------------------------------------------------------
void makcuSetMask(uint32_t ms);
void makcuClearMask(void);

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
static char s_line[128];
static int  s_len = 0;

static bool s_active     = false;   // 收到过上位机的可打印字节
static bool s_handshaked = false;   // 收到过 km.version

bool asciiCmdActive(void)     { return s_active; }
bool asciiCmdHandshaked(void) { return s_handshaked; }

// ---------------------------------------------------------------------------
// 发一行 (以 \r\n 结尾, 与 MAKCUNEW 的 println 行为一致)
// ---------------------------------------------------------------------------
static void sendLine(const char *s)
{
    Serial.print(s);
    Serial.print("\r\n");
}

// ---------------------------------------------------------------------------
// 追踪应答: 与 MAKCUNEW sendTrackedResponse 逐字一致
//   无 '#' -> "<result>\r\n"
//   有 '#' -> ">>> #<id>:<result>\r\n"
// ---------------------------------------------------------------------------
static void sendTrackedResponse(const char *origCommand, const char *result)
{
    const char *hash = strchr(origCommand, '#');
    if (hash) {
        Serial.print(">>> #");
        Serial.print(atoi(hash + 1));
        Serial.print(":");
        Serial.print(result);
        Serial.print("\r\n");
    } else {
        sendLine(result);
    }
}

// ---------------------------------------------------------------------------
// 命令行分派
// ---------------------------------------------------------------------------
static void handleLine(const char *line)
{
    // ---- 版本握手: 整个链路的第一道门 ----
    if (strncmp(line, "km.version", 10) == 0) {
        s_handshaked = true;
        sendTrackedResponse(line, "MAKCU-PASSTHROUGH-1.0.0");
        return;
    }

    // ---- 屏蔽真实输入 ----
    //
    // km.maskoff 必须排在 km.mask( 前面判断吗? 其实不必: "km.maskoff" 的第 8 个
    // 字符是 'o', 而 "km.mask(" 要求 '(' —— 两者不可能互相前缀匹配。这里保持
    // MAKCUNEW 的书写顺序(off 在前), 便于对照。
    if (strncmp(line, "km.maskoff", 10) == 0) {
        makcuClearMask();
        return;
    }

    if (strncmp(line, "km.mask(", 8) == 0) {
        int ms = 0;
        if (sscanf(line + 8, "%d", &ms) == 1) {
            // <=0 视为解除, 与上位机的约定一致 (mask(<=0) 走解除语义)
            if (ms <= 0) {
                makcuClearMask();
            } else {
                // 上限钳制: 防止失控调用方用一次极大值把设备焊死在屏蔽态。
                // 上限值与上位机 kMaskMaxMs 保持一致。
                if ((uint32_t)ms > MAKCU_MASK_MAX_MS) ms = MAKCU_MASK_MAX_MS;
                makcuSetMask((uint32_t)ms);
            }
        }
        // 刻意不应答 —— 见文件头说明
        return;
    }

    // ---- 按键异步上报开关 ----
    //
    // 上位机连上后立刻发 km.buttons(1)。本工程在键盘单板上没有物理鼠标按键
    // 需要上报, 所以只回应答、不实现上报流。应答必须给, 否则上位机可能认为
    // 这条命令没生效。
    if (strncmp(line, "km.buttons", 10) == 0) {
        sendTrackedResponse(line, "OK");
        return;
    }

    // ---- 其余 km.* 命令 ----
    //
    // Apotheosis 是 ACK-free 的(它只对 km.version / km.getpos 这类查询等应答),
    // 所以不认识的命令【静默忽略】是正确行为; 回一行反而会污染它的文本通道。
    // 这里刻意什么都不做。
}

// ---------------------------------------------------------------------------
// 喂入一个字节
//
// ★ 调用时机由 makcu_link.cpp 决定: 只在【二进制帧解析器空闲】时喂进来。
//   这样二进制帧的 payload 字节不会被误当成 ASCII 文本累积 —— 否则 payload 里
//   恰好出现 0x0A 时会产生一行垃圾, 万一撞上 km.* 前缀就会被误执行。
// ---------------------------------------------------------------------------
void asciiCmdFeed(uint8_t b)
{
    if (b == '\n') {
        if (s_len > 0) {
            s_line[s_len] = '\0';
            s_active = true;
            handleLine(s_line);
        }
        s_len = 0;
        return;
    }

    if (b == '\r') return;

    if (b >= 0x20 && b < 0x7F) {
        if (s_len == 0) s_active = true;
        if (s_len < (int)sizeof(s_line) - 1) {
            s_line[s_len++] = (char)b;
        } else {
            // 行太长: 丢弃并重新累积。不能截断后继续, 否则前缀匹配可能误命中。
            s_len = 0;
        }
        return;
    }

    // 非可打印字节: 清空累积。上位机不会在文本行中间插控制字节, 所以看到它
    // 只可能是二进制残留 —— 清掉才安全。
    s_len = 0;
}
