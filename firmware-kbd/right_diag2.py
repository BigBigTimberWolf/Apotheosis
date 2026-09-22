# -*- coding: utf-8 -*-
"""在右板 DIAG 状态行之后追加 链路逐级计数 + 描述符解析结果"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_host\src\diag.cpp'
s = open(p, encoding='utf-8', errors='replace').read()

ANCHOR = "                (unsigned long)Serial1.available());"
assert ANCHOR in s, "状态行结尾未找到"
assert 'DIAG| CHAIN' not in s, "已经插入过了"

ADD = ANCHOR + '''

          // ===== 链路逐级计数 =====
          //   定位"卡在哪一级": 每个数都是上一级的输出、下一级的输入。
          //   reports -> mouseGate -> decoded -> fwd 任何一级为 0 就是卡点。
          extern uint32_t volatile g_rxReports;
          extern uint32_t volatile g_rxMouseGate;
          extern uint32_t volatile g_rxDecoded;
          extern uint32_t volatile g_rxFwd;
          extern uint32_t volatile g_rxKbdGate;
          diagLine("DIAG| CHAIN reports=%lu mouseGate=%lu decoded=%lu fwd=%lu kbd=%lu",
                   (unsigned long)g_rxReports,
                   (unsigned long)g_rxMouseGate,
                   (unsigned long)g_rxDecoded,
                   (unsigned long)g_rxFwd,
                   (unsigned long)g_rxKbdGate);

          // 描述符解析结果 (鼠标解码的 layoutOk 判定直接依赖这些值)
          diagLine("DIAG| DESC valid=%d bSz=%d bStart=%d xStart=%d xSz=%d yStart=%d ySz=%d whSz=%d rid=%d hasRid=%d",
                   (int)(usbHost.descPerIfaceValid[0] ? 1 : 0),
                   (int)usbHost.descPerIface[0].buttonSize,
                   (int)usbHost.descPerIface[0].buttonStartByte,
                   (int)usbHost.descPerIface[0].xAxisStartByte,
                   (int)usbHost.descPerIface[0].xAxisSize,
                   (int)usbHost.descPerIface[0].yAxisStartByte,
                   (int)usbHost.descPerIface[0].yAxisSize,
                   (int)usbHost.descPerIface[0].wheelSize,
                   (int)usbHost.descPerIface[0].reportId,
                   (int)(usbHost.descPerIface[0].hasReportId ? 1 : 0));'''

s = s.replace(ANCHOR, ADD, 1)
open(p, 'w', encoding='utf-8', newline='').write(s)
print("已插入 CHAIN + DESC 诊断行")
print()
print("验证:")
print("  CHAIN 行:", 'DIAG| CHAIN' in s)
print("  DESC 行 :", 'DIAG| DESC' in s)
print("  diagLine 写 Serial1:", 'Serial1.println(buf)' in s)
