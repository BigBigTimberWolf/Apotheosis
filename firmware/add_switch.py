# -*- coding: utf-8 -*-
"""
隔离测试: 加一个编译开关 FW_DUALHID (默认 0 = 关闭)

目的: 把"左板修复8(双HID)"这个变量单独关掉, 用来判断:
  - 若关闭后鼠标恢复 -> 问题在双HID改动
  - 若关闭后鼠标仍不动 -> 问题在右板(修复9没生效或别的)

这比继续猜更有效 —— 一次烧录就能定位。
"""

import sys
sys.stdout.reconfigure(encoding='utf-8')

# ---------- 1) dual_hid.cpp: 用宏包住整个实现 ----------
p = r'fw_device\src\dual_hid.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

if 'FW_DUALHID_ENABLE' not in s:
    # 在 include 之后插入宏保护
    anchor = '#include "esp32-hal-tinyusb.h"'
    assert anchor in s
    s = s.replace(anchor, anchor + '''

// 编译开关: 默认关闭。设为 1 才启用第二个 HID 接口。
// 用途: 出问题时可以一键回到"单接口"基线, 快速判断故障来源。
#ifndef FW_DUALHID_ENABLE
#define FW_DUALHID_ENABLE 0
#endif

#if !FW_DUALHID_ENABLE
// ===== 关闭态: 全部退化为空实现, 保证链接通过且不影响原有行为 =====

bool dualHidBegin(void) { return false; }
bool dualHidKeyboardReady(void) { return false; }
bool dualHidSendKeyboard(uint8_t, const uint8_t *) { return false; }
extern "C" uint16_t dualHidLoadDescriptor(uint8_t *, uint8_t *) { return 0; }

#else
''', 1)

    # 末尾补 #endif
    s = s.rstrip() + '\n\n#endif // FW_DUALHID_ENABLE\n'
    open(p, 'w', encoding='utf-8', newline='').write(s)
    print("[1] dual_hid.cpp 已加 FW_DUALHID_ENABLE 开关(默认0=关闭)")
else:
    print("[1] 开关已存在")

# ---------- 2) platformio.ini: 加可选的启用开关 ----------
p2 = r'fw_device\platformio.ini'
s2 = open(p2, encoding='utf-8', errors='replace').read()

if 'FW_DUALHID_ENABLE' not in s2:
    s2 = s2.replace(
        '  -Wl,--wrap=tud_hid_descriptor_report_cb',
        '''  ; 双HID接口开关: 0=关闭(回到单接口基线), 1=启用第二个HID接口。
  ; 出问题时先设 0 确认基线, 再逐步开启定位。
  -DFW_DUALHID_ENABLE=0
  -Wl,--wrap=tud_hid_descriptor_report_cb''', 1)
    open(p2, 'w', encoding='utf-8', newline='').write(s2)
    print("[2] platformio.ini 已加 -DFW_DUALHID_ENABLE=0")
else:
    print("[2] 开关已存在")
