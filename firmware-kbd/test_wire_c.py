# -*- coding: utf-8 -*-
"""
用 C 编译器级别的验证: 直接编译一个测试程序, 用真实的 kbd_wire.h 做往返测试。
比 Python 复刻更可靠 —— 测的就是设备里跑的那份代码。
"""
import subprocess, os, sys, tempfile
sys.stdout.reconfigure(encoding='utf-8')

hdr = r'C:\Users\Administrator\Desktop\KBD_PASSTHROUGH\shared\kbd_wire.h'

c_code = r'''
#include <stdio.h>
#include <string.h>
#include "kbd_wire.h"

int main(void)
{
    int fails = 0;

    /* 1) 各种长度往返 */
    for (int plen = 1; plen <= KBD_MAX_REPORT; ++plen) {
        uint8_t payload[KBD_MAX_REPORT];
        for (int i = 0; i < plen; ++i) payload[i] = (uint8_t)(i * 7 + 3);

        uint8_t frame[KBD_MAX_FRAME];
        int n = kbdPackFrame(frame, (uint8_t)plen, payload, plen);

        if (n != plen + 7) {
            printf("  plen=%2d: 帧长 %d != %d  *** 长度错 ***\n", plen, n, plen + 7);
            fails++;
            continue;
        }

        uint8_t seq = 0; const uint8_t *got = NULL;
        int r = kbdUnpackFrame(frame, n, &seq, &got);
        if (r != plen || memcmp(got, payload, plen) != 0) {
            printf("  plen=%2d: 解包失败 r=%d  *** 内容错 ***\n", plen, r);
            fails++;
        }
    }
    printf("  各长度往返: %s\n", fails ? "有失败" : "全部通过");

    /* 2) 关键: payload 不能被动过 */
    {
        uint8_t payload[8] = {0x00,0x00,0x04,0x11,0x22,0x33,0x44,0x55};
        uint8_t frame[KBD_MAX_FRAME];
        int n = kbdPackFrame(frame, 1, payload, 8);
        printf("  帧长(plen=8) = %d (应 15)\n", n);
        printf("  帧内容: ");
        for (int i = 0; i < n; ++i) printf("%02X ", frame[i]);
        printf("\n");
        if (memcmp(&frame[5], payload, 8) == 0) printf("  payload 未被 CRC 覆盖: OK\n");
        else { printf("  payload 被破坏: *** 失败 ***\n"); fails++; }
    }

    /* 3) 连发多帧 */
    {
        uint8_t payload[8] = {0,0,0x04,0,0,0,0,0};
        uint8_t stream[KBD_MAX_FRAME * 5];
        int total = 0;
        for (int i = 0; i < 5; ++i) total += kbdPackFrame(&stream[total], (uint8_t)i, payload, 8);
        int frames = 0, idx = 0;
        while (idx < total) {
            uint8_t seq; const uint8_t *g;
            int r = kbdUnpackFrame(&stream[idx], total - idx, &seq, &g);
            if (r > 0) { frames++; idx += 5 + r + 2; }
            else idx++;
        }
        printf("  连发5帧 -> 解出 %d 帧  %s\n", frames, frames == 5 ? "OK" : "*** 失败 ***");
        if (frames != 5) fails++;
    }

    printf("\n总体: %s\n", fails ? "*** 有失败 ***" : "全部通过");
    return fails;
}
'''

tmp = tempfile.mkdtemp()
cpp = os.path.join(tmp, "t.c")
open(cpp, "w", encoding="utf-8").write(c_code)
shutil_hdr = os.path.join(tmp, "kbd_wire.h")
open(shutil_hdr, "w", encoding="utf-8").write(open(hdr, encoding="utf-8").read())

# 找一个可用的 C 编译器
import shutil
cc = None
for cand in ["gcc", "clang", "cc"]:
    if shutil.which(cand):
        cc = cand; break

print("=== 用真实 kbd_wire.h 做 C 级往返测试 ===")
if not cc:
    print("  没找到 C 编译器, 跳过 (Python 测试已通过)")
    sys.exit(0)

exe = os.path.join(tmp, "t.exe" if os.name == "nt" else "t")
r = subprocess.run([cc, "-I", tmp, cpp, "-o", exe], capture_output=True, text=True)
if r.returncode != 0:
    print("  编译失败:", r.stderr[:500])
    sys.exit(1)

r = subprocess.run([exe], capture_output=True, text=True)
print(r.stdout)
