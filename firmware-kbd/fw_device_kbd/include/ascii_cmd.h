// ============================================================================
// ascii_cmd.h / ascii_cmd.cpp —— ASCII 文本命令通道 (CH343)
//
// 【为什么需要这个】
//   Apotheosis 的 MakcuNewConnection 用【两条通道】跟固件说话:
//
//     通道1: 二进制帧 (makcu_proto.h)  -> 位移/按键/滚轮等高频数据
//     通道2: ASCII 文本行 (本文件)      -> 握手/屏蔽等低频控制
//
//   我最初只实现了通道1, 结果上位机【连不上】—— 因为握手走的是通道2。
//
// 【握手流程 (从 MakcuNew.cpp 读到的确切行为)】
//
//   上位机: 打开串口 @115200 (kBootBaud)
//           发 "km.version\r\n"
//           在 500ms 内等一行包含 "MAKCU-PASSTHROUGH" 的应答
//   固件:   必须回一行含该字样的文本, 否则上位机判定连接失败并中止
//
//   ★ 这是整个链路的【第一道门】, 不回就什么都做不了。
//
// 【mask 屏蔽 (注释原文)】
//   "走 ASCII 文本通道, 不新增二进制帧"
//   "固件 fw_device 收到 km.mask(N) 后经板间 UART 转发给 fw_host,
//    由 fw_host 在其 mask 窗口内丢弃真实键鼠输入"
//   "固件对这条命令只回一行 km.mask(N) ok 到板间链路, 不会回到本串口"
//
//   => 屏蔽【在右板执行】, 左板只负责转发
//   => N 钳制上限 2000ms (kMaskMaxMs)
// ============================================================================

#ifndef ASCII_CMD_H
#define ASCII_CMD_H

#include <stdint.h>

// 版本标识 —— 上位机的 probeAscii 就是在等这个字符串
#define MAKCU_VERSION_TAG   "MAKCU-PASSTHROUGH"

// mask 窗口上限 (与上位机 kMaskMaxMs 一致)
#define MAKCU_MASK_MAX_MS   2000

// 喂入一个字节 (来自 CH343)
//
// ★ 必须只在【二进制帧解析器空闲】时调用 —— 见 ascii_cmd.cpp 的说明。
//   这个约束由 makcu_link.cpp 的 makcuLinkFeedPc() 保证。
void asciiCmdFeed(uint8_t b);

// 是否已完成握手 (收到过 km.version)
bool asciiCmdHandshaked(void);

// 是否收到过上位机的可打印字节。
//   用途: 上位机一旦开口, 就永久静音本板的文本日志 —— 日志虽然也是纯 ASCII、
//   不会破坏握手, 但会混进上位机的文本通道里造成干扰。
bool asciiCmdActive(void);

#endif // ASCII_CMD_H
