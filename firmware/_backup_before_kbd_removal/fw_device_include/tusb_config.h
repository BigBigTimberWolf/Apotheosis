#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

// Common configuration
#define CFG_TUSB_MCU                OPT_MCU_ESP32S3
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

// Device configuration
// 注意: 原名 CFG_TUD_ENDOINT0_SIZE 拼写错误(ENDOINT), 从未生效 —— 保留原名会让人
// 以为端点0缓冲被配成了 64, 实际一直是 TinyUSB 默认值。此处更正为规范拼写。
#define CFG_TUD_ENDPOINT0_SIZE      64

// Enable class support
#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 1   // 单接口(键盘或鼠标, 按角色二选一)
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0
#define CFG_TUD_DFU                 0

// String descriptors
#define CFG_TUD_DESC_STRING         1

#endif // _TUSB_CONFIG_H_
