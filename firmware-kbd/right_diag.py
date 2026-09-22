# -*- coding: utf-8 -*-
"""
右板: 加入完整链路计数器 + deviceMouseReady 健壮修复

【为什么】
  右板此前完全是黑盒。这次把链路上每个关卡都计数, 并在 DIAG 行里报出来,
  这样【一次烧录】就能定位到底卡在哪一级:

    设备报文总数 -> 通过鼠标门控 -> 通过布局校验 -> 实际转发

  任何一级为 0, 就说明卡在那里。

【同时修 deviceMouseReady】
  它是所有下行数据(鼠标+键盘)的总闸, 原来【只】由左板发来的 "USB_INIT"
  命令置位。一旦命令时序错过、或之后设备重枚举触发 DEV_GONE, 这个闸就永久
  关着 —— 表现为"两块板都活着、链路通, 但一个字节的数据都不发"。
  改成由【端点提交成功】驱动, 不再依赖命令时机。
"""
import sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source\fw_host\src\esp_usb_host.cpp'
s = open(p, encoding='utf-8', errors='replace').read()
n0 = len(s)

# ===========================================================================
# 1) 计数器定义
# ===========================================================================
DECL = '''
// ===== 链路计数器 (诊断用; 由 diag.cpp 的 DIAG 行报出) =====
//   每一级都计数, 这样任何一级为 0 就能直接定位卡点。
uint32_t volatile g_rxReports   = 0;   // 端点收到的报文总数
uint32_t volatile g_rxMouseGate = 0;   // 通过"鼠标端点"门控的报文数
uint32_t volatile g_rxDecoded   = 0;   // 通过 reportId/layout 校验并解码成功的
uint32_t volatile g_rxFwd       = 0;   // 实际转发给左板的位移
uint32_t volatile g_rxKbdGate   = 0;   // 通过键盘门控的报文数
uint32_t volatile g_rxNotMouse  = 0;   // 既非鼠标端点也非键盘端点的报文
'''
if 'g_rxReports' not in s:
    idx = s.find('\n', s.find('#include'))
    s = s[:idx+1] + DECL + s[idx+1:]

# ===========================================================================
# 2) 在最外层入口计数: 有数据的每一帧
# ===========================================================================
OLD1 = """    // 鼠标报文解码(精确布局优先, 不可用时用标准 8 位兜底布局)。
    if (has_data && (epIsSelectedMouse || epIsFallbackMouse))
    {"""
NEW1 = """    if (has_data) g_rxReports++;

    // 鼠标报文解码(精确布局优先, 不可用时用标准 8 位兜底布局)。
    if (has_data && (epIsSelectedMouse || epIsFallbackMouse))
    {
        g_rxMouseGate++;"""
assert OLD1 in s, "鼠标门控块未找到"
s = s.replace(OLD1, NEW1, 1)

# ===========================================================================
# 3) 布局校验通过处计数
# ===========================================================================
OLD2 = """        if (reportIdOk && layoutOk)
        {
            hid_mouse_report_t report = {};"""
NEW2 = """        if (reportIdOk && layoutOk)
        {
            g_rxDecoded++;
            hid_mouse_report_t report = {};"""
assert OLD2 in s, "布局校验块未找到"
s = s.replace(OLD2, NEW2, 1)

# ===========================================================================
# 4) 实际转发处计数
# ===========================================================================
OLD3 = """            if (report.x != 0 || report.y != 0 || report.wheel != 0)
            {
                usbHost->onMouseMove(report);
            }"""
NEW3 = """            if (report.x != 0 || report.y != 0 || report.wheel != 0)
            {
                g_rxFwd++;
                usbHost->onMouseMove(report);
            }"""
assert OLD3 in s, "转发块未找到"
s = s.replace(OLD3, NEW3, 1)

# ===========================================================================
# 5) deviceMouseReady 健壮修复: 端点提交成功后即置位
# ===========================================================================
OLD4 = """                err = usb_host_transfer_submit(this->usbTransfer[this->usbTransferSize - 1]);
                if (err != ESP_OK)
                {
                    ESP_LOGE("EspUsbHost", "usb_host_transfer_submit() failed with err=%x", err);
                }"""
NEW4 = """                err = usb_host_transfer_submit(this->usbTransfer[this->usbTransferSize - 1]);
                if (err != ESP_OK)
                {
                    ESP_LOGE("EspUsbHost", "usb_host_transfer_submit() failed with err=%x", err);
                }
                else
                {
                    // ★ deviceMouseReady 是所有下行数据(鼠标 + 键盘)的总闸。
                    //
                    // 【原来只由 "USB_INIT" 命令置位】一旦命令时序错过, 或之后
                    // 设备重枚举触发 DEV_GONE 把它清零, 这个闸就永久关着 ——
                    // 现象是"两块板都活着、板间链路通, 但一个字节数据都不发"。
                    //
                    // 端点已成功提交 => 本板确实在读取该设备 => 就绪。
                    // 这样它反映【实际状态】而不是命令时机, 断线时 DEV_GONE 仍会清零。
                    deviceMouseReady = true;
                }"""
assert OLD4 in s, "端点提交块未找到"
s = s.replace(OLD4, NEW4, 1)

open(p, 'w', encoding='utf-8', newline='').write(s)
print(f"esp_usb_host.cpp: {n0} -> {len(s)} 字节")
print()
print("验证:")
for name, pat in [("计数器定义", "g_rxReports   = 0"),
                  ("报文总数计数", "if (has_data) g_rxReports++"),
                  ("鼠标门控计数", "g_rxMouseGate++"),
                  ("解码成功计数", "g_rxDecoded++"),
                  ("转发计数", "g_rxFwd++"),
                  ("deviceMouseReady 修复", "deviceMouseReady = true;")]:
    print(f"  {name:22}: {'OK' if pat in s else '*** 缺失 ***'}")
