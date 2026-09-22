# -*- coding: utf-8 -*-
"""左板: 0x23 帧改为「原样字节直发」"""
import sys
sys.stdout.reconfigure(encoding='utf-8')
root = r'C:\Users\Administrator\Desktop\MAKCUNEW_firmware_source'

# ===========================================================================
# 1) handleCommands.cpp: 新增 kbdApplyRealRaw
# ===========================================================================
p1 = root + r'\fw_device\src\handleCommands.cpp'
s1 = open(p1, encoding='utf-8', errors='replace').read()

ANCHOR = "static void kbdApplyRealReport(uint8_t mod, const uint8_t keys6[6]) {"
assert ANCHOR in s1, "锚点未找到"

NEW_FN = '''// ============================================================================
// 键盘原样转发入口 (与独立版架构一致)
//
// 右板把端点收到的【原始字节】透传过来, 这里【原样发给 USB】——
// 不解析、不合并、不查表、不去重。用户实测这套最流畅。
//
// raw[0] = 修饰键, raw[1] = 保留, raw[2..7] = 6 个键码
// (标准 boot keyboard 布局, 与真键盘直插时主机看到的一致)
//
// 【km.mask 屏蔽仍然有效】
//   屏蔽在【右板】做: fw_host 的 onKeyboard 在屏蔽窗口内直接不转发真实键盘
//   变化。所以这里收不到报文就等于被屏蔽了, 与转发方式无关。
// ============================================================================
static void kbdApplyRealRaw(const uint8_t *raw, uint8_t len)
{
    if (!raw || len < 8) return;

    if (kbdUsbReady()) {
        if (kbdUsbSendKeyboard(raw)) g_diagEmitOk++;
        else                         g_diagEmitFail++;
    } else {
        g_diagEmitFail++;
    }
}

''' + ANCHOR

s1 = s1.replace(ANCHOR, NEW_FN, 1)
open(p1, 'w', encoding='utf-8', newline='').write(s1)
print("[1] 左板: 已加 kbdApplyRealRaw")

# ===========================================================================
# 2) proto_parser.cpp: 0x23 帧按长度分派
# ===========================================================================
p2 = root + r'\fw_device\src\proto_parser.cpp'
s2 = open(p2, encoding='utf-8', errors='replace').read()

OLD2 = """    case 0x23: {
        uint8_t keys[6] = {0, 0, 0, 0, 0, 0};
        if (len >= 1) {
            const uint8_t n = (uint8_t)((len - 1) < 6 ? (len - 1) : 6);
            for (uint8_t i = 0; i < n; ++i) keys[i] = pl[1 + i];
        }"""
NEW2 = """    case 0x23: {
        // ★ 新版: payload = [iface][raw0..rawN-1]  —— 原样转发
        //   判定: 去掉 iface 后仍有 >= 8 字节, 且首字节是接口号(0..7)
        if (len >= 9 && pl[0] <= 7) {
            if (onKbRaw) onKbRaw(pl + 1, (uint8_t)(len - 1));
            break;
        }

        // 旧版兼容: payload = [mod][key0..key5]
        uint8_t keys[6] = {0, 0, 0, 0, 0, 0};
        if (len >= 1) {
            const uint8_t n = (uint8_t)((len - 1) < 6 ? (len - 1) : 6);
            for (uint8_t i = 0; i < n; ++i) keys[i] = pl[1 + i];
        }"""
assert OLD2 in s2, "0x23 分支未找到"
s2 = s2.replace(OLD2, NEW2, 1)

# 加回调声明
if 'onKbRaw' not in s2.split('case 0x23')[0]:
    import re
    m = re.search(r'(\s*)(std::function<void\(uint8_t, const uint8_t \*\)> onKbReport;)', s2)
    if m:
        s2 = s2[:m.end(1)] + m.group(2) + "\n" + m.group(1) + "std::function<void(const uint8_t *, uint8_t)> onKbRaw;" + s2[m.end(2):]
        print("[2a] 已加 onKbRaw 声明 (std::function 形式)")
    else:
        # 找 onKbReport 的声明方式
        r = re.search(r'^.*onKbReport.*$', s2, re.M)
        print("  onKbReport 声明形式:", r.group(0).strip() if r else "未找到")

open(p2, 'w', encoding='utf-8', newline='').write(s2)
print("[2] proto_parser: 0x23 已支持原样字节分派")
