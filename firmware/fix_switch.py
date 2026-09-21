# -*- coding: utf-8 -*-
"""
修正: wrap 函数必须【无条件存在】(因为链接器标志始终存在),
      但它在关闭态下只做转发。
"""

import sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'fw_device\src\dual_hid.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

# 关闭态的空实现里【去掉】dualHidLoadDescriptor(它不属于 wrap),
# 并补上真实的 wrap 转发函数。
OLD = """#if !FW_DUALHID_ENABLE
// ===== 关闭态: 全部退化为空实现, 保证链接通过且不影响原有行为 =====

bool dualHidBegin(void) { return false; }
bool dualHidKeyboardReady(void) { return false; }
bool dualHidSendKeyboard(uint8_t, const uint8_t *) { return false; }
extern "C" uint16_t dualHidLoadDescriptor(uint8_t *, uint8_t *) { return 0; }

#else
"""

NEW = """#if !FW_DUALHID_ENABLE
// ===== 关闭态: 功能全部退化为空实现 =====
//
// 注意: __wrap_tud_hid_descriptor_report_cb 必须【无条件存在】——
// 因为 -Wl,--wrap=... 链接器标志始终在 build_flags 里, 只要 TinyUSB 引用了
// 该符号, 链接器就要求 __wrap_ 版本必须存在(否则 undefined reference)。
// 所以这里保留它, 只是纯转发给 Arduino 原实现。

bool dualHidBegin(void) { return false; }
bool dualHidKeyboardReady(void) { return false; }
bool dualHidSendKeyboard(uint8_t, const uint8_t *) { return false; }

extern "C" uint8_t const *__real_tud_hid_descriptor_report_cb(uint8_t instance);

extern "C" uint8_t const *__wrap_tud_hid_descriptor_report_cb(uint8_t instance)
{
    // 关闭态: 一律交回 Arduino 原实现, 行为与未加 wrap 时完全一致。
    return __real_tud_hid_descriptor_report_cb(instance);
}

#else
"""

assert OLD in s, "disabled block not found"
s = s.replace(OLD, NEW, 1)

# 关闭态不该再有 dualHidLoadDescriptor 的外部声明问题;
# 启用态里的 dualHidLoadDescriptor 保持原样(在 #else 之后)
open(p, 'w', encoding='utf-8', newline='').write(s)
print("wrap 函数已改为无条件存在(关闭态纯转发)")
