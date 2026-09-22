// ============================================================================
// kbd_desc.h —— 报告描述符 (单接口 + 双 Top-Level Collection)
//
// 【为什么是单接口】
//   预编译的 TinyUSB 库写死了 CFG_TUD_HID = 1 (证据: 符号 _hidd_itf 总大小
//   140 字节, tud_hid_n_report 里 instance 步长也是 140 -> 只放得下 1 个实例)。
//   工程里写 -DCFG_TUD_HID=2 改不了已编译好的 .a。
//   因此不能注册两个 HID 接口 —— 第二个接口会被 TinyUSB 的 hidd_open() 拒绝,
//   整个配置枚举失败(这正是上一版鼠标一起坏掉的原因)。
//
// 【单接口为什么够用】
//   Windows 的 kbdhid / mouhid 是按【Top-Level Collection】加载的, 不是按接口。
//   一个接口里放两个 Collection(键盘 + 鼠标), 系统会分别把它们认成键盘和鼠标。
//   接口本身声明为 Boot Keyboard (Class_03/SubClass_01/Prot_01),
//   与真键盘接收器的 MI_00 对齐。
//
// 【Report ID 的取舍】
//   两个 Collection 必须在同一个接口里共存, 因此必须用 Report ID 区分。
//   键盘 = Report ID 1, 鼠标 = Report ID 2。
//   透传时把真键盘报文的 Report ID 归一化为 1 后发出(见 usb_hid.cpp)。
//   真键盘本身若带 Report ID, 右板会剥掉再透传, 保证这里格式统一。
// ============================================================================

#ifndef KBD_DESC_H
#define KBD_DESC_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// 接口 / 端点
// ---------------------------------------------------------------------------
#define ITF_NUM_HID        0
#define ITF_NUM_TOTAL      1

// 一个接口, 两个 IN 端点? 不行 —— 单接口内多个端点需在接口描述符里声明数量。
// 这里用【一个 IN 端点, 两种 Report ID】, 是标准且最简单可靠的做法。
#define EPNUM_HID_IN       0x81

// ---------------------------------------------------------------------------
// Report ID
// ---------------------------------------------------------------------------
#define RID_KEYBOARD       1
#define RID_MOUSE          2

// ---------------------------------------------------------------------------
// 报告描述符 (键盘 + 鼠标 两个 Collection)
//
// 无 Report ID 的用法在这里不适用(两个 Collection 必须区分), 所以都用 RID。
// ---------------------------------------------------------------------------
#define KBD_REPORT_DESC                                                     \
    /* ================= 键盘 Collection ================= */              \
    0x05, 0x01,        /* Usage Page (Generic Desktop)        */           \
    0x09, 0x06,        /* Usage (Keyboard)                    */           \
    0xA1, 0x01,        /* Collection (Application)            */           \
    0x85, RID_KEYBOARD,/*   Report ID (1)                      */           \
    0x05, 0x07,        /*   Usage Page (Keyboard/Keypad)      */           \
    0x19, 0xE0,        /*   Usage Min (0xE0 LeftCtrl)         */           \
    0x29, 0xE7,        /*   Usage Max (0xE7 RightGUI)         */           \
    0x15, 0x00,        /*   Logical Min (0)                   */           \
    0x25, 0x01,        /*   Logical Max (1)                   */           \
    0x75, 0x01,        /*   Report Size (1)                   */           \
    0x95, 0x08,        /*   Report Count (8)                  */           \
    0x81, 0x02,        /*   Input (Data,Var,Abs)  -> 修饰键    */           \
    0x95, 0x01,        /*   Report Count (1)                  */           \
    0x75, 0x08,        /*   Report Size (8)                   */           \
    0x81, 0x01,        /*   Input (Const)         -> 保留字节  */           \
    0x95, 0x05,        /*   Report Count (5)                  */           \
    0x75, 0x01,        /*   Report Size (1)                   */           \
    0x05, 0x08,        /*   Usage Page (LED)                  */           \
    0x19, 0x01,        /*   Usage Min (1)                     */           \
    0x29, 0x05,        /*   Usage Max (5)                     */           \
    0x91, 0x02,        /*   Output (Data,Var,Abs) -> LED       */           \
    0x95, 0x01,        /*   Report Count (1)                  */           \
    0x75, 0x03,        /*   Report Size (3)                   */           \
    0x91, 0x01,        /*   Output (Const)                    */           \
    0x95, 0x06,        /*   Report Count (6)                  */           \
    0x75, 0x08,        /*   Report Size (8)                   */           \
    0x15, 0x00,        /*   Logical Min (0)                   */           \
    0x25, 0x65,        /*   Logical Max (101)                 */           \
    0x05, 0x07,        /*   Usage Page (Keyboard/Keypad)      */           \
    0x19, 0x00,        /*   Usage Min (0)                     */           \
    0x29, 0x65,        /*   Usage Max (101)                   */           \
    0x81, 0x00,        /*   Input (Data,Ary,Abs) -> 键码数组   */           \
    0xC0,              /* End Collection                      */           \
                                                                           \
    /* ================= 鼠标 Collection ================= */              \
    0x05, 0x01,        /* Usage Page (Generic Desktop)        */           \
    0x09, 0x02,        /* Usage (Mouse)                       */           \
    0xA1, 0x01,        /* Collection (Application)            */           \
    0x85, RID_MOUSE,   /*   Report ID (2)                     */           \
    0x09, 0x01,        /*   Usage (Pointer)                   */           \
    0xA1, 0x00,        /*   Collection (Physical)             */           \
    0x05, 0x09,        /*     Usage Page (Button)             */           \
    0x19, 0x01,        /*     Usage Min (1)                   */           \
    0x29, 0x05,        /*     Usage Max (5)                   */           \
    0x15, 0x00,        /*     Logical Min (0)                 */           \
    0x25, 0x01,        /*     Logical Max (1)                 */           \
    0x95, 0x05,        /*     Report Count (5)                */           \
    0x75, 0x01,        /*     Report Size (1)                 */           \
    0x81, 0x02,        /*     Input (Data,Var,Abs) -> 按键     */           \
    0x95, 0x01,        /*     Report Count (1)                */           \
    0x75, 0x03,        /*     Report Size (3)                 */           \
    0x81, 0x01,        /*     Input (Const)       -> 填充      */           \
    0x05, 0x01,        /*     Usage Page (Generic Desktop)    */           \
    0x09, 0x30,        /*     Usage (X)                       */           \
    0x09, 0x31,        /*     Usage (Y)                       */           \
    0x09, 0x38,        /*     Usage (Wheel)                   */           \
    0x15, 0x81,        /*     Logical Min (-127)              */           \
    0x25, 0x7F,        /*     Logical Max (127)               */           \
    0x75, 0x08,        /*     Report Size (8)                 */           \
    0x95, 0x03,        /*     Report Count (3)                */           \
    0x81, 0x06,        /*     Input (Data,Var,Rel) -> X/Y/Wheel*/           \
    0xC0,              /*   End Collection                    */           \
    0xC0               /* End Collection                      */

// 由 count_desc.py 精确计算并回填 (见该脚本)
#define KBD_REPORT_DESC_LEN   119

#endif // KBD_DESC_H
