// ============================================================================
// ClonedHID.cpp - 从真设备 HID 报告描述符解析出报文格式
//
// 与 fw_host 的 parseHIDReportDescriptor() 是同一套逻辑, 但目的相反:
//   fw_host  : 解析出来是为了【解码】真设备发来的报文
//   本文件    : 解析出来是为了【编码】我们要发给被控机的报文
//
// 两边必须对同一份描述符得出相同结论, 否则会出现"主机解得对、设备发得错"。
// ============================================================================
#include "ClonedHID.h"

namespace cloned {

// HID 短项读取(与 fw_host 侧实现保持一致)
struct Item {
    uint8_t item;
    uint32_t value;
    uint8_t size;
};

static bool readItem(const uint8_t *d, int n, int &i, Item &out) {
    if (i >= n) return false;
    const uint8_t prefix = d[i];
    if (prefix == 0xFE) {                 // 长项: 本实现不处理, 跳过
        if (i + 2 >= n) return false;
        const uint8_t sz = d[i + 1];
        i += 2 + sz;
        return false;
    }
    const uint8_t size = prefix & 0x03;
    const uint8_t type = (prefix >> 2) & 0x03;
    const uint8_t tag  = (prefix >> 4) & 0x0F;
    if (i + 1 + size > n) return false;

    uint32_t v = 0;
    for (uint8_t b = 0; b < size; ++b) {
        v |= ((uint32_t)d[i + 1 + b]) << (8 * b);
    }
    if (size == 4) v = (uint32_t)(int32_t)v;

    out.item = (uint8_t)((type << 4) | tag);   // 与 fw_host 的编码一致
    out.value = v;
    out.size = size;
    i += 1 + size;
    return true;
}

ClonedReportLayout parse(uint8_t *data, int length) {
    ClonedReportLayout L;

    uint32_t bitOffset = 0;
    uint32_t reportSize = 0;
    uint32_t reportCount = 0;
    uint8_t  usagePage = 0;
    uint8_t  usage = 0;
    uint8_t  usageMinimum = 0;

    int maxByte = 0;

    int i = 0;
    while (i < length) {
        Item it;
        if (!readItem(data, length, i, it)) {
            // readItem 返回 false 可能是长项(已前进)或越界(停止)
            if (i >= length) break;
            continue;
        }

        switch (it.item) {
        case 0x04: usagePage = (uint8_t)it.value; break;
        case 0x08: usage = (uint8_t)it.value; break;
        case 0x18: usageMinimum = (uint8_t)it.value; break;
        case 0x28: break;
        case 0x84:
            L.reportId = (uint8_t)it.value;
            L.hasReportId = true;
            bitOffset += 8;
            break;
        case 0x74: reportSize = it.value; break;
        case 0x94: reportCount = it.value; break;

        case 0x80:   // INPUT
        {
            const uint32_t bits = reportSize * reportCount;

            // 按钮 / 键盘修饰键
            if (usagePage == 0x09 && !L.hasButtons &&
                ((usage >= 1 && usage <= 16) || (usageMinimum >= 1 && usageMinimum <= 16))) {
                L.buttonByte = (uint8_t)(bitOffset / 8);
                L.hasButtons = true;
                if (bitOffset + bits > (uint32_t)maxByte * 8) {
                    maxByte = (int)((bitOffset + bits + 7) / 8);
                }
                bitOffset += bits;
                break;
            }

            // 键盘修饰键(Usage Page 0x07, 0xE0..0xE7, 1-bit x8)
            if (usagePage == 0x07 && !L.hasModifier && reportSize == 1 &&
                ((usage >= 0xE0 && usage <= 0xE7) ||
                 (usageMinimum >= 0xE0 && usageMinimum <= 0xE7))) {
                L.modifierByte = (uint8_t)(bitOffset / 8);
                L.hasModifier = true;
                L.isKeyboard = true;
                bitOffset += bits;
                break;
            }

            // 键盘键码数组
            if (usagePage == 0x07 && reportSize == 8 && !L.keyArrayCount &&
                (usageMinimum < 0xE0 || usageMinimum == 0)) {
                L.keyArrayByte = (uint8_t)(bitOffset / 8);
                L.keyArrayCount = (uint8_t)(reportCount > 32 ? 32 : reportCount);
                L.isKeyboard = true;
                bitOffset += bits;
                break;
            }

            // X / Y 轴
            if (usagePage == 0x01 && (usage == 0x30 || usage == 0x31)) {
                const uint32_t perAxisBits = reportSize ? reportSize : 8;
                if (usage == 0x30 && !L.hasX) {
                    L.xByte = (uint8_t)(bitOffset / 8);
                    L.xBits = (uint8_t)perAxisBits;
                    L.hasX = true;
                } else if (usage == 0x31 && !L.hasY) {
                    L.yByte = (uint8_t)(bitOffset / 8);
                    L.yBits = (uint8_t)perAxisBits;
                    L.hasY = true;
                }
                // 两轴挤在一个 INPUT 项里(N=1, 各 4 位/12 位交织)
                const uint32_t axisCount = reportCount ? reportCount : 1;
                if (perAxisBits < 8 && axisCount >= 2) L.interleaved = true;
                bitOffset += perAxisBits * axisCount;
                break;
            }

            // 滚轮
            if (usagePage == 0x01 && usage == 0x38 && !L.hasWheel) {
                L.wheelByte = (uint8_t)(bitOffset / 8);
                L.hasWheel = true;
                bitOffset += bits;
                break;
            }

            bitOffset += bits;
            break;
        }

        default:
            break;
        }

        const int bytesSoFar = (int)((bitOffset + 7) / 8);
        if (bytesSoFar > maxByte) maxByte = bytesSoFar;
    }

    // Report ID 会把所有偏移后移 1(报文首字节是 ID)
    if (L.hasReportId) {
        L.buttonByte++;
        L.xByte++;
        L.yByte++;
        L.wheelByte++;
        L.modifierByte++;
        L.keyArrayByte++;
        maxByte++;
    }

    // 报文总长
    uint8_t len = (uint8_t)(maxByte > 255 ? 255 : maxByte);

    // 键盘: 至少要能装下键码数组
    if (L.isKeyboard) {
        const int need = L.keyArrayByte + L.keyArrayCount;
        if (need > len) len = (uint8_t)(need > 255 ? 255 : need);
    }
    // 鼠标: 至少要能装下滚轮
    if (L.hasWheel) {
        const int need = L.wheelByte + 1;
        if (need > len) len = (uint8_t)need;
    }
    if (len == 0) len = 8;   // 解析不出东西时的保底长度

    L.reportLen = len;
    L.valid = (L.hasX && L.hasY) || L.isKeyboard;
    return L;
}

// ---------------------------------------------------------------------------
// 按 layout 组装鼠标报文
//
// dx/dy/wheel 是相对位移, buttons 是按键位图(bit0=左, bit1=右, bit2=中,
// bit3=后退, bit4=前进 —— 与 USB HID Button Page 的编号一致)。
// 真设备描述符里按钮个数可能不足 5, 多余的位会被自然丢弃(写进 buttonByte
// 时只占低位, 主机按它声明的位数读)。
// ---------------------------------------------------------------------------
void buildMouseReport(const ClonedReportLayout &L, uint8_t *out, uint8_t outLen,
                      int dx, int dy, int wheel, uint8_t buttons)
{
    if (!out || outLen == 0) return;
    memset(out, 0, outLen);

    uint8_t off = 0;
    if (L.hasReportId && outLen > 0) {
        out[0] = L.reportId;
        off = 1;
    }

    if (L.hasButtons && (uint8_t)(L.buttonByte) < outLen) {
        out[L.buttonByte] = buttons & 0x1F;
    }

    if (L.interleaved && L.hasX && L.hasY) {
        // 12 位半字节交织: X = b0 | (b1 & 0x0F) << 8, Y = (b1 >> 4) | b2 << 4
        //
        // 注意: 这里的位移是 12 位有符号, 范围 -2048..2047。设备侧注入的位移
        // 已经是拆分过的小步长(见 handleMove), 不会超范围; 超了也只会被截断,
        // 补码语义下与真设备行为一致。
        const uint16_t xv = (uint16_t)(int16_t)dx & 0x0FFF;
        const uint16_t yv = (uint16_t)(int16_t)dy & 0x0FFF;
        if ((int)L.xByte + 2 < outLen) {
            out[L.xByte]     = (uint8_t)(xv & 0xFF);
            out[L.xByte + 1] = (uint8_t)(((xv >> 8) & 0x0F) | ((yv & 0x0F) << 4));
            out[L.xByte + 2] = (uint8_t)((yv >> 4) & 0xFF);
        }
    } else {
        if (L.hasX) {
            if (L.xBits <= 8) {
                if ((uint8_t)L.xByte < outLen) out[L.xByte] = (uint8_t)(int8_t)dx;
            } else {
                const uint16_t v = (uint16_t)(int16_t)dx;
                if ((int)L.xByte + 1 < outLen) {
                    out[L.xByte]     = (uint8_t)(v & 0xFF);
                    out[L.xByte + 1] = (uint8_t)((v >> 8) & 0xFF);
                }
            }
        }
        if (L.hasY) {
            if (L.yBits <= 8) {
                if ((uint8_t)L.yByte < outLen) out[L.yByte] = (uint8_t)(int8_t)dy;
            } else {
                const uint16_t v = (uint16_t)(int16_t)dy;
                if ((int)L.yByte + 1 < outLen) {
                    out[L.yByte]     = (uint8_t)(v & 0xFF);
                    out[L.yByte + 1] = (uint8_t)((v >> 8) & 0xFF);
                }
            }
        }
    }

    if (L.hasWheel && (uint8_t)L.wheelByte < outLen) {
        out[L.wheelByte] = (uint8_t)(int8_t)wheel;
    }

    (void)off;
}

// ---------------------------------------------------------------------------
// 按 layout 组装键盘报文
//
// 注意: 真设备的键盘描述符里键码数组可能不是 6 个(NKRO 键盘是位图形式,
// 那种格式本函数无法表达 —— 位图键盘的"Usage Min/Max 1..N, REPORT_SIZE=1"
// 会被解析成 hasModifier 分支而 keyArrayCount 为 0, 此时 valid=false,
// 上层应回落到内置 USBHIDKeyboard)。
// ---------------------------------------------------------------------------
void buildKeyboardReport(const ClonedReportLayout &L, uint8_t *out, uint8_t outLen,
                         uint8_t modifiers, const uint8_t *keys)
{
    if (!out || outLen == 0) return;
    memset(out, 0, outLen);

    if (L.hasReportId && outLen > 0) out[0] = L.reportId;

    if (L.hasModifier && (uint8_t)L.modifierByte < outLen) {
        out[L.modifierByte] = modifiers;
    }
    if (L.keyArrayCount && keys) {
        uint8_t n = L.keyArrayCount;
        if ((int)L.keyArrayByte + n > outLen) {
            n = (uint8_t)(outLen > L.keyArrayByte ? outLen - L.keyArrayByte : 0);
        }
        for (uint8_t i = 0; i < n; ++i) {
            out[L.keyArrayByte + i] = keys[i];
        }
    }
}

} // namespace cloned
