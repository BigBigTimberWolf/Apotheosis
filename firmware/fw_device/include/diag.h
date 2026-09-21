#pragma once

#include <stdint.h>

// 诊断固件入口（仅在 -DFW_DIAG=1 时生效）
void diagStart();

// ★ 下面这些计数器【不再用 FW_DIAG 守卫】。
//   它们是排查"右板到底有没有发数据"的关键指标(Serial1 原始字节数),
//   每次调试都需要看, 而开销只是几个自增, 可以忽略。
extern volatile uint32_t g_diag_rx1_bytes;  // Serial1 收到的字节总数
extern volatile uint32_t g_diag_rx1_lines;  // Serial1 收到的完整行数
extern volatile uint32_t g_diag_max_line;   // Serial1 收到的最长一行(字节)
extern volatile uint32_t g_diag_snap_len;   // Serial1 前若干字节的快照长度
extern uint8_t g_diag_snap[];               // 快照本体（用于看右板说了什么）
extern volatile uint32_t g_diag_hbt_count;  // 右板 "#HBT" 心跳行数（判定板间链路是否通）
