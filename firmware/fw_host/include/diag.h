#pragma once

// 右板诊断固件入口（仅在 -DFW_DIAG=1 时生效）
void diagStart();

// 最小启动自检入口（仅在 -DFW_BOOTTEST=1 时生效）。
// 不初始化 USB / Serial1, 只用 LED9 闪灯证明"固件能跑起来"。
void bootTestStart();

// USB 分级诊断入口（仅在 -DFW_USBSTAGE=1 时生效）。
// 逐级初始化 USB, 每级结果用 LED9 脉冲个数编码, 无需串口即可定位卡点。
void usbStageStart();
