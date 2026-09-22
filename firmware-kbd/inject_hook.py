# -*- coding: utf-8 -*-
"""
在复用的 esp_usb_host.cpp 里注入"原始字节上报"钩子。

位置: _onReceive() 里拿到 endpoint_num 与 actual_num_bytes 之后。
原则: 只把【端点收到的原始字节】交出去, 不做任何解析。

这一步让右板变成纯粹的"字节搬运工":
  端点数据 -> kbdHostOnRawReport() -> 打包 -> Serial1 -> 左板
"""

import sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'C:\Users\Administrator\Desktop\KBD_PASSTHROUGH\fw_host_kbd\src\esp_usb_host.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

ANCHOR = """    uint8_t endpoint_num = transfer->bEndpointAddress & 0x0F;
    bool has_data = (transfer->actual_num_bytes > 0);
"""

INJECT = """    uint8_t endpoint_num = transfer->bEndpointAddress & 0x0F;
    bool has_data = (transfer->actual_num_bytes > 0);

    // ===== 原始字节透传 (新增) =====
    //
    // 【为什么不在这里解析】
    //   这是本方案与老代码最根本的区别。老代码会: 解析报告描述符 -> 判断是
    //   键盘还是鼠标 -> 按位域拆出修饰键/键码 -> 再重组成标准 8 字节发给左板。
    //   中间任何一步判断错(例如接口协议是 NONE 而不是 KEYBOARD, 或描述符用了
    //   不同的编码方式), 键盘就不工作 —— 这正是之前反复失败的根源。
    //
    //   现在直接把【端点收到的原始字节】交给左板, 由左板补一个 Report ID 后
    //   原样发给被控机。不解析就不可能解析错; 多媒体键、组合键、厂商自定义键
    //   也全都天然保留。
    //
    // 【透传边界】
    //   复制: 报告内容(键码字节)、报文长度、报文时序顺序
    //   不复制: 轮询间隔、刷新率 —— 那是传输层特性, 由左板自己决定。
    if (has_data) {
        kbdHostOnRawReport(transfer->data_buffer, transfer->actual_num_bytes);
    }
"""

assert ANCHOR in s, "anchor not found"
assert 'kbdHostOnRawReport' not in s, "already injected"

s = s.replace(ANCHOR, INJECT, 1)

# 声明外部钩子
DECL = """
// 原始报文钩子 (实现在 main.cpp): 右板只负责搬运字节, 不解析。
extern "C" void kbdHostOnRawReport(const uint8_t *data, int len);
"""
if 'kbdHostOnRawReport' not in s.split('void EspUsbHost::_onReceive')[0]:
    # 插到第一个 include 之后
    idx = s.find('\n', s.find('#include'))
    s = s[:idx+1] + DECL + s[idx+1:]

open(p, 'w', encoding='utf-8', newline='').write(s)
print("已注入 kbdHostOnRawReport 钩子")
print()
print("=== 验证 ===")
import re
print("  钩子声明:", 'extern "C" void kbdHostOnRawReport' in s)
print("  钩子调用:", s.count('kbdHostOnRawReport(') )
