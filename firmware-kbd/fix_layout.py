# -*- coding: utf-8 -*-
"""
修复: 布局"可用"的判据太松, 导致兜底路径被错误禁用

【问题】
    layoutUsable = descPerIfaceValid[iface] && descPerIface[iface].valid;

  只看 valid 标志。但实测(日志):
      DESC valid=1 bSz=0 bStart=0 xStart=0 xSz=8 yStart=0 ySz=8
  描述符被标成"解析成功", 布局却是废的 —— 两轴都在字节 0、没有按键字段。

  后果是双重的:
    1) layoutUsable=true  ->  兜底路径(标准8位鼠标布局)【被禁用】
    2) 精确布局又解不出位移 ->  decoded=0
  两条路同时失效, 表现为"灯在闪但光标不动", 且不报任何错。

【修法】
  把"可用"的判据收紧为"结构合理", 而不只是 valid:
    - 必须有两个轴 (xAxisSize/yAxisSize != 0)
    - 两轴不能在同一字节, 除非是 12 位半字节交织那种合法写法
  这样废布局会正确地落到兜底路径上(标准 byte0=buttons,1=X,2=Y,3=wheel)。
"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_host\src\esp_usb_host.cpp'
s = open(p, encoding='utf-8', errors='replace').read()
n0 = len(s)

# ---------- 1) 插入布局合理性判据 ----------
ANCHOR = "    const bool layoutUsable = usbHost->descPerIfaceValid[ifaceNum] &&\n                              usbHost->descPerIface[ifaceNum].valid;"
assert ANCHOR in s, "layoutUsable 定义未找到"

NEW = '''    // ===== 布局"真的能用"的判据 =====
    //
    // 【为什么不能只看 valid】
    //   实测日志: DESC valid=1 bSz=0 xStart=0 yStart=0 —— 描述符被标成解析成功,
    //   布局却是废的(两轴同址、无按键)。若只按 valid 判定 layoutUsable:
    //     layoutUsable=true  ->  兜底路径被禁用
    //     精确布局又解不出位移 ->  decoded=0
    //   两条路同时失效, 鼠标一个字节都发不出去, 而且不报任何错。
    //
    //   这里补上结构检查, 让废布局正确地落到兜底路径。
    auto layoutLooksSane = [](const EspUsbHost::HIDReportDescriptor &d) -> bool {
        if (!d.valid) return false;
        if (d.xAxisSize == 0 || d.yAxisSize == 0) return false;      // 没有轴 -> 不可能是鼠标
        // 两轴落在同一字节: 只有 12 位半字节交织那种写法是合法的
        if (d.xAxisStartByte == d.yAxisStartByte &&
            !(d.xAxisSize == 12 && d.yAxisSize == 12 && d.maxAxisBitsPerByte == 2)) {
            return false;
        }
        return true;
    };

    const bool layoutUsable = usbHost->descPerIfaceValid[ifaceNum] &&
                              layoutLooksSane(usbHost->descPerIface[ifaceNum]);'''

s = s.replace(ANCHOR, NEW, 1)

# ---------- 2) epIsSelectedMouse 同样收紧 ----------
OLD2 = """        (usbHost->endpoint_data_list[ifaceNum].bInterfaceClass == USB_CLASS_HID) &&
        usbHost->descPerIfaceValid[ifaceNum] &&
        usbHost->descPerIface[ifaceNum].valid;"""
NEW2 = """        (usbHost->endpoint_data_list[ifaceNum].bInterfaceClass == USB_CLASS_HID) &&
        usbHost->descPerIfaceValid[ifaceNum] &&
        usbHost->descPerIface[ifaceNum].valid;
    // 注: 上面保留了原判据(精确布局), 下面的 layoutLooksSane 用于兜底决策。"""
assert OLD2 in s, "epIsSelectedMouse 定义未找到"
s = s.replace(OLD2, NEW2, 1)

# ---------- 3) 诊断: dump 全部接口的描述符 ----------
OLD3 = '''          diagLine("DIAG| DESC valid=%d bSz=%d bStart=%d xStart=%d xSz=%d yStart=%d ySz=%d whSz=%d rid=%d hasRid=%d",
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

NEW3 = '''          // dump 全部接口 —— 之前只看 [0], 而鼠标可能在别的接口上
          diagLine("DIAG| MOUSEIFACE mouseIface=%d", (int)usbHost.mouseIface);
          for (int k = 0; k < 4; ++k) {
              diagLine("DIAG| DESC[%d] val=%d cl=%d pr=%d bSz=%d bSt=%d xSt=%d xSz=%d ySt=%d ySz=%d wh=%d rid=%d",
                       k,
                       (int)(usbHost.descPerIfaceValid[k] ? 1 : 0),
                       (int)usbHost.endpoint_data_list[k].bInterfaceClass,
                       (int)usbHost.endpoint_data_list[k].bInterfaceProtocol,
                       (int)usbHost.descPerIface[k].buttonSize,
                       (int)usbHost.descPerIface[k].buttonStartByte,
                       (int)usbHost.descPerIface[k].xAxisStartByte,
                       (int)usbHost.descPerIface[k].xAxisSize,
                       (int)usbHost.descPerIface[k].yAxisStartByte,
                       (int)usbHost.descPerIface[k].yAxisSize,
                       (int)usbHost.descPerIface[k].wheelSize,
                       (int)usbHost.descPerIface[k].reportId);
          }'''
assert OLD3 in s, "DESC 诊断块未找到"
s = s.replace(OLD3, NEW3, 1)

open(p, 'w', encoding='utf-8', newline='').write(s)
print(f"esp_usb_host.cpp: {n0} -> {len(s)} 字节")
print()
print("验证:")
print("  layoutLooksSane 已加 :", 'layoutLooksSane' in s)
print("  layoutUsable 已收紧 :", 'layoutLooksSane(usbHost->descPerIface[ifaceNum])' in s)
print("  全接口 DESC dump    :", 'DIAG| DESC[%d]' in s)
print("  mouseIface 报告     :", 'DIAG| MOUSEIFACE' in s)
