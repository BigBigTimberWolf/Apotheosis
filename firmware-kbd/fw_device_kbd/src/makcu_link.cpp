// ============================================================================
// makcu_link.cpp —— 左板: CH343 上位机协议 + 真实输入合并 + 报文发射
//
// 【数据流】
//
//   右板 ──Serial1──> [真实设备原始报文] ──┐
//                                          ├─> 合并 ─> USB ─> 被控机
//   上位机 ─CH343──> [makcu 注入命令]   ──┘
//
// 【GHOST 语义 (按需求)】
//   GHOST 开启: 丢弃真实输入, 只发注入。
//   期间上位机通常也不发注入 -> 结果就是"完全静默"。
//
// 【发射节流】
//   每 REPORT_INTERVAL_MS 取一次合并快照发送。
//   只在状态变化时真正调用 USB 发送, 避免无效报文占用总线。
// ============================================================================

#include <Arduino.h>
#include "makcu_proto.h"
#include "kbd_wire.h"
#include "kbd_usb.h"
#include "merge.h"
#include "ascii_cmd.h"

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
#define REPORT_INTERVAL_MS   1      // 发射周期: 1ms (1000Hz)

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
static KbdState   s_realKbd;         // 真键盘当前状态 (来自右板)
static KbdState   s_injKbd;          // 注入的键盘状态
static MouseState s_realMouse;       // 真鼠标
static MouseState s_injMouse;        // 注入的鼠标

static bool s_ghost = false;         // true = 丢弃真实输入 (二进制 0x43 GHOST_MODE)

// ---------------------------------------------------------------------------
// 定时屏蔽 (ASCII "km.mask(N)" / "km.maskoff")
//
// 【与 s_ghost 的关系】
//   两者语义相同, 都是"丢弃真实输入", 区别只在生命周期:
//     s_ghost     : 持续态, 由二进制 0x43 开关, 不会自己结束
//     s_maskUntil : 定时态, 由 ASCII km.mask(N) 开启, N 毫秒后【自己结束】
//
// 【为什么必须有硬超时】
//   屏蔽期间的语义是"完全静默": 真实输入被丢弃, 而上位机在这段时间通常也不发
//   注入。所以一旦屏蔽被永久留在开启状态, 用户会看到"键鼠彻底失灵"——
//   而且没有任何东西会把它恢复回来(必须重新上电)。上位机崩溃/断线时尤其危险。
//   硬超时是解除屏蔽的【唯一保证路径】, 不依赖上位机再发一条解除命令。
//   上限 2000ms 与上位机 kMaskMaxMs 一致。
//
// 【0 是哨兵】
//   s_maskUntilMs == 0 表示"未屏蔽"。写入前若算出恰好为 0 则改成 1,
//   否则一次极短屏蔽会被当成"从未屏蔽"。
// ---------------------------------------------------------------------------
static uint32_t s_maskUntilMs = 0;

void makcuClearMask(void);   // 前置声明: makcuSetMask(0) 会用到

void makcuSetMask(uint32_t ms)
{
    if (ms == 0) { makcuClearMask(); return; }
    s_maskUntilMs = millis() + ms;
    if (s_maskUntilMs == 0) s_maskUntilMs = 1;

    // 进入屏蔽时【不】清空 s_realKbd/s_realMouse。
    //
    // 这里与 0x43 GHOST_MODE 的处理刻意不同, 原因:
    //   屏蔽窗口内真实报文仍在到达并持续更新 s_real* —— 这是好事。用户此刻
    //   按着哪些键是【已知且最新】的。窗口结束时直接恢复合并, 按住不放的键
    //   会立刻重新生效, 不需要用户松手重按。
    //   若在这里清空, 而接收器又是"只在变化时发帧"的类型, 那么屏蔽结束后
    //   这些键就再也回不来了(要等用户松手重按)。
    //
    // 窗口内主机端看到的效果仍然正确: 合并时跳过真实部分, 于是屏蔽开始那一刻
    // 正按着的键在下一个发射周期就被释放掉了。
}

void makcuClearMask(void)
{
    s_maskUntilMs = 0;
}

bool makcuMaskActive(void)
{
    return s_maskUntilMs != 0;
}

// 真实输入此刻是否应当被丢弃 (持续态 OR 定时态)
static inline bool realInputDropped(void)
{
    return s_ghost || (s_maskUntilMs != 0);
}

// KEY_TAP 的固件内定时弹起
static uint32_t s_tapUntil = 0;
static KbdState s_tapState;

// 统计
static uint32_t s_cmdCount   = 0;    // 收到的上位机命令数
static uint32_t s_crcErr     = 0;
static uint32_t s_realFrames = 0;
static uint32_t s_ghostDrop  = 0;
static bool     s_linkAlive  = false;  // 是否收到过上位机命令 (用于日志静音)

// ---------------------------------------------------------------------------
// 设备角色 (由右板告知): 0=未知 1=键盘 2=鼠标
// ---------------------------------------------------------------------------
int volatile g_deviceRole = 0;

// ---------------------------------------------------------------------------
// 日志静音: 收到第一帧上位机命令后永久停止文本日志, 避免污染协议流
// ---------------------------------------------------------------------------
bool makcuLinkAlive(void) { return s_linkAlive; }

// ---------------------------------------------------------------------------
// 处理一条上位机命令
// ---------------------------------------------------------------------------
static void handleCommand(uint8_t cmd, const uint8_t *p, int plen)
{
    s_cmdCount++;

    switch (cmd) {
    // ---------------- 鼠标 ----------------
    case CMD_MOVE:
        if (plen >= 4) {
            s_injMouse.dx += makcuI16(p);
            s_injMouse.dy += makcuI16(p + 2);
        }
        break;

    case CMD_MOVE_RAW:
        if (plen >= 4) {
            s_injMouse.dx += makcuI16(p);
            s_injMouse.dy += makcuI16(p + 2);
        }
        break;

    case CMD_MOVE_BATCH:
        // 固件内求和后只输出一次位移
        if (plen >= 4) {
            int32_t sx = 0, sy = 0;
            for (int i = 0; i + 3 < plen; i += 4) {
                sx += makcuI16(p + i);
                sy += makcuI16(p + i + 2);
            }
            if (sx >  32767) sx =  32767;
            if (sx < -32768) sx = -32768;
            if (sy >  32767) sy =  32767;
            if (sy < -32768) sy = -32768;
            s_injMouse.dx += (int16_t)sx;
            s_injMouse.dy += (int16_t)sy;
        }
        break;

    case CMD_MOVETO:
        // 绝对移动: 无当前位置信息, 暂按相对处理 (后续可加位置跟踪)
        if (plen >= 4) {
            s_injMouse.dx += makcuI16(p);
            s_injMouse.dy += makcuI16(p + 2);
        }
        break;

    case CMD_MOVE_CANCEL:
        // 清空积压 = 停火
        s_injMouse.clearDelta();
        break;

    case CMD_BUTTON_MASK:
        if (plen >= 1) s_injMouse.buttons = p[0] & 0x1F;
        break;

    case CMD_CLICK:
        // payload: u8 btn_bits + u16 down_ms
        // 固件内定时弹起 —— 先按下, 到点自动松开。
        // 实现: 记下按下状态和到期时间, 在主循环里检查。
        if (plen >= 3) {
            s_injMouse.buttons |= (p[0] & 0x1F);
            extern uint32_t volatile g_clickReleaseAt;
            extern uint8_t  volatile g_clickBits;
            g_clickBits = (p[0] & 0x1F);
            g_clickReleaseAt = millis() + makcuU16(p + 1);
        }
        break;

    case CMD_WHEEL:
        if (plen >= 1) s_injMouse.wheel += (int8_t)p[0];
        break;

    // ---------------- 键盘 ----------------
    case CMD_KEY_MASK:
        // payload: u8 mod + u8 keys[6]  (标准 HID 键盘报表布局)
        if (plen >= 7) {
            s_injKbd.mod = p[0];
            memcpy(s_injKbd.keys, p + 1, 6);
        }
        break;

    case CMD_KEY_TAP:
        // payload: u8 mod + u8 key + u16 hold_ms
        // 固件内定时弹起(自清) —— 不依赖上位机后续命令, 所以上位机崩了也不会卡键
        if (plen >= 4) {
            s_tapState.clear();
            s_tapState.mod = p[0];
            s_tapState.keys[0] = p[1];
            s_tapUntil = millis() + makcuU16(p + 2);
        }
        break;

    // ---------------- 控制 ----------------
    case CMD_GHOST_MODE:
        // ★ 屏蔽真实输入 (你要的功能)
        //   on  -> 丢弃真实键盘/鼠标报文
        //   off -> 恢复
        if (plen >= 1) {
            s_ghost = (p[0] != 0);
            if (s_ghost) {
                s_realKbd.clear();
                s_realMouse.clear();
            }
        }
        break;

    case CMD_PANIC:
        // 紧急停止: 清空所有注入状态
        s_injKbd.clear();
        s_injMouse.clear();
        s_tapState.clear();
        s_tapUntil = 0;
        break;

    case CMD_SUB_ASYNC:
        // 订阅按键异步上报: 暂不实现上报, 仅接受
        break;

    case CMD_SET_BAUD:
        // 暂不实现 (改波特率会打断当前会话)
        break;

    case CMD_GET_VERSION:
    case CMD_GET_STATS:
        // Apotheosis 的注释里明确写着: "固件从不发送任何二进制响应帧"
        // 所以这里不回应答, 与现有固件行为一致 (ACK-free)
        break;

    case CMD_REBOOT:
        // 交给上层处理
        extern void makcuRequestReboot(void);
        makcuRequestReboot();
        break;

    default:
        // 未知命令忽略
        break;
    }
}

// ---------------------------------------------------------------------------
// CH343: 喂入一个字节 (解析上位机协议帧)
// ---------------------------------------------------------------------------
static uint8_t s_pcBuf[MAKCU_MAX_FRAME];
static int     s_pcLen = 0;
static uint32_t s_pcLastMs = 0;   // 上一个字节的时刻, 用于残帧超时作废

void makcuLinkFeedPc(uint8_t b)
{
    // ---------------------------------------------------------------------
    // 双通道仲裁: 同一个 UART 上跑着二进制帧和 ASCII 文本行两种东西
    // ---------------------------------------------------------------------
    //
    // 【为什么需要仲裁, 不能简单地把每个字节同时喂给两个解析器】
    //   二进制帧的 payload 里完全可能出现可打印字节(比如 dx=0x41='A'),
    //   甚至可能恰好出现 0x0A('\n')。若不加区分地累积, 就会拼出一行垃圾文本;
    //   万一那行垃圾碰巧以 "km." 开头(理论上可能), 就会被当成命令执行。
    //
    // 【判据】
    //   s_pcLen == 0 表示二进制解析器处于【帧间空闲】状态, 此时到达的字节
    //   只可能是 ASCII 文本 (或一个帧的起始 0xA5)。而 0xA5 不是可打印字符,
    //   asciiCmdFeed 会直接丢弃它, 所以不会互相干扰。
    //   一旦某个字节启动了二进制帧(s_pcLen != 0), ASCII 累积就被抑制, 直到
    //   整帧收完(含 CRC 校验那一步把 s_pcLen 归零)。
    //
    // 这正是 MAKCUNEW fw_device 里"普通 ASCII 行照常进环、但不会吞掉帧体"的同款
    // 处理方式。
    //
    // 【为什么还要加空闲复位】
    //   s_pcLen 有可能永久停在 2..4 这种"半个帧"的值上: 比如线上出现一个孤立的
    //   0xA5 后面只跟了一个字节, 之后就再没有符合帧格式的数据。这时 ASCII 累积
    //   会被【永久】抑制 —— 表现为"上位机发什么命令都没反应", 而且不报错。
    //   所以只要超过 100ms 没有新字节, 就认为那个残帧作废。
    //   一帧最多 39 字节, 在 115200 下不到 4ms, 100ms 有 25 倍余量, 不会误伤
    //   正常的分帧到达。
    const uint32_t nowMs = millis();
    if (s_pcLen != 0 && (uint32_t)(nowMs - s_pcLastMs) > 100) {
        s_pcLen = 0;
    }
    s_pcLastMs = nowMs;

    if (s_pcLen == 0) {
        asciiCmdFeed(b);
    }

    if (s_pcLen == 0) {
        if (b != MAKCU_MAGIC0) return;
        s_pcBuf[s_pcLen++] = b;
        return;
    }
    if (s_pcLen == 1) {
        if (b == MAKCU_MAGIC1)      s_pcBuf[s_pcLen++] = b;
        else if (b == MAKCU_MAGIC0) s_pcBuf[0] = b;
        else                        s_pcLen = 0;
        return;
    }

    s_pcBuf[s_pcLen++] = b;

    if (s_pcLen == 5) {
        const int plen = s_pcBuf[2];
        if (plen > MAKCU_MAX_PAYLOAD) { s_pcLen = 0; return; }
    }
    if (s_pcLen >= 5) {
        const int need = 5 + s_pcBuf[2] + 2;
        if (s_pcLen == need) {
            uint8_t cmd = 0;
            const uint8_t *pl = nullptr;
            const int n = makcuUnpack(s_pcBuf, s_pcLen, &cmd, &pl);
            if (n >= 0) {
                s_linkAlive = true;          // 收到有效命令 -> 静音日志
                handleCommand(cmd, pl, n);
            } else {
                s_crcErr++;
            }
            s_pcLen = 0;
        } else if (s_pcLen > need) {
            s_pcLen = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Serial1: 喂入一个字节 (解析右板来的真实设备报文)
// ---------------------------------------------------------------------------
static uint8_t  s_hostBuf[KBD_MAX_FRAME];
static int      s_hostLen    = 0;
static uint32_t s_hostLastMs = 0;

void makcuLinkFeedHost(uint8_t b)
{
    // ---------------------------------------------------------------------
    // ★ 残帧超时 —— 没有它就会【永久卡键】
    //
    //   板间链路跑 5Mbaud, 一个满帧(最长 23 字节)传输只要约 50us。所以任何
    //   "半截帧挂在这里超过 20ms" 都只可能是丢字节/多字节造成的失步。
    //
    //   失步的后果非常隐蔽: 下面用的是 s_hostBuf[2] 里那个【陈旧】的长度来决定
    //   还要再等多少字节, 于是随后的帧会被按【错误的边界】切分。一旦【按键释放
    //   报文】被这样吞掉, s_realKbd.keys 就会永远停在"按下"状态 —— 主机于是
    //   一直自动连发, 表现就是"打 haole 变成 haleeeeeeeeeeee...", 而且不会自愈。
    //
    //   注意发射侧不可能是元凶: makcuLinkTick 每 REPORT_INTERVAL_MS(1ms) 都会
    //   用【最新】状态重发一次, 而 usb_hid.cpp 的 kbdUsbSendReport 没有任何
    //   "状态没变就跳过"的缓存, 直接把结果返回。所以卡住的一定是这里的接收状态。
    //
    //   加上超时后, 失步的残帧最多存活 20ms 就被丢弃, 解析器随即在下一个
    //   MAGIC0 处重新同步 -> 卡键会在 20ms 内自动纠正。
    // ---------------------------------------------------------------------
    const uint32_t nowMs = millis();
    if (s_hostLen > 0 && (uint32_t)(nowMs - s_hostLastMs) > 20u) {
        s_hostLen = 0;          // 丢弃失步残帧, 重新找帧头
    }
    s_hostLastMs = nowMs;

    if (s_hostLen == 0) {
        if (b != KBD_MAGIC0) return;
        s_hostBuf[s_hostLen++] = b;
        return;
    }
    if (s_hostLen == 1) {
        if (b == KBD_MAGIC1)      s_hostBuf[s_hostLen++] = b;
        else if (b == KBD_MAGIC0) s_hostBuf[0] = b;
        else                      s_hostLen = 0;
        return;
    }

    // ★ 越界防护: 上面 else-if 的 "s_hostLen > need 才复位" 判断发生在追加【之后】,
    //   所以 s_hostLen 最多能到 need+1 = 5+16+2+1 = 24, 而 s_hostBuf 只有
    //   KBD_MAX_FRAME = 23 字节 -> 会写越界 1 字节。这种内存破坏本身就可能造成
    //   各种诡异现象, 必须挡住。
    if (s_hostLen >= (int)sizeof(s_hostBuf)) { s_hostLen = 0; return; }
    s_hostBuf[s_hostLen++] = b;

    if (s_hostLen == 5) {
        const int plen = s_hostBuf[2];
        if (plen <= 0 || plen > KBD_MAX_REPORT) { s_hostLen = 0; return; }
    }
    if (s_hostLen >= 5) {
        const int need = 5 + s_hostBuf[2] + 2;
        if (s_hostLen == need) {
            uint8_t seq = 0, type = 0;
            const uint8_t *pl = nullptr;
            const int n = kbdUnpackFrameEx(s_hostBuf, s_hostLen, &seq, &type, &pl);

            if (n > 0) {
                s_realFrames++;

                // ★ GHOST: 丢弃真实输入
                if (s_ghost) {
                    s_ghostDrop++;
                } else if (type == KBD_TYPE_REPORT) {
                    // 真实设备报文: 按角色解释成键盘或鼠标
                    if (g_deviceRole == 1) {
                        // 键盘: 标准 8 字节布局
                        s_realKbd.mod = pl[0];
                        if (n >= 8) memcpy(s_realKbd.keys, pl + 2, 6);
                    } else if (g_deviceRole == 2) {
                        // 鼠标: buttons + dx + dy + wheel
                        if (n >= 4) {
                            s_realMouse.buttons = pl[0] & 0x1F;
                            s_realMouse.dx += (int8_t)pl[1];
                            s_realMouse.dy += (int8_t)pl[2];
                            s_realMouse.wheel += (int8_t)pl[3];
                        }
                    }
                } else if (type == KBD_TYPE_ID && n >= 8 && pl[0] == 0x5A) {
                    // ★ 右板诊断帧 -> 打到 CH343
                    //
                    // 【为什么需要它】
                    //   右板只有一个 USB 口, 既当键盘 Host 又当接电脑的从口 ——
                    //   键盘插着的时候, 它的日志没有任何通道能出来。所以右板把状态
                    //   经 Serial1 送到这里, 由本板转到 CH343。这是键盘插着时
                    //   【唯一】能看到右板内部状态的途径。
                    //
                    // 注意: 这个分支【不改变任何按键状态】, 只打印。判据里的 0x5A
                    // 标记把"诊断帧"和开机时那个真实身份帧区分开(后者 pl[0] 是 VID 低字节,
                    // 但 VID=0x3554 的低字节是 0x54, 不是 0x5A, 不会撞上)。
                    Serial.printf("[KBD-RIGHT] kbd=%u mouse=%u iface=%u reports=%u frames=%u id=%u\n",
                                  (unsigned)pl[1], (unsigned)pl[2],
                                  (unsigned)(pl[3] == 0xFF ? 255 : pl[3]),
                                  (unsigned)pl[4], (unsigned)pl[5], (unsigned)pl[6]);
                }
            }
            s_hostLen = 0;
        } else if (s_hostLen > need) {
            s_hostLen = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// 周期发射: 合并真实 + 注入, 发一次快照
// ---------------------------------------------------------------------------
static uint32_t s_lastEmit = 0;

void makcuLinkTick(void)
{
    const uint32_t now = millis();

    // KEY_TAP 到期自动弹起
    if (s_tapUntil && (int32_t)(now - s_tapUntil) >= 0) {
        s_tapUntil = 0;
        s_tapState.clear();
    }

    // CLICK 到期自动松开
    extern uint32_t volatile g_clickReleaseAt;
    extern uint8_t  volatile g_clickBits;
    if (g_clickReleaseAt && (int32_t)(now - g_clickReleaseAt) >= 0) {
        s_injMouse.buttons &= (uint8_t)~g_clickBits;
        g_clickReleaseAt = 0;
        g_clickBits = 0;
    }

    if (now - s_lastEmit < REPORT_INTERVAL_MS) return;
    s_lastEmit = now;

    // ---- 定时屏蔽到期: 自动恢复真实输入 ----
    //
    // 这是解除屏蔽的【唯一保证路径】。上位机崩溃/断线时, 靠这里把真实输入还回来,
    // 否则设备会永久卡在"键鼠全无反应"的状态, 只能重新上电。
    if (s_maskUntilMs != 0 && (int32_t)(now - s_maskUntilMs) >= 0) {
        s_maskUntilMs = 0;
    }

    const bool dropReal = realInputDropped();

    // ---- 合并 ----
    KbdState kbd = s_injKbd;
    if (!dropReal) kbd.mergeFrom(s_realKbd);
    kbd.mergeFrom(s_tapState);          // KEY_TAP 也算注入

    MouseState mouse;
    mouse.buttons = s_injMouse.buttons;
    mouse.dx = s_injMouse.dx;
    mouse.dy = s_injMouse.dy;
    mouse.wheel = s_injMouse.wheel;
    if (!dropReal) {
        mouse.buttons |= s_realMouse.buttons;
        mouse.dx += s_realMouse.dx;
        mouse.dy += s_realMouse.dy;
        mouse.wheel += s_realMouse.wheel;
    }

    // ---- 发送 ----
    if (g_deviceRole == 1) {
        uint8_t rep[8];
        rep[0] = kbd.mod;
        rep[1] = 0;
        memcpy(rep + 2, kbd.keys, 6);
        kbdUsbSendReport(rep, 8);
    } else if (g_deviceRole == 2) {
        // 鼠标报文在 usb_hid.cpp 里按 RID 打包
        extern bool kbdUsbSendMouse(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel);
        kbdUsbSendMouse(mouse.buttons, mouse.dx, mouse.dy, mouse.wheel);
    }

    // 位移是相对量, 发完清零; 按键是绝对态, 保留
    s_injMouse.clearDelta();
    s_realMouse.clearDelta();
}

// ---------------------------------------------------------------------------
// 统计输出 (供主循环打印, 日志静音后不调用)
// ---------------------------------------------------------------------------
void makcuLinkStats(uint32_t *cmds, uint32_t *crcs, uint32_t *realFrames,
                    uint32_t *ghostDrops, bool *ghost)
{
    if (cmds)       *cmds = s_cmdCount;
    if (crcs)       *crcs = s_crcErr;
    if (realFrames) *realFrames = s_realFrames;
    if (ghostDrops) *ghostDrops = s_ghostDrop;
    if (ghost)      *ghost = s_ghost;
}
