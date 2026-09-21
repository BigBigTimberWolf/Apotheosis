#include "handleCommands.h"
#include "InitSettings.h"
#include "tasks.h"
#include "diag.h"
#include <Arduino.h>
#include <USB.h>
#include "USBSetup.h"
#include "usb_desc.h"   // 直写 TinyUSB: kbdUsbSendKeyboard / kbdUsbSendMouse
#include "tusb.h"
#include <esp_intr_alloc.h>
#include <cstring>
#include <atomic>
#include <mutex>
#include <RingBuf.h>

// 鼠标位置累积与非覆盖型位移记账 (双路汇总: 真实鼠标 + 上位机注入)
static std::atomic<int32_t> s_pending_dx(0);
static std::atomic<int32_t> s_pending_dy(0);
static std::atomic<int32_t> s_pending_wheel(0);

// Task handles
extern TaskHandle_t mouseMoveTaskHandle;

// ==================== 固件接线: 私有二进制协议 + 纯直通透传 + 键盘 ====================
proto::Parser g_proto;    // Serial0(PC 链路)
proto::Parser g_proto1;   // Serial1(真实鼠标链路 fw_host -> 本设备)

// ---- Serial1 二进制帧状态机 ----
//
// 为什么需要它: docs/proto.md §4.1 指出 "ASCII km.move 路径没有内联旁路, 恒定走任务通知,
// 因此二进制 0x01 的延迟严格低于 ASCII km.move"。但这套二进制解析器原来只挂在 Serial0 上,
// Serial1 是纯 ASCII 行解析 —— 主机根本没法把二进制发给设备, 实鼠标链路吃不到那个延迟优势。
//
// 兼容性(核心): 只在"行首"用 2 字节前瞻识别魔数 0xA5 0x5C / 0xDE 0xAD。
//   - 任何以可打印字符开头的 ASCII 行都不可能被误判;
//   - 首字节是 0xA5/0xDE 但不是完整魔数的, 其字节会原样回放进 ASCII 环, 不丢数据;
//   - 二进制帧的字节完全不进 ASCII 环, 两种格式共存而不互相污染。
enum : uint8_t { S1_BIN_IDLE = 0, S1_BIN_FRAME = 1 };
static uint8_t  s_s1_bin_state = S1_BIN_IDLE;
static uint8_t  s_s1_stage[2];       // 行首 2 字节暂存(确认非帧后再回放给 ASCII)
static uint8_t  s_s1_stage_n = 0;
static uint8_t  s_s1_skip = 0;       // 本行已放弃帧识别, 剩余字节直接走 ASCII

// 主机是否已通过 km.movefmt(1) 协商切换到二进制位移帧. 默认 0(ASCII), 保证向后兼容.
static volatile bool g_serial1_binary_moves = false;

// ---- 命令来源标记: 真实鼠标(Serial1) vs 上位机注入(Serial0) ----
static const uint8_t SRC_INJ = 0, SRC_REAL = 1;
volatile uint8_t g_cmd_source = SRC_INJ;
static uint8_t g_real_mask = 0;          // 真实鼠标按键影子
static uint8_t g_inj_mask  = 0;          // 注入按键影子
static uint8_t s_last_out_mask = 0;      // 已输出到HID的合并掩码(差分用)

// ---- 按键/坐标状态锁 ----
// 原实现里 g_real_mask / g_inj_mask / s_last_out_mask / mouseX / mouseY 是普通全局变量,
// 却被 4 个不同任务(甚至跨核)做非原子的 read-modify-write:
//   serial1Task(真实鼠标 km.left)  core1 prio6
//   serial0Task(上位机注入 km.left) core1 prio6
//   mouseMoveTask                  core1 prio7  <- 会抢占上面两个, 正好落在 RMW 中间
//   ClickTick                      core0 prio2
// 竞态丢的如果是一次"抬键"(g_real_mask & ~bit), 那次抬键连 emitMergedMask 都没进去,
// 事后状态看起来完全自洽 => 没有任何机制会补发 => 游戏机侧按键永久卡死.
// 本锁串行化所有对这两个掩码和坐标的读改写. 注意: 持锁期间绝不调用 Mouse.* (会阻塞 100ms).
static SemaphoreHandle_t s_btn_mtx = nullptr;

// ---- 上位机(如Apotheosis)按键事件推送开关 ----
static volatile bool g_makcu_buttons_enabled = false;
static volatile bool g_async_sub = false;
static SemaphoreHandle_t s_tx0_mtx = nullptr;     // ASCII应答与0x84帧互斥(防字节级交错)

// 0x05 MOVE_CANCEL: 置位后, 正在拆分回放中的长位移会在两步之间中断
static volatile bool s_cancel_move = false;

// ---- 键盘注入状态 ----
//
// 键盘同样有"两个来源", 与鼠标按键的 g_real_mask/g_inj_mask 完全对称:
//   SRC_REAL : fw_host 送来的真实键盘快照 (0x23 KB_REPORT)
//   SRC_INJ  : 上位机注入 (0x21 KEY_MASK / 0x22 KEY_TAP)
//
// 分离的理由和鼠标一样: 如果两者共用一份状态, 上位机注入一次按键后,
// 真实键盘的下一帧快照(内容里没有那个键)会立刻把它抹掉 —— 注入静默失效;
// 反过来真实按键也会被注入的抬键清掉。所以各存各的, 出口再合并。
static uint8_t s_kbd_mod_real = 0;
static uint8_t s_kbd_keys_real[6] = {0};
static uint8_t s_kbd_mod_inj = 0;
static uint8_t s_kbd_keys_inj[6] = {0};

// 已经真正写进 HID 的合并状态(差分用, 与鼠标 s_last_out_mask 同思路)
static uint8_t s_kbd_mod_prev = 0;
static uint8_t s_kbd_keys_prev[6] = {0};

// ===== "已确认送出"状态 + 补发 =====
//
// 【为什么必须有这一对】
//   键盘是【快照】语义: 主机此刻看到的就是最后一帧的内容。所以一旦某一帧发失败
//   被静默丢掉, 而它恰好是【抬键】帧, 主机就永远认为那个键还按着 —— 用户看到的
//   就是"某个键卡住、疯狂重复", 而且不按任何错。
//   实测: 用户敲一串键, 其中 l/h/d 等整段被卡住重复输出。
//
//   根部原因是"每收一帧立刻发一次": 真实接收器约 1ms 一帧, 而 USB 端点也约
//   1ms 才送得出一帧, 于是长期跑在饱和状态, waitEndpointReady() 一旦抢不到
//   就返回 false, 那一帧直接丢。
//
// 【独立版(KBD_PASSTHROUGH)为什么没这个问题】
//   它根本不"每帧发一次": 收帧只更新状态, 由周期 tick 无条件发一次当前快照
//   (makcu_link.cpp 的 makcuLinkTick)。发失败也没关系, 下一个 tick 会再发同样的
//   快照 —— 状态天然收敛, 丢帧不会留下卡键。
//
// 【这里采用的方案: 确认送出 + 1ms 补发 + 周期性重申】
//   s_kbd_sent 只在【发送成功】后才更新(这一步是关键, 曾经写成"发送前先记账",
//   结果一次失败就永远不再重发该状态, 表现为永久卡键)。
//   只要 s_kbd_desired != s_kbd_sent, 1ms 的 kbdTick 就会重试, 直到真的送出去。
//   另外每 KBD_REAFFIRM_MS 无条件重申一次, 兜住"端点接受了但主机没收到"这一类。
#define KBD_REAFFIRM_MS   100

// ---- 真实键盘链路失活判据 ----
//
// 【为什么需要】
//   键盘是快照语义: 主机认为"最后收到的那一帧"就是当前状态。所以只要【抬键帧
//   没有到达左板】, 主机就永远认为那个键还按着 —— 用户看到的就是"某个键无限
//   重复, 而且之后敲什么都没反应"。可能的来路至少有四条:
//     1) 右板 USB 端点丢帧
//     2) Serial1(板间 5Mbps)丢帧
//     3) 右板正在转发的键盘接口发生变化, 旧接口的残留状态无人纠正
//     4) 键盘链路整体死掉(端点静默 / 设备掉线 / 板间线松)
//   前三条已经分别修掉了, 但(4)这类"链路整个不响了"没法靠补发解决 ——
//   补发只会把同一个卡住的状态反复送出去。所以必须有一个"判死并强制释放"的兜底。
//
// 【关键: 为什么用【心跳】而不是"收不到键盘帧"作判据】
//   只看"有没有键盘帧"是不行的, 因为有两种完全不同的原因会导致没有帧:
//     链路活着, 用户一直按着某个键 -> 接收器不再发新帧(只在变化时发)
//     链路死了
//   这两件事从左板看一模一样。若按第一种误判, 就会把用户的长按强行打断。
//
//   所以右板每 50ms 发一个【不带按键状态】的心跳(0x23 帧 + payload=[0xFF])。
//   于是判据变成: 心跳也没了 -> 链路死了 -> 可以安全地把按键全部释放。
//
// 【阈值】
//   心跳 50ms, 判死 400ms -> 8 倍余量, 容忍连续丢 7 个心跳。只有真的断了才触发。
#define KBD_LINK_TIMEOUT_MS  400

static uint32_t s_kbd_link_last_ms = 0;
static bool     s_kbd_link_seen    = false;

static uint8_t  s_kbd_desired[8]    = {0};   // 最近一次算出的"应该是什么状态"
static bool     s_kbd_desired_valid = false;
static uint8_t  s_kbd_sent[8]       = {0};   // 已【确认送出】的状态
static bool     s_kbd_sent_valid    = false;
static uint32_t s_kbd_lastSendMs    = 0;

struct KeyTap { uint8_t mod, key; uint32_t release_ms; bool active; };
static KeyTap s_taps[4] = {};

// ---- 固件内点击自动释放状态 ----
struct MouseClickState {
    uint8_t bits;
    uint32_t release_ms;
    bool active;
};
static MouseClickState s_mouse_clicks[4] = {};

// ---- 键盘状态锁 ----
//
// 上面 s_btn_mtx 只保护鼠标掩码与坐标, 键盘的 s_kbd_* 与 s_taps 一直是无锁的。
// 而它们被完全相同的三个任务跨核并发访问:
//   serial1Task(真实键盘 0x23 KB_REPORT) core1 prio6 -> kbdApplyRealReport
//   serial0Task(上位机注入 0x21/0x22)     core1 prio6 -> kbdApplyReport / handleKeyTapCmd
//   ClickTick(kbdTick 到期抬键)           core0 prio2  -> 真跨核, 不是伪并发
// 后果与鼠标掩码那一段注释描述的完全一样: kbdEmitMerged 的"差分写出"先用
// s_kbd_keys_prev 算出该 press/release 谁, 再覆盖 prev —— 若中途被 kbdTick 打断,
// prev 与实际写进 HID 的状态就永久失配, 之后所有差分都基于错误基线,
// 表现为某个键再也不被 release(永久卡死), 而键盘没有鼠标那样的周期看门狗兜底。
//
// 本锁串行化全部 s_kbd_* / s_taps 的读改写。铁律同 s_btn_mtx:
// 持锁期间绝不调用 Kbd.press/release(内部会等主机轮询, 可能阻塞), 只更新状态,
// 真正的 HID 写出放到锁外由 kbdEmitMerged 统一做。
static SemaphoreHandle_t s_kbd_mtx = nullptr;

static uint16_t crc16_local(const uint8_t *d, size_t n) {
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= *d++;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

// 读取两个掩码的合并值. 调用者必须已持有 s_btn_mtx.
static inline uint8_t mergedMaskLocked() {
    return (uint8_t)(g_real_mask | g_inj_mask);
}

static void emitMergedMask(uint8_t merged);

// 唯一的"改按键状态"入口. 锁内只做状态更新, 锁外才发 HID(避免持锁阻塞).
static void applyBtnLocked(uint8_t src, uint8_t bit, bool state) {
    if (!s_btn_mtx || xSemaphoreTake(s_btn_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;
    uint8_t *m = (src == SRC_REAL) ? &g_real_mask : &g_inj_mask;
    *m = state ? (uint8_t)(*m | bit) : (uint8_t)(*m & ~bit);
    uint8_t merged = mergedMaskLocked();
    xSemaphoreGive(s_btn_mtx);
    emitMergedMask(merged);
}

// 直接设定注入掩码(0x10 BUTTON_MASK / 0x44 PANIC).
static void setInjMaskLocked(uint8_t value) {
    if (!s_btn_mtx || xSemaphoreTake(s_btn_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;
    g_inj_mask = value & 0x1F;
    uint8_t merged = mergedMaskLocked();
    xSemaphoreGive(s_btn_mtx);
    emitMergedMask(merged);
}

// 清空全部按键状态(USB_GOODBYE / 设备重连). 真实鼠标侧也必须清,
// 否则真实鼠标按着键时被拔出会留下永久按下的 bit(PANIC 也救不回来).
static void clearAllButtonsLocked() {
    if (!s_btn_mtx || xSemaphoreTake(s_btn_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return;
    g_real_mask = 0;
    g_inj_mask = 0;
    mouseX = 0;
    mouseY = 0;
    uint8_t merged = mergedMaskLocked();
    xSemaphoreGive(s_btn_mtx);
    emitMergedMask(merged);
}

// ============================================================================
// 克隆感知的输出路由
//
// 未克隆时走内置 USBHIDMouse/USBHIDKeyboard(标准 5 字节鼠标 / 8 字节键盘格式)。
// 克隆生效后改走克隆实例, 按真设备的报文格式组装 —— 否则被控机读到的描述符
// 与收到的字节流对不上, 坐标和按键会整体错位。
//
// 为什么要保留内置路径: 克隆依赖 fw_host 提供真描述符, 而 fw_host 可能没插、
// 或插的设备描述符解析失败。此时必须仍然能用, 只是"身份不克隆"而已。
// ============================================================================

// 这三个函数是"设备输出"的唯一出口。
//
// 保留这层间接的理由: 报文格式克隆(阶段 4)当前做不了(原因见 USBSetup.cpp),
// 但一旦将来框架允许, 只需改这三个出口即可, 不必再动 handleMove /
// handleMouseWheel / emitMergedMask 等一堆调用点。
// sendClonedMouseReport() 目前恒返回 false, 因此实际走的都是内置路径。

// 鼠标按键状态。
// 原来由 Arduino 的 Mouse 对象内部维护; 现在直写 TinyUSB, 需要自己记。
// 位序与报告描述符一致: bit0=L bit1=R bit2=M bit3=S1(前) bit4=S2(后)
static uint8_t s_mouseBtnState = 0;

// ===== 诊断计数器 (供 main.cpp 心跳打印) =====
// 左板是链路下游: 同时能看到「右板送来多少」和「本级发出多少」,
// 因此这三个数就能定位断点在右板还是在发送口。
uint32_t volatile g_diagMoveRx   = 0;   // 收到真实鼠标位移帧数
uint32_t volatile g_diagBtnRx    = 0;   // 收到真实鼠标按键帧数
uint32_t volatile g_diagKbdRx    = 0;   // 收到真实键盘报文数
uint32_t volatile g_diagKbdHbt   = 0;   // 收到键盘链路心跳数(右板每 50ms 一发)
uint32_t volatile g_diagEmitOk   = 0;   // 成功发出的 HID 报文数
uint32_t volatile g_diagEmitFail = 0;   // 发送失败(超时/端点忙)次数
uint32_t volatile g_diagMoved    = 0;   // 累计已发出的位移量(绝对值)

// ★ 出口改为返回 bool
//
// 【为什么必须返回】
//   原来 void 版本丢掉发送结果, 调用方一律按"已发出"记账。
//   而直写 TinyUSB 之后, 发送【可能在端点忙时失败】—— 于是位移被静默吞掉,
//   km.getpos / km.moveto 的坐标也跟着错。失败必须让调用方知道。
static bool emitMouseMove(int8_t x, int8_t y) {
    if (sendClonedMouseReport(x, y, 0, 0)) { g_diagEmitOk++; return true; }
    // 无 Report ID, 一次发全量(按键 + 位移 + 滚轮)
    const bool ok = kbdUsbSendMouse(s_mouseBtnState, x, y, 0);
    if (ok) { g_diagEmitOk++; g_diagMoved += (uint32_t)((x < 0 ? -x : x) + (y < 0 ? -y : y)); }
    else    { g_diagEmitFail++; }
    return ok;
}

static bool emitMouseWheel(int8_t w) {
    if (sendClonedMouseReport(0, 0, w, 0)) { g_diagEmitOk++; return true; }
    const bool ok = kbdUsbSendMouse(s_mouseBtnState, 0, 0, w);
    if (ok) g_diagEmitOk++; else g_diagEmitFail++;
    return ok;
}

static bool emitMouseButtons(uint8_t mask) {
    if (sendClonedMouseReport(0, 0, 0, mask)) { g_diagEmitOk++; return true; }

    // ★ 掩码没变就别重发。
    //
    // 本函数会被 ClickTick 以 1ms 周期调用(按键重申看门狗)。原来这里无条件
    // 调 kbdUsbSendMouse, 于是形成 ~1000 次/秒 的发送洪水 —— 而 USB 端点
    // 每 1ms 只能送【一个】报文, 真实键盘报文只能在这股洪水里排队抢机会,
    // 表现为"按了键有时候完全没反应"。
    //
    // 实测: emit ok 以 1500/秒增长, 而同期真实键盘报文只有 41 个。
    //
    // 状态没变就不发是安全的: 看门狗的目的是"把丢失的抬键补回来", 而一旦
    // 掩码与本地影子状态一致, 说明上一次已经按该掩码发过, 无需再发。
    const uint8_t m = (uint8_t)(mask & 0x1F);

    // 与【上次成功发出】的掩码比较。
    // 注意不能拿 s_mouseBtnState 当依据 —— 它在下面被赋值, 若发送失败就会
    // 记成"已发", 之后同样的掩码永远不再重发 => 抬键丢失后按键卡死。
    static uint8_t s_mouseBtnSent  = 0;
    static bool    s_mouseBtnValid = false;
    if (s_mouseBtnValid && m == s_mouseBtnSent) return true;

    s_mouseBtnState = m;                      // 当前状态(位移报文会带上它)
    const bool ok = kbdUsbSendMouse(m, 0, 0, 0);
    if (ok) {
        s_mouseBtnSent  = m;                  // 只有成功才记录
        s_mouseBtnValid = true;
        g_diagEmitOk++;
    } else {
        g_diagEmitFail++;                     // 失败保留重试机会
    }
    return ok;
}

// 把合并掩码下发到 HID。参数是"调用方观察到的掩码", 但真正提交以锁内读到的
// 权威值为准 —— 修掉审查 C4: 调用方在锁外拿到 merged 之后、调用本函数之前,
// 另一任务可能已经改了 g_real_mask/g_inj_mask。若直接采信传入值并写回
// s_last_out_mask, 影子状态就会与实际 HID 状态失配, 之后真实的变化会因
// `merged == s_last_out_mask` 被误判为"无变化"而静默丢弃 -> 按键卡死
// (正是下面看门狗注释想防的问题, 但看门狗每 100ms 才纠一次)。
// 传入值仅作"是否需要下发"的提示: 它过期也不影响正确性, 因为这里会重新判一次。
static void emitMergedMask(uint8_t mergedHint) {
    if (!s_btn_mtx || xSemaphoreTake(s_btn_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;
    const uint8_t merged = mergedMaskLocked();      // 权威值
    if (merged == s_last_out_mask) {                // 真的没变化才跳过
        xSemaphoreGive(s_btn_mtx);
        return;
    }
    s_last_out_mask = merged;
    xSemaphoreGive(s_btn_mtx);

    emitMouseButtons(merged);
}

// ---- 周期性重申看门狗 ----
// 为什么必须有: emitMergedMask 只在"状态变化"时下发, 且发送失败不可见
// (USBHIDMouse::move() 是 void, 内部 SendReport 的返回值被丢弃). 一旦某次抬键丢失,
// 没有任何东西会重试 => 永久卡死. 本函数每 100ms 无条件重申一次当前掩码, 强制收敛.
// 代价几乎为零: USBHIDMouse::buttons() 内部有 `if (b != _buttons)` 判断,
// 重申未变化的状态不会产生多余 USB 报文.
static uint32_t s_last_assert_ms = 0;

void buttonWatchdogTick() {
    uint32_t now = millis();
    if ((uint32_t)(now - s_last_assert_ms) < 100) return;
    s_last_assert_ms = now;
    if (!s_btn_mtx || xSemaphoreTake(s_btn_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;
    uint8_t merged = mergedMaskLocked();
    s_last_out_mask = 0xFF;            // 强制让下一次 diff 重新下发全部按键状态
    xSemaphoreGive(s_btn_mtx);
    emitMergedMask(merged);
}

static void updateButtonState() {
    uint8_t merged;
    uint8_t real_snapshot, inj_snapshot;
    if (!s_btn_mtx || xSemaphoreTake(s_btn_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;
    merged        = mergedMaskLocked();
    real_snapshot = g_real_mask;
    inj_snapshot  = g_inj_mask;
    xSemaphoreGive(s_btn_mtx);

    emitMergedMask(merged);

    // 1. 如果上位机开启了 MAKCU 按键事件流 (如 Apotheosis 的 km.buttons(1))
    // 直接下发非可打印单字节掩码 (<32)，上位机 listener 零延迟捕获按键状态！
    // 走 s_tx0_mtx: 必须和 ASCII 应答互斥, 否则两种字节流会交错成垃圾.
    if (g_makcu_buttons_enabled && s_tx0_mtx) {
        if (xSemaphoreTake(s_tx0_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
            Serial0.write(merged & 0x1F);
            xSemaphoreGive(s_tx0_mtx);
        }
    }

    // 2. 如果开启了二进制异步订阅
    // 栈缓冲而非 std::vector: 本函数在高频按键事件上被调用, 原实现在热路径做
    // 堆分配(ESP32-S3 堆操作需持锁), 会造成延迟抖动与碎片。
    if (g_async_sub && s_tx0_mtx) {
        uint8_t f[9];
        f[0] = 0xA5; f[1] = 0x5C; f[2] = 2; f[3] = 0;
        f[4] = 0x84; f[5] = real_snapshot; f[6] = inj_snapshot;
        uint16_t c = crc16_local(f + 2, 5);
        f[7] = (uint8_t)(c & 0xFF);
        f[8] = (uint8_t)((c >> 8) & 0xFF);
        if (xSemaphoreTake(s_tx0_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
            Serial0.write(f, sizeof(f));
            xSemaphoreGive(s_tx0_mtx);
        }
    }
}

static void applyBtn(uint8_t bit, bool state) {
    // g_cmd_source 是全局的"当前来源", 会随帧变化; 在锁内取值可避免跨任务读到已被改写的值
    uint8_t src;
    if (s_btn_mtx && xSemaphoreTake(s_btn_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        src = g_cmd_source;
        xSemaphoreGive(s_btn_mtx);
    } else {
        src = g_cmd_source;
    }
    applyBtnLocked(src, bit, state);
}

static bool keysContains(const uint8_t keys6[6], uint8_t k) {
    if (!k) return false;
    for (int i = 0; i < 6; ++i) if (keys6[i] == k) return true;
    return false;
}

// 把"真实键盘快照"登记进 real 侧, 然后合并两个来源真正写 HID。
//
// 合并规则与鼠标按键的 merged = real | inj 一致:
//   - 修饰键按位或(两边都能按 Shift, 谁先松都不会误放另一个的)
//   - 键码取并集, 最多 6 个(HID 标准键盘报文的槽位数)
// 这样真实键盘和上位机注入可以同时在用, 互不干扰。
static void kbdEmitMerged() {
    // ---- 第一阶段: 锁内完成"合并 + 差分计算 + 提交 prev" ----
    //
    // 差分结果先记进局部数组, 提交 s_kbd_*_prev 也在这里做完, 然后立刻放锁。
    // 这样"读 prev -> 算差异 -> 写 prev"是一个不可分割的临界区, 不会被 core0 的
    // kbdTick 插进来造成 prev 与实际 HID 状态失配(那会导致永久卡键, 见 s_kbd_mtx 说明)。
    uint8_t  pressMod = 0, releaseMod = 0;
    uint8_t  pressKeys[6] = {0,0,0,0,0,0};
    uint8_t  releaseKeys[6] = {0,0,0,0,0,0};
    uint8_t  nPress = 0, nRelease = 0;
    uint8_t  mod = 0;
    bool     committed = false;
    uint8_t  mergedKeys[6] = {0,0,0,0,0,0};   // 合并后的最终键码状态(供接口1发快照)

    if (s_kbd_mtx && xSemaphoreTake(s_kbd_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        mod = (uint8_t)(s_kbd_mod_real | s_kbd_mod_inj);

        // ★ tap 也是"按下来源"。0x22 KEY_TAP 只登记槽位并置 release_ms,
        //   按下靠这里把它的键/修饰位算进合并快照 —— 否则 tap 永远按不下去
        //   (旧实现是在 handleKeyTapCmd 里直接按键, 改成快照语义后那一步没了)。
        for (int i = 0; i < 4; ++i) {
            if (s_taps[i].active) mod |= s_taps[i].mod;
        }

        uint8_t keys[6] = {0, 0, 0, 0, 0, 0};
        uint8_t n = 0;
        // real 侧优先占位, 再补 inj 侧(去重)
        for (int i = 0; i < 6 && n < 6; ++i) {
            const uint8_t k = s_kbd_keys_real[i];
            if (k && !keysContains(keys, k)) keys[n++] = k;
        }
        for (int i = 0; i < 6 && n < 6; ++i) {
            const uint8_t k = s_kbd_keys_inj[i];
            if (k && !keysContains(keys, k)) keys[n++] = k;
        }
        // tap 的键码兜底补位 (槽位有限, 满则丢弃而不是挤掉别人)
        for (int i = 0; i < 4 && n < 6; ++i) {
            const uint8_t k = s_taps[i].active ? s_taps[i].key : 0;
            if (k && !keysContains(keys, k)) keys[n++] = k;
        }

        // 修饰键差分
        for (uint8_t m = 0; m < 8; ++m) {
            const uint8_t bit = 1 << m;
            const bool now = (mod & bit) != 0;
            const bool was = (s_kbd_mod_prev & bit) != 0;
            if (now && !was) pressMod   |= bit;
            else if (!now && was) releaseMod |= bit;
        }

        // 键码差分(与旧实现逐字节等价, 只是先记录后发送)
        for (int i = 0; i < 6; ++i) {
            const uint8_t old_k = s_kbd_keys_prev[i];
            if (old_k && !keysContains(keys, old_k) && nRelease < 6) releaseKeys[nRelease++] = old_k;
        }
        for (int i = 0; i < 6; ++i) {
            const uint8_t new_k = keys[i];
            if (new_k && !keysContains(s_kbd_keys_prev, new_k) && nPress < 6) pressKeys[nPress++] = new_k;
        }

        // 提交: prev 与新状态一致
        memcpy(s_kbd_keys_prev, keys, 6);
        s_kbd_mod_prev = mod;
        committed = true;

        // 把合并结果带出临界区, 供锁外从接口1发完整快照用(见下方)
        memcpy(mergedKeys, keys, 6);

        xSemaphoreGive(s_kbd_mtx);
    }

    if (!committed) return;   // 拿不到锁就整帧丢弃: prev 未动, 下一帧会重新算出同样的差异

    // ---- 第二阶段: 锁外真正写 HID ----
    // Kbd.press/release 内部要等主机轮询报文, 可能阻塞, 绝不能在持锁时调用。
    //
    // 克隆路径当前恒不可用(sendClonedKbdReport 恒 false), 保留判定以便将来启用:
    // 克隆生效时应发完整快照而不是逐键差分, 因此这里先尝试克隆发送。
    if (sendClonedKbdReport(mod, nullptr)) return;

    // ===== 直写 TinyUSB: 发【完整快照】 =====
    //
    // 只有一个键盘接口 (Boot Keyboard, 无 Report ID), 所以直接发 8 字节:
    //     byte0 = modifier, byte1 = 保留, byte2..7 = 6 个键码
    //
    // 【为什么发快照而不是差分】
    //   boot keyboard 无 Report ID, 主机每次期望的就是完整状态。
    //   真键盘也是按键一变就重发全量, 所以这与真实行为一致。
    //
    // mergedKeys 是本函数锁内算好的最终状态 (real ∪ inj ∪ tap), 直接用。
    //
    // 【注意: 这里【不】用 kbdUsbReady() 包住】
    //   端点暂时不可用也必须先把"应该是什么状态"记下来, 否则这一帧就永久消失了
    //   —— 抬键帧消失就是永久卡键。发送由下面的逻辑尝试, 失败留给 kbdTick 补发。
    {
        uint8_t rep[8];
        rep[0] = mod;
        rep[1] = 0;
        memcpy(rep + 2, mergedKeys, 6);

        // ---- 记下"应该是什么状态", 由 kbdTick 负责把它真的送出去 ----
        //
        // ★ 内容相同且已确认送出时【跳过】, 避免 1ms 周期的快照洪水
        //   (端点每 1ms 只送得出一帧, 洪水会让真实按键排队丢帧)。
        //   但跳过的前提是 s_kbd_sent 记的是【发送成功过】的内容 —— 见下面的
        //   记账时机, 以及 kbdTick 里的补发逻辑。
        memcpy(s_kbd_desired, rep, 8);
        s_kbd_desired_valid = true;

        const uint32_t nowMs = millis();
        const bool sameSent = s_kbd_sent_valid && memcmp(s_kbd_sent, rep, 8) == 0;
        const bool needReaffirm =
            (uint32_t)(nowMs - s_kbd_lastSendMs) >= KBD_REAFFIRM_MS;

        if (!(sameSent && !needReaffirm)) {
            if (kbdUsbReady() && kbdUsbSendKeyboard(rep)) {
                // ★ 只在【发送成功之后】才记账。
                //
                //   这一步是卡键故障的根: 曾经写成"发送前先记账", 于是一次发送
                //   失败也会被记成"已送出", 之后同样的状态永远不再重发 ——
                //   抬键帧丢掉就永久卡键。现在失败不记账, kbdTick 每 1ms 会拿
                //   s_kbd_desired 继续重试, 直到真的送出去。
                memcpy(s_kbd_sent, rep, 8);
                s_kbd_sent_valid = true;
                s_kbd_lastSendMs = nowMs;
                g_diagEmitOk++;
            } else {
                g_diagEmitFail++;   // 不记账 -> 下一 tick 重试; 绝不静默丢弃
            }
        }
        // 否则: 主机已经就是这个状态, 不必再占一次端点。
    }

    // 差分的中间量不再使用(保留计算是为了不动锁内逻辑), 显式标记避免告警
    (void)pressMod; (void)releaseMod;
    (void)nPress; (void)nRelease;
    (void)pressKeys; (void)releaseKeys;
}

// 旧签名保留: 命令行/注入侧直接设 inj 状态再合并。
static void kbdApplyReport(uint8_t mod, const uint8_t keys6[6]) {
    if (s_kbd_mtx && xSemaphoreTake(s_kbd_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_kbd_mod_inj = mod;
        memcpy(s_kbd_keys_inj, keys6, 6);
        xSemaphoreGive(s_kbd_mtx);
    }
    kbdEmitMerged();
}

// 真实键盘快照(fw_host 透传)。只改 real 侧, 不碰 inj 侧。
// ============================================================================
// 键盘原样转发入口 (与独立版架构一致)
//
// 右板把端点收到的【原始字节】透传过来, 这里【原样发给 USB】——
// 不解析、不合并、不查表、不去重。用户实测这套最流畅。
//
// raw[0] = 修饰键, raw[1] = 保留, raw[2..7] = 6 个键码
// (标准 boot keyboard 布局, 与真键盘直插时主机看到的一致)
//
// 【km.mask 屏蔽仍然有效】
//   屏蔽在【右板】做: fw_host 在【原样转发路径】上判 maskIsActive(), 屏蔽窗口内
//   直接丢弃真实键盘报文。注意屏蔽【不是】在 onKeyboard() 里判的 —— 那条路已被
//   原样转发旁路, 把门控写在 onKeyboard 里等于屏蔽失效(这是一个已经踩过的坑)。
//   所以这里收不到报文就等于被屏蔽了, 与左板怎么发无关。
// ============================================================================
static void kbdApplyRealRaw(const uint8_t *raw, uint8_t len)
{
    if (!raw || len < 8) return;

    // ★ 角色互锁: 只有键盘角色的左板才发键盘报文。
    //
    //   左板的 HID 接口是二选一的 —— 键盘角色下是 8 字节 boot 键盘, 鼠标角色下
    //   是 4 字节鼠标。若鼠标单板上收到一帧 8 字节键盘报文(右板的键盘兜底判定
    //   在某些复合设备上可能误发), 直接写进 4 字节的鼠标接口会让被控机读到错位
    //   数据, 表现为鼠标乱动/乱按键。这条互锁把鼠标单板完全隔离在外。
    if (!kbdUsbIsKeyboardRole()) {
        g_diagEmitFail++;
        return;
    }

    // ---- 只更新"真实侧"状态, 不在回调里直接发 ----
    //
    // ★ 这里是卡键故障的修复点。
    //
    //   原来的写法是"收到一帧就立刻 kbdUsbSendKeyboard() 发一次"。真实接收器
    //   约 1ms 一帧, 而 USB 端点也约 1ms 才送出一帧, 于是长期跑在饱和边缘;
    //   waitEndpointReady() 一旦抢不到就返回 false, 那一帧被静默丢掉。
    //   丢普通帧无所谓(下一帧内容相同会补上), 但【丢抬键帧就是永久卡键】——
    //   用户实测: 敲一串键, l/h/d 等整段卡住无限重复。
    //
    //   改成独立版那套模型: 回调只更新状态, 由 1ms 的 kbdTick 送当前快照,
    //   并且只在【确认送出】后才记账, 没送出去就一直重试。状态天然收敛,
    //   丢帧不可能留下卡键。
    //
    // ★ 与注入的关系: real 和 inj 各存各的, 出口合并 (与独立版
    //   `kbd.mergeFrom(s_realKbd)` 完全一致)。用户保证两者不冲突。
    if (s_kbd_mtx && xSemaphoreTake(s_kbd_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_kbd_mod_real = raw[0];
        memcpy(s_kbd_keys_real, raw + 2, 6);
        xSemaphoreGive(s_kbd_mtx);
    }
    s_kbd_link_last_ms = millis();     // 收到真实键盘报文也说明链路活着
    s_kbd_link_seen    = true;

    // 立刻尝试送一次(低延迟), 送不出去就留给 kbdTick 补发。
    kbdEmitMerged();
}

static void kbdApplyRealReport(uint8_t mod, const uint8_t keys6[6]) {
    // 旧版 6 字节兼容路径(0x23 帧 payload = [mod][key0..key5])。
    //
    // ★ 只做一件事: 把老格式拼成标准 8 字节 boot 报文, 然后交给原样转发的同一条路。
    //
    // 【为什么不再直接 kbdUsbSendKeyboard】
    //   直接发会绕过"确认送出"的记账, 让 s_kbd_sent 与实际主机状态失配 ——
    //   之后 kbdTick 的补发判据就不可靠了(有可能误判成"已送出"而不再重试)。
    //   统一走 kbdApplyRealRaw, 全链路只有【一个】发出口, 记账不会分叉。
    uint8_t raw[8];
    raw[0] = mod;
    raw[1] = 0;
    memcpy(raw + 2, keys6, 6);
    kbdApplyRealRaw(raw, sizeof(raw));
}

void handleKeyMaskCmd(uint8_t mod, const uint8_t keys6[6]) {
    kbdApplyReport(mod, keys6);
}

static void kbdTick() {
    uint32_t now = millis();

    // ===== 真实键盘链路失活兜底: 把永久卡键变成不可能 =====
    //
    // 判据是【心跳】而不是"键盘帧" —— 理由见 KBD_LINK_TIMEOUT_MS 处:
    //   "收不到键盘帧"既可能是链路断了, 也可能是用户一直按着, 两者从左板看
    //   完全一样。右板每 50ms 发的无状态心跳把这两件事区分开了。
    //   只有心跳也消失 400ms, 才判定链路已死并释放全部按键。
    if (s_kbd_link_seen && kbdUsbIsKeyboardRole() &&
        (uint32_t)(now - s_kbd_link_last_ms) >= KBD_LINK_TIMEOUT_MS) {
        s_kbd_link_seen = false;      // 只报一次; 下一帧报文或心跳会重新置位

        bool anyHeld = false;
        if (s_kbd_mtx && xSemaphoreTake(s_kbd_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
            anyHeld = (s_kbd_mod_real != 0);
            for (int i = 0; i < 6 && !anyHeld; ++i) {
                if (s_kbd_keys_real[i]) anyHeld = true;
            }
            if (anyHeld) {
                s_kbd_mod_real = 0;
                memset(s_kbd_keys_real, 0, sizeof(s_kbd_keys_real));
            }
            xSemaphoreGive(s_kbd_mtx);
        }

        if (anyHeld) {
            Serial0.printf("[KBD] kbd link dead %lums -> forced release\n",
                           (unsigned long)(now - s_kbd_link_last_ms));
            kbdEmitMerged();          // 把"全松开"真的发给主机
        }
    }

    // 锁内: 判定哪些 tap 到期, 并决定到期后该抬哪些键/修饰位。
    // 修饰位的"是否仍被需要"判据改为真正的来源状态 (real|inj) 与其它存活的 tap,
    // 而不是 s_kbd_mod_prev —— prev 是"已写进 HID 的合并快照", 它无法区分
    // 这个 Shift 是 0x21 要求的还是别处残留的, 用它判定会误抬/漏抬(见审查 L3)。
    uint8_t relKeys[4] = {0,0,0,0};
    uint8_t relMod  = 0;
    uint8_t nRel = 0;

    if (s_kbd_mtx && xSemaphoreTake(s_kbd_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (int i = 0; i < 4; ++i) {
            KeyTap &t = s_taps[i];
            if (!(t.active && (int32_t)(now - t.release_ms) >= 0)) continue;

            if (nRel < 4) relKeys[nRel++] = t.key;

            for (uint8_t m = 0; m < 8; ++m) {
                const uint8_t bit = 1 << m;
                if (!(t.mod & bit)) continue;
                bool stillNeeded = ((s_kbd_mod_real | s_kbd_mod_inj) & bit) != 0;
                for (int j = 0; j < 4 && !stillNeeded; ++j) {
                    if (j != i && s_taps[j].active && (s_taps[j].mod & bit)) stillNeeded = true;
                }
                if (!stillNeeded) relMod |= bit;
            }
            t.active = false;
        }
        xSemaphoreGive(s_kbd_mtx);
    }

    // 锁外: 状态已在锁内改好(占用计数/槽位已释放)。
    //
    // ★ 这里是"补发"的地方 —— 键盘不丢帧的保证。
    //
    // 【为什么当前状态还没送出就必须再发一次】
    //   键盘是快照语义: 主机只认最后一帧。若某一帧发送失败被丢掉, 而它恰好是
    //   抬键帧, 主机就永远认为那个键还按着 —— 用户实测到"l/h/d 整段卡住重复"。
    //
    //   所以判据不能是"有没有变化", 而必须是"主机是不是已经拿到当前状态":
    //     s_kbd_desired != s_kbd_sent  -> 重试发送 (含每 KBD_REAFFIRM_MS 的重申)
    //   s_kbd_sent 只在发送成功后才更新, 所以没送出去就会一直重试。
    //
    // 【为什么不能无条件发】
    //   真实接收器约 1ms 一帧, 端点也约 1ms 才送出一帧。无条件发就是每秒约
    //   1000 帧的洪水(实测 emit ok 约 1500/秒), 真实按键反而在洪水里排队丢帧。
    //   下面 kbdEmitMerged 内部会在"内容已确认送出且未到重申周期"时直接跳过,
    //   所以这里放心调用即可 —— 该跳过的它自己会跳。
    const bool needRetry =
        s_kbd_desired_valid &&
        (!s_kbd_sent_valid || memcmp(s_kbd_sent, s_kbd_desired, 8) != 0 ||
         (uint32_t)(now - s_kbd_lastSendMs) >= KBD_REAFFIRM_MS);

    if (nRel > 0 || relMod != 0 || needRetry) {
        kbdEmitMerged();
    } else {
        (void)relKeys;
    }
}

void handleKeyTapCmd(uint8_t mod, uint8_t key, uint16_t dur_ms) {
    // 锁内只做"抢槽位 + 登记", 不在这里按 HID 键 —— 由 kbdEmitMerged 在锁外统一写出。
    //
    // 修掉审查 L2: 原"4 槽全满"分支会 release(key)/release(mod), 但那条路径上
    // 从未 press 过它们 —— 释放的是【别人】(0x21 KEY_MASK 或其它 tap)正持有的键,
    // 会把并发组合键静默拆掉。正确做法是槽满时直接丢弃本次 tap: 不按也不抬,
    // 由调用方(或下一次 tap)自行重试, 绝不会误伤其它来源。
    if (!s_kbd_mtx || xSemaphoreTake(s_kbd_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;

    bool scheduled = false;
    for (auto &t : s_taps) {
        if (!t.active) {
            t.mod = mod; t.key = key;
            t.release_ms = millis() + (dur_ms ? dur_ms : 60);
            t.active = true;
            scheduled = true;
            break;
        }
    }
    xSemaphoreGive(s_kbd_mtx);
    if (!scheduled) return;

    // 按下: 修饰位优先于主键(与真实键盘的报文语义一致)。
    // 原实现只按了 key, mod 存进结构体后从未被使用 —— 0x22 的修饰键参数被静默
    // 忽略, "Shift+A" 这类组合键会降解成单键 A。
    // 槽位已在锁内登记, 由 kbdEmitMerged 统一算快照发出
    kbdEmitMerged();
}

// 由 ClickTick 任务每 1ms 调用: 处理点击/按键的定时抬键 + 按键状态重申看门狗
//
// 注意: s_mouse_clicks 由 protoOnClick(serial0Task, core1)写入, 本函数在
// ClickTick(core0)读写, 是跨核并发。原实现只把 g_inj_mask 的修改放进了锁,
// 槽位的扫描、c.active=false 都在锁外 —— 会与 protoOnClick 的"找空槽->占用"
// 交错, 导致同一个槽被两次占用(第二次的抬键计划覆盖第一次, 第一个点击永不弹起)
// 或槽位泄漏。现在整段扫描+提交都在锁内。
void clickTick() {
    uint32_t now = millis();
    bool changed = false;

    if (s_btn_mtx && xSemaphoreTake(s_btn_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (auto &c : s_mouse_clicks) {
            if (c.active && (int32_t)(now - c.release_ms) >= 0) {
                g_inj_mask &= ~c.bits;
                c.active = false;
                changed = true;
            }
        }
        xSemaphoreGive(s_btn_mtx);
    }

    if (changed) updateButtonState();
    kbdTick();
    buttonWatchdogTick();
}

static void protoOnButtonMask(uint8_t mask) {
    setInjMaskLocked(mask);
}

static void protoOnClick(uint8_t btn_bits, uint16_t down_ms) {
    btn_bits &= 0x1F;
    if (!btn_bits) return;

    // 先按下并登记抬键计划; 若 4 个槽位全满则原实现会"按下照发、抬键计划丢弃"
    // => 该键永远没有抬键 => 必卡. 故槽满时改为不发按下(点击被丢弃, 但不会卡).
    //
    // 槽位分配与 g_inj_mask 的置位在同一临界区内完成: 否则会与 clickTick(core0)
    // 的到期回收交错, 同一槽被两次占用或计划被覆盖。
    bool scheduled = false;
    if (s_btn_mtx && xSemaphoreTake(s_btn_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (auto &c : s_mouse_clicks) {
            if (!c.active) {
                c.bits = btn_bits;
                c.release_ms = millis() + (down_ms ? down_ms : 50);
                c.active = true;
                g_inj_mask |= btn_bits;
                scheduled = true;
                break;
            }
        }
        xSemaphoreGive(s_btn_mtx);
    }
    if (!scheduled) return;

    updateButtonState();
}

static void protoOnWheel(int8_t delta) {
    handleMouseWheel(delta);
}

static void protoOnPanic() {
    // 只清注入侧(真实鼠标掩码由真实设备负责); 若卡在 g_real_mask, 需物理按一下该键.
    setInjMaskLocked(0);
}

// 0x05 MOVE_CANCEL: 上位机停火时清空设备侧尚未发出的积压，
// 保证"松手即停"，而不是把之前攒下的位移继续吐出去。
// 另外置中断标志: 若此刻正在 handleMove 的拆分回放中, 会在两步之间停下来.
static void protoOnMoveCancel() {
    s_cancel_move = true;
    s_pending_dx.store(0, std::memory_order_relaxed);
    s_pending_dy.store(0, std::memory_order_relaxed);
    s_pending_wheel.store(0, std::memory_order_relaxed);
}

static void protoOnMove(int16_t dx, int16_t dy) {
    // 极速直通路径: 若USB就绪且无排队积压，直接内联输出，省去FreeRTOS线程唤醒与上下文切换(立省15~25微秒)
    if (isUsbReadyToTransfer() && s_pending_dx.load(std::memory_order_relaxed) == 0 && s_pending_dy.load(std::memory_order_relaxed) == 0) {
        handleMove(dx, dy);
        return;
    }

    s_pending_dx.fetch_add(dx, std::memory_order_relaxed);
    s_pending_dy.fetch_add(dy, std::memory_order_relaxed);
    if (mouseMoveTaskHandle != NULL) {
        xTaskNotifyGive(mouseMoveTaskHandle);
    }
}

static void protoOnMoveTo(int16_t x, int16_t y) {
    handleMoveto(x, y);
}

static void asciiLineCb(const uint8_t *data, size_t len) {
    char buf[512];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, data, len);
    while (len && (buf[len - 1] == ' ' || buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = 0;
    buf[len] = 0;
    if (len) {
        g_cmd_source = SRC_INJ;
        processCommand(buf);
    }
}

static void cbSetBaud(uint32_t baud) {
    if (baud >= 115200 && baud <= 6000000) {
        Serial0.end();
        vTaskDelay(pdMS_TO_TICKS(50));
        Serial0.begin(baud);
        Serial0.onReceive(serial0ISR);
    }
}

void protoInit() {
    s_tx0_mtx   = xSemaphoreCreateMutex();
    s_btn_mtx   = xSemaphoreCreateMutex();
    s_kbd_mtx   = xSemaphoreCreateMutex();

    g_proto.begin();
    g_proto.onMove = protoOnMove;
    g_proto.onMoveTo = protoOnMoveTo;
    g_proto.onButtonMask = protoOnButtonMask;
    g_proto.onClick = protoOnClick;
    g_proto.onWheel = protoOnWheel;
    g_proto.onPanic = protoOnPanic;
    g_proto.onAsciiLine = asciiLineCb;
    g_proto.onSetBaud = cbSetBaud;
    g_proto.onReboot = []() { ESP.restart(); };
    g_proto.onMoveCancel = protoOnMoveCancel;
    // 订阅成功立即补发一次当前按键态: 固件平时只在按键"变化"时才推 0x84，
    // 若上位机接入时用户已经按着键，不补发就会一直误判为未按下。
    g_proto.onSubAsync = [](bool on) {
        g_async_sub = on;
        if (on) updateButtonState();
    };
    g_proto.onKeyMask = handleKeyMaskCmd;
    g_proto.onKeyTap = handleKeyTapCmd;

    // ---- Serial1(真实鼠标链路)的二进制解析器 ----
    // 只注册"纯动作"命令。刻意不设 sendFrame, 也不注册 0x40/0x41 等会触发 ack() 的命令:
    // 设备往 Serial1 回写的任何字节都会被 fw_host 的 ASCII 帧解析器读走, 只会造成噪声。
    g_proto1.begin();
    g_proto1.onMove = [](int16_t dx, int16_t dy) {
        g_diagMoveRx++;
        // 与 ASCII km.move 的旁路条件完全一致(见 handleKmMoveCommand)
        if (isUsbReadyToTransfer() &&
            s_pending_dx.load(std::memory_order_relaxed) == 0 &&
            s_pending_dy.load(std::memory_order_relaxed) == 0) {
            handleMove(dx, dy);
            return;
        }
        s_pending_dx.fetch_add(dx, std::memory_order_relaxed);
        s_pending_dy.fetch_add(dy, std::memory_order_relaxed);
        if (mouseMoveTaskHandle != NULL) xTaskNotifyGive(mouseMoveTaskHandle);
    };
    g_proto1.onButtonMask = [](uint8_t mask) {
        g_diagBtnRx++;
        // 来源=真实鼠标, 与 ASCII km.left 系列走同一入口(含 s_btn_mtx 与看门狗覆盖)
        if (s_btn_mtx && xSemaphoreTake(s_btn_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
            g_real_mask = mask & 0x1F;
            xSemaphoreGive(s_btn_mtx);
        } else {
            return;
        }
        updateButtonState();
    };
    g_proto1.onWheel = [](int8_t d) { handleMouseWheel(d); };
    // 键盘透传: fw_host 收到真实键盘报文后发 0x23 KB_REPORT。
    // 走 real 侧, 与上位机注入(0x21)互不干扰 —— 见 kbdEmitMerged 的说明。
    // ★ 键盘原样转发: fw_host 送来的端点原始字节直接发 USB, 不解析不合并。
    //   用户实测(独立版)这套最流畅。
    g_proto1.onKbRaw = [](const uint8_t *raw, uint8_t len) {
        g_diagKbdRx++;
        kbdApplyRealRaw(raw, len);
    };

    g_proto1.onKbReport = [](uint8_t mod, const uint8_t keys6[6]) {
        g_diagKbdRx++;
        kbdApplyRealReport(mod, keys6);
    };

    // 键盘链路心跳 (0x23 + payload=[0xFF], 右板每 50ms 一发)。
    // 只更新时间戳, 【不动任何按键状态】—— 它只表达"链路活着"。
    // kbdTick 靠它区分"链路死了"和"用户一直按着", 见 KBD_LINK_TIMEOUT_MS。
    g_proto1.onKbHeartbeat = []() {
        g_diagKbdHbt++;
        s_kbd_link_last_ms = millis();
        s_kbd_link_seen    = true;
    };
    // 其余回调保持 nullptr: 绝对定位/点击/波特率/重启在实鼠标链路上都无意义,
    // onSetBaud 更是必须为空 —— 否则上位机可以经 Serial1 改掉 Serial0 的波特率。
    g_proto1.sendFrame = nullptr;
}

// ==========================================================================

bool processingUsbCommands = false;

// 串口接收缓冲: 必须容纳整行 JSON.
// 端点描述符每项约 166 字符, MAX_ENDPOINT_DESCRIPTORS=10 -> 最坏约 1700 字节,
// 原值 2048 勉强够, 但未知描述符的 data 字段可能更长, 故放宽到 16KB.
RingBuf<char, 16384> serial1RingBuffer;
int currentCommandIndex = 0;

int16_t mouseX = 0;
int16_t mouseY = 0;

const char *commandQueue[] = {
    "sendDeviceInfo",
    "sendDescriptorDevice",
    "sendEndpointDescriptors",
    "sendInterfaceDescriptors",
    "sendHidDescriptors",
    "sendIADescriptors",
    "sendEndpointData",
    "sendUnknownDescriptors",
    "sendDescriptorconfig",
    // 阶段 4: 取真设备的原始 HID 报告描述符, 用于报文格式克隆。
    // 放在最后: 前面几项是原厂软体也依赖的握手内容, 不能因为这一项
    // (新固件才支持)失败而影响它们。
    "sendRawHidDescriptors"
};

void handleSerial0Speed(const char *command);
void handleDebug(const char *command);
void handleEspLog(const char *command);
void handleKmMoveto(const char *command);
void handleKmGetpos(const char *command);
void handleKmMouseButton(const char *command);
void handleKmWheel(const char *command);
void handleUsbHello(const char *command);
void handleUsbGoodbye(const char *command);
void handleNoDevice(const char *command);
void handleDebugcommand(const char *command);

// ============================================================================
// USB_sendRawHidDescriptors:<iface>:<hex>
//
// fw_host 在拿到真设备的 HID 报告描述符后按接口逐行发来。这里是阶段 4
// (报文格式克隆)的入口: 把 hex 还原成字节, 交给 USBSetup 里的克隆逻辑。
// ============================================================================
void handleRawHidDescriptor(const char *command)
{
    static const char kPrefix[] = "USB_sendRawHidDescriptors:";
    const char *p = command;
    if (strncmp(p, kPrefix, sizeof(kPrefix) - 1) != 0) return;
    p += sizeof(kPrefix) - 1;

    // 解析接口号
    const uint8_t iface = (uint8_t)atoi(p);
    const char *colon = strchr(p, ':');
    if (!colon) return;

    receiveRealHidDescriptor(iface, colon + 1);
}

// km.mask(ms) / km.maskoff : 瞬时屏蔽真实键鼠输入(转发给右板执行)。
//
// 【为什么需要转发】屏蔽的执行点在 fw_host(右板): 只有它能看到真实鼠标键盘的
// HID 报文, 屏蔽就是在 onMouseMove/onKeyboard/onMouseButtons 里直接 return 掉。
// 但上位机(Apotheosis)的串口接在【本板(fw_device)的 Serial0】上 —— 本板从
// Serial0 收到的命令, 右板是看不到的。于是上位机发的 km.mask 落到本板命令表里
// 匹配不到任何表项, 被 handleDebugcommand() 静默丢弃, 屏蔽功能在 B 口完全不可用。
//
// 解决: 本板把 km.mask 原样经 Serial1(板间链路)转给右板, 由右板真正执行。
// 本板自己不保存任何屏蔽状态 —— 语义上"屏蔽"只对右板的真实输入判定有意义,
// 而注入走本板 Serial0, 不受影响(这正是需求: 屏蔽期间注入照常生效)。
//
// 应答: 右板执行后会回 "km.mask(N) ok"(见 fw_host/commands.cpp)。该行经 Serial1
// 回到本板, 本板不解析它(会落入 handleDebugcommand 丢弃), 不会污染 B 口输出。
void handleKmMask(const char *command)
{
    // 只转发本板确认合法的两种形式, 避免把任意文本灌进板间链路。
    // 右板侧同样是前缀匹配("km.mask(" / "km.maskoff"), 这里保持一致。
    if (strncmp(command, "km.maskoff", 10) == 0) {
        Serial1.println("km.maskoff");
        return;
    }
    if (strncmp(command, "km.mask(", 8) == 0) {
        Serial1.println(command);
    }
}

// km.reenum : 触发 USB 重枚举(被控机会看到设备拔出再插入)。
//
// 用途: 让被控机重新读取设备描述符(VID/PID/字符串在握手后可能被更新)。
// 副作用: CDC 串口断开约 1 秒 —— 调用方必须能容忍。
void handleKmReenum(const char *command)
{
    (void)command;
    cloneReenumerate();
    Serial0.println("km.reenum ok");
}

// km.cloneinfo : 回报真设备描述符的接收情况。
//
// 语义说明(避免误读): sawMouse/sawKbd 表示【收到了真设备的报告描述符并解析
// 成功】, 不代表被控机看到的描述符被替换了 —— 那在当前框架下做不到
// (原因见 USBSetup.cpp)。这些值用于确认握手链路正常、以及真设备报文的
// 格式特征(轴位宽/滚轮位置), 便于排查"方向/滚轮不对"这类问题。
void handleCloneInfo(const char *command)
{
    (void)command;
    Serial0.printf("km.cloneinfo sawMouse=%d sawKbd=%d "
                   "mouseRepLen=%u kbdRepLen=%u mouseXbits=%u mouseYbits=%u\n",
                   isCloneMouseActive() ? 1 : 0,
                   isCloneKbdActive() ? 1 : 0,
                   (unsigned)(mouseLayout() ? mouseLayout()->reportLen : 0),
                   (unsigned)(kbdLayout() ? kbdLayout()->reportLen : 0),
                   (unsigned)(mouseLayout() ? mouseLayout()->xBits : 0),
                   (unsigned)(mouseLayout() ? mouseLayout()->yBits : 0));
}

// Apotheosis 专属兼容指令
void handleKmButtons(const char *command);
void handleKmVersion(const char *command);
void handleKmMoveFmt(const char *command);
void handleRawHidDescriptor(const char *command);
void handleKmReenum(const char *command);
void handleCloneInfo(const char *command);
void handleKmMask(const char *command);

CommandEntry serial0CommandTable[] = {
    {"DEBUG_", handleDebug},
    {"SERIAL_", handleSerial0Speed}
};

CommandEntry debugCommandTable[] = {
    {"ESPLOG_", handleEspLog},
    {"PRINT_Parsed_Descriptors", printParsedDescriptors},
    {"HID_Descriptors", [](const char* arg) { Serial1.print(arg); }}
};

// 注意: 表项靠前缀匹配, 长词必须排在短词前面。
// "km.move" 是 "km.moveto" 的前缀, 顺序颠倒会让 moveto 被当成相对位移执行。
// cmdEntryMatches() 已加词边界保护, 这里再显式排序做双保险。
CommandEntry normalCommandTable[] = {
    {"km.moveto", handleKmMoveto},
    {"km.movefmt", handleKmMoveFmt},   // 必须排在 "km.move" 之前(更长的前缀优先)
    {"km.move", handleKmMoveCommand},
    {"km.getpos", handleKmGetpos},
    {"km.buttons", handleKmButtons},
    {"km.version", handleKmVersion},
    {"km.left(1)",   handleKmMouseButton},
    {"km.left(0)",   handleKmMouseButton},
    {"km.right(1)",  handleKmMouseButton},
    {"km.right(0)",  handleKmMouseButton},
    {"km.middle(1)", handleKmMouseButton},
    {"km.middle(0)", handleKmMouseButton},
    {"km.side1(1)",  handleKmMouseButton},
    {"km.side1(0)",  handleKmMouseButton},
    {"km.side2(1)",  handleKmMouseButton},
    {"km.side2(0)",  handleKmMouseButton},
    {"km.wheel", handleKmWheel},
    // 描述符克隆: 触发一次 USB 重枚举, 让被控机重新读取(克隆后的)描述符。
    // 副作用: CDC 串口会断开约 1 秒 —— 调用方(上位机)必须能容忍。
    {"km.reenum", handleKmReenum},
    {"km.cloneinfo", handleCloneInfo}
};

// 瞬时屏蔽真实输入: 本板不执行, 只经 Serial1 转发给右板(fw_host)执行。
// 单独成表而不是放进 normalCommandTable —— 后者被 !processingUsbCommands 门控,
// 握手期间发的屏蔽命令会被丢弃(详见 processCommand 里的说明)。
CommandEntry maskCommandTable[] = {
    {"km.maskoff", handleKmMask},
    {"km.mask(",   handleKmMask}     // 以 '(' 结尾 -> 纯前缀匹配, 可带参数
};

CommandEntry usbCommandTable[] = {
    {"USB_HELLO", handleUsbHello},
    {"USB_GOODBYE", handleUsbGoodbye},
    {"USB_ISNULL", handleNoDevice},
    {"USB_sendDeviceInfo:", receiveDeviceInfo},
    {"USB_sendDescriptorDevice:", receiveDescriptorDevice},
    {"USB_sendEndpointDescriptors:", receiveEndpointDescriptors},
    {"USB_sendInterfaceDescriptors:", receiveInterfaceDescriptors},
    {"USB_sendHidDescriptors:", receiveHidDescriptors},
    {"USB_sendRawHidDescriptors:", handleRawHidDescriptor},
    {"USB_sendIADescriptors:", receiveIADescriptors},
    {"USB_sendEndpointData:", receiveEndpointData},
    {"USB_sendUnknownDescriptors:", receiveUnknownDescriptors},
    {"USB_sendDescriptorconfig:", receivedescriptorConfiguration}
};

void processCommand(const char *command);

void trimCommand(char* command) {
    int len = strlen(command);
    while (len > 0 && (command[len - 1] == ' ' || command[len - 1] == '\n' || command[len - 1] == '\r')) {
        command[len - 1] = '\0';
        len--;
    }
}

void serial0RX() {
    uint8_t buf[128];
    while (true) {
        int avail = Serial0.available();
        if (avail <= 0) break;
        int to_read = (avail > (int)sizeof(buf)) ? (int)sizeof(buf) : avail;
        int n = Serial0.read(buf, to_read);
        if (n > 0) {
            g_proto.feed(buf, n);
        } else {
            break;
        }
    }
}

void serial1RX() {
    while (Serial1.available() > 0) {
        char byte = Serial1.read();

        // ★ 不再用 FW_DIAG 守卫: 这是排查"右板到底有没有发"的关键数据,
        //   每次都要看。开销只是两个自增, 可忽略。
        g_diag_rx1_bytes++;
        {
            uint32_t sl = g_diag_snap_len;
            if (sl < 64) { g_diag_snap[sl] = (uint8_t)byte; g_diag_snap_len = sl + 1; }
        }

        if (byte == '\r') {
            continue;
        }

        // ---- 帧内: 全部交给 g_proto1, 不进 ASCII 环 ----
        if (s_s1_bin_state == S1_BIN_FRAME) {
            g_proto1.feed((const uint8_t *)&byte, 1);
            // 关键: 只要解析器已经消费完整帧(回到 WAIT_HEADER)就立刻回到行首状态,
            // 不能靠等 '\n' 退出 —— 帧体完全可以包含 0x0A(PAYLOAD 或 CRC 高字节),
            // 而若一直不退出, 后续所有 ASCII 数据都会被当成帧体吞掉(包括 km.left 抬键)。
            if (g_proto1.atFrameBoundary()) {
                s_s1_bin_state = S1_BIN_IDLE;
                s_s1_stage_n = 0;
                s_s1_skip = 0;
            }
            continue;
        }

        bool asciiPush = true;   // 本字节最终是否要进 ASCII 环
        bool lineComplete = false;

        // ---- 行首 2 字节前瞻: 确认是不是帧头 ----
        //
        // 探测条件从 "g_serial1_binary_moves" 放宽为 "协商过二进制位移 或
        // 收到过键盘帧"。原因: 键盘帧(0x23 KB_REPORT)是【无条件】以二进制
        // 发出的, 不依赖 km.movefmt 协商 —— 因为键盘报文没有 ASCII 等价形式
        // (0x23 帧携带 6 字节键码快照, 用文本表达又长又要 sscanf 解析)。
        //
        // 若仍然只在 g_serial1_binary_moves 时才探测, 未协商时整个 0x23 帧的
        // 字节会被当成 ASCII 文本灌进环缓冲 —— 键盘静默失效, 还会把
        // A5/5C 之类的非文本字节污染进命令解析器。
        //
        // 注意: 探测本身是无害的。它只在前瞻到 "A5 5C" / "DE AD" 魔数时才吃字节,
        // 普通 ASCII 行(km.left 等)照常进环, 因此对旧上位机完全兼容。
        if (s_s1_skip == 0 && s_s1_stage_n < 2) {
            s_s1_stage[s_s1_stage_n++] = (uint8_t)byte;
            asciiPush = false;                  // 先扣住, 等判定结果

            if (s_s1_stage_n == 1) {
                if (byte == '\n') {
                    // 空行: 直接按 ASCII 收尾
                    asciiPush = true;
                } else if ((uint8_t)byte == 0xA5 || (uint8_t)byte == 0xDE) {
                    continue;                   // 魔数候选, 等第二字节
                } else {
                    asciiPush = true;           // 首字节不是候选 -> 本行按 ASCII
                    s_s1_skip = 1;
                }
            } else { /* s_s1_stage_n == 2 */
                const uint8_t b0 = s_s1_stage[0], b1 = s_s1_stage[1];
                if ((b0 == 0xA5 && b1 == 0x5C) || (b0 == 0xDE && b1 == 0xAD)) {
                    s_s1_bin_state = S1_BIN_FRAME;
                    s_s1_stage_n = 0;
                    s_s1_skip = 0;                  // 丢弃半截 ASCII 行的行状态
                    g_proto1.feed(s_s1_stage, 2);   // 帧头交给解析器
                    continue;
                }
                // 不是帧头: 两个暂存字节原样回放给 ASCII(不丢数据), 本行按 ASCII 走
                s_s1_skip = 1;
                for (uint8_t k = 0; k < 2; ++k) {
                    if (!serial1RingBuffer.isFull()) serial1RingBuffer.push((char)s_s1_stage[k]);
                }
                s_s1_stage_n = 0;
                if (b1 != '\n') {
                    continue;                   // 本字节已在回放里处理完
                }
                lineComplete = true;            // b1 是换行: 回放已收行, 这里只做收尾
            }
        }

        if (!lineComplete) {
            if (asciiPush) {
                if (!serial1RingBuffer.isFull()) {
                    serial1RingBuffer.push(byte);
                }
                if (byte != '\n') continue;
            } else {
                continue;
            }
        }

        {
            s_s1_bin_state = S1_BIN_IDLE;
            s_s1_stage_n = 0;
            s_s1_skip = 0;
#if FW_DIAG
            g_diag_rx1_lines++;
#endif
            // static: serial1Task 栈只有 4096 字节, 不能用栈上大数组.
            // 原值 620 会把端点描述符 JSON(>619 字节)截断, 导致 deserializeJson 失败,
            // 而所有 receive* 回调在解析失败时都直接 return, 握手就此永久卡死.
            static char commandBuffer[8192];
            int commandIndex = 0;

            while (!serial1RingBuffer.isEmpty() && commandIndex < sizeof(commandBuffer) - 1) {
                char c;
                serial1RingBuffer.pop(c);
                if (c == '\n') break;
                commandBuffer[commandIndex++] = c;
            }

            commandBuffer[commandIndex] = '\0';
#if FW_DIAG
            if ((uint32_t)commandIndex > g_diag_max_line) g_diag_max_line = (uint32_t)commandIndex;
#endif
            trimCommand(commandBuffer);

            // ===== 右板诊断转发 (不再用 FW_DIAG 守卫) =====
            //
            // 【为什么生产版也开】
            //   右板是焊死的整板,【没有 CH343】, 它唯一的观测通道就是把状态行
            //   经 Serial1 发给本板, 再由本板的 CH343 出去。之前只有 FW_DIAG
            //   构建才转发, 于是排查右板问题时两眼一抹黑。
            //
            // 【安全性】
            //   只转发以固定前缀开头的行:
            //     "#HBT"  -> 只计数, 不输出(避免刷屏)
            //     "DIAG|" -> 原样输出
            //   这些都不是 km.* 命令, 上位机 Apotheosis 的 probeAscii 只等特定
            //   应答串, 对无关 ASCII 行直接忽略, 因此不会污染协议。
            if (strcmp(commandBuffer, "#HBT") == 0) {
                g_diag_hbt_count++;
                continue;
            }
            if (strncmp(commandBuffer, "DIAG|", 5) == 0) {
                Serial0.println(commandBuffer);
                continue;               // 诊断行不当作命令解析
            }

            g_cmd_source = SRC_REAL;

            if (strncmp(commandBuffer, "km.move", 7) == 0) {
                handleKmMoveCommand(commandBuffer);
            } else if (commandIndex > 0) {
                processCommand(commandBuffer);
            }
        }
    }
}

// 位移指令：使用原子累加队列，双路安全合并，绝不覆盖，绝不丢步
void handleKmMoveCommand(const char *command) {
    int x = 0, y = 0;

    // 解析 "km.move(x,y)" 或 "km.move(x,y,segments)"
    const char *p = strchr(command, '(');
    if (!p) return;

    if (sscanf(p + 1, "%d,%d", &x, &y) < 2) {
        return;
    }

    // 极速直通路径: 与二进制 0x01 的 protoOnMove 对齐。
    //
    // docs/proto.md §4.1 指出 "ASCII km.move 路径没有这个旁路, 恒定走任务通知",
    // 这正是本项要消除的延迟差: 实鼠标链路(fw_host -> Serial1)发的就是 km.move,
    // 每个报表都要多一次 xTaskNotifyGive + 任务唤醒 + 上下文切换。
    // 这里补齐同样的旁路条件(USB 就绪 且 无待发积压), 让 ASCII 路径享受同等延迟。
    //
    // 安全性: s_pending_wheel 不参与判定, 因为 handleMove 只处理 dx/dy;
    // 若调用瞬间 USB 变为忙, handleMove 内部会自行把余额塞回 s_pending_* 并通知任务,
    // 因此这条旁路不会丢步。
    if (isUsbReadyToTransfer() &&
        s_pending_dx.load(std::memory_order_relaxed) == 0 &&
        s_pending_dy.load(std::memory_order_relaxed) == 0) {
        handleMove(x, y);
        return;
    }

    s_pending_dx.fetch_add(x, std::memory_order_relaxed);
    s_pending_dy.fetch_add(y, std::memory_order_relaxed);

    if (mouseMoveTaskHandle != NULL) {
        xTaskNotifyGive(mouseMoveTaskHandle);
    }
}

void handleUsbHello(const char *command) {
    deviceConnected = true;
    processingUsbCommands = true;
    currentCommandIndex = 0;
    sendNextCommand();
}

void handleUsbGoodbye(const char *command) {
    Serial0.println("USB Device disconnected. Resetting state!");
    deviceConnected = false;
    processingUsbCommands = false;
    // 真实鼠标侧也一并清空: 原来只清 g_inj_mask, 真实鼠标按着键时被拔出
    // 会留下 g_real_mask 的 bit, 由于 merged = real | inj, 0x44 PANIC 也救不回来.
    clearAllButtonsLocked();
}

void sendNextCommand() {
    if (!processingUsbCommands || currentCommandIndex >= sizeof(commandQueue) / sizeof(commandQueue[0])) {
        return;
    }
    const char *command = commandQueue[currentCommandIndex];
    Serial1.println(command);
    currentCommandIndex++;
    if (currentCommandIndex >= sizeof(commandQueue) / sizeof(commandQueue[0])) {
        processingUsbCommands = false;
        InitUSB();
        vTaskDelay(100);
        Serial1.println("USB_INIT");
    }
}

// 支持带 Track ID 格式: "cmd#<id>" 回复 ">>> #<id>:<result>\n"
// 全部 Serial0 输出共用 s_tx0_mtx, 防止与 0x84 异步帧在字节级交错.
static void sendTrackedResponse(const char *origCommand, const String &result) {
    if (!s_tx0_mtx || xSemaphoreTake(s_tx0_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;
    const char *hash = strchr(origCommand, '#');
    if (hash) {
        int cmdId = atoi(hash + 1);
        Serial0.print(">>> #");
        Serial0.print(cmdId);
        Serial0.print(":");
        Serial0.println(result);
    } else {
        Serial0.println(result);
    }
    xSemaphoreGive(s_tx0_mtx);
}

void handleKmButtons(const char *command) {
    int mode = -1;
    const char *paren = strchr(command, '(');
    if (paren && sscanf(paren + 1, "%d", &mode) == 1) {
        g_makcu_buttons_enabled = (mode != 0);
        // 与 0x48 同理: 开启单字节掩码流后立刻补一帧当前按键态
        if (g_makcu_buttons_enabled) updateButtonState();
        sendTrackedResponse(command, "OK");
    } else {
        uint8_t currentMask;
        if (s_btn_mtx && xSemaphoreTake(s_btn_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
            currentMask = mergedMaskLocked() & 0x1F;
            xSemaphoreGive(s_btn_mtx);
        } else {
            currentMask = 0;
        }
        sendTrackedResponse(command, String(currentMask));
    }
}

void handleKmVersion(const char *command) {
    sendTrackedResponse(command, "MAKCU-PASSTHROUGH-1.0.0");
}

// km.movefmt(1|0) / km.movefmt
//
// 让主机查询/切换 Serial1 的位移帧格式: 0=ASCII km.move(默认), 1=二进制 0x01 MOVE。
// 它是切换的"握手点": 主机只有在收到 >>> #id:OK 之后才会开始发二进制帧。
// 旧固件不认识这条命令 -> 静默忽略 -> 主机超时 -> 回落 ASCII, 因此新主机 + 旧设备不会断链。
void handleKmMoveFmt(const char *command) {
    int mode = -1;
    const char *paren = strchr(command, '(');
    if (paren && sscanf(paren + 1, "%d", &mode) == 1) {
        g_serial1_binary_moves = (mode != 0);
        if (g_serial1_binary_moves) {
            // 切进二进制前清掉半截 ASCII 行状态, 避免把残留字节误判成帧
            s_s1_bin_state = S1_BIN_IDLE;
            s_s1_stage_n = 0;
            s_s1_skip = 0;
        }
        sendTrackedResponse(command, "OK");
    } else {
        sendTrackedResponse(command, g_serial1_binary_moves ? "1" : "0");
    }
}

// 前缀匹配 + 词边界保护。
//
// 原实现只做 strncmp 前缀匹配, 命中结果由表项顺序决定, 于是:
//   normalCommandTable 里 {"km.move"} 排在 {"km.moveto"} 之前,
//   而 "km.moveto(100,200)" 的前 7 个字符恰好就是 "km.move"
//   -> 绝对定位被 handleKmMoveCommand 当成【相对位移】执行: 不是"移到(100,200)",
//      而是"再走(100,200)", 静默产生完全错误的动作, 且 handleKmMoveto 永远不可达。
//      上位机 km.getpos + km.moveto 的绝对瞄准链路因此全废。
//
// 规则: 表项以字母/数字结尾时(它本身是一个完整单词), 要求命令紧随其后的字符是
// 分隔符(串尾 / '#' / 空格)或参数起始符 '(' —— 短词就不会吞并长词。
// 以非字母数字结尾的表项(DEBUG_ / SERIAL_ / ESPLOG_ / km.left(1) / USB_sendXxx:)
// 保持纯前缀匹配, 它们本来就靠续接字符区分。
static bool cmdEntryMatches(const char *command, const char *entry) {
    size_t n = strlen(entry);
    if (n == 0 || strncmp(command, entry, n) != 0) return false;

    const char last = entry[n - 1];
    const bool entryIsWord = (last >= '0' && last <= '9') ||
                             (last >= 'A' && last <= 'Z') ||
                             (last >= 'a' && last <= 'z');
    if (!entryIsWord) return true;

    const char next = command[n];
    return next == '\0' || next == '(' || next == '#' || next == ' ' || next == '\t';
}

void processCommand(const char *command) {
    for (const auto &entry : debugCommandTable) {
        if (cmdEntryMatches(command, entry.command)) {
            entry.handler(command);
            return;
        }
    }

    for (const auto &entry : serial0CommandTable) {
        if (cmdEntryMatches(command, entry.command)) {
            entry.handler(command);
            return;
        }
    }

    for (const auto &entry : usbCommandTable) {
        if (cmdEntryMatches(command, entry.command)) {
            entry.handler(command);
            return;
        }
    }

    // 屏蔽命令必须在 processingUsbCommands 为 true(握手进行中)时也能命中。
    //
    // normalCommandTable 被 !processingUsbCommands 门控(见下方), 而握手期间
    // 上位机完全可能发 km.mask —— 若把它放进 normalCommandTable, 那次屏蔽会被
    // 静默丢弃, 表现为"偶尔屏蔽失灵"。它只是往 Serial1 转发一行文本, 与握手
    // 状态无关, 因此提前到门控之外。km.movefmt 有同样的门控问题, 但那是
    // 右板主动协商、且刻意在握手后才发(见 fw_host/esp_tasks.cpp:283), 维持原样。
    for (const auto &entry : maskCommandTable) {
        if (cmdEntryMatches(command, entry.command)) {
            entry.handler(command);
            return;
        }
    }

    if (!processingUsbCommands) {
        for (const auto &entry : normalCommandTable) {
            if (cmdEntryMatches(command, entry.command)) {
                entry.handler(command);
                return;
            }
        }
    }

    handleDebugcommand(command);
}

void handleEspLog(const char *command) {
    const char *message = command + strlen("ESPLOG_");
    if (strlen(message) > 0) {
        Serial0.println(message);
    }
}

void handleSerial0Speed(const char *command) {
    int speed;
    if (sscanf(command + strlen("SERIAL_"), "%d", &speed) == 1) {
        if (speed >= 115200 && speed <= 6000000) {
            Serial0.print("Setting Serial0 speed to: ");
            Serial0.println(speed);
            Serial0.end();
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            Serial0.begin(speed);
            Serial0.onReceive(serial0ISR);
            Serial0.println("Serial0 speed change successful.");
        }
    }
}

void handleDebug(const char *command) {
    int debugLevel;
    if (strcmp(command, "DEBUG_ON") == 0) {
        Serial1.println("DEBUG_ON");
    } else if (strcmp(command, "DEBUG_OFF") == 0) {
        Serial1.println("DEBUG_OFF");
    } else if (sscanf(command + strlen("DEBUG_"), "%d", &debugLevel) == 1) {
        Serial1.println(command);
    }
}

void handleDebugcommand(const char *command) {
    // 忽略普通调试回显
}

void handleNoDevice(const char *command) {
    deviceConnected = false;
}

// 专职非阻塞位移派发任务：无锁原子交换，零丢步
void mouseMoveTask(void *pvParameters) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));

        int32_t x = s_pending_dx.exchange(0, std::memory_order_relaxed);
        int32_t y = s_pending_dy.exchange(0, std::memory_order_relaxed);

        if (x != 0 || y != 0) {
            handleMove(x, y);
        }

        // 滚轮同样需要积压兜底: 位移发不出去会排队，滚轮以前是直接丢弃
        int32_t w = s_pending_wheel.exchange(0, std::memory_order_relaxed);
        if (w != 0) {
            if (isUsbReadyToTransfer()) {
                while (w != 0) {
                    int step = (w > 127) ? 127 : ((w < -127) ? -127 : w);
                    emitMouseWheel(static_cast<int8_t>(step));
                    w -= step;
                }
            } else {
                s_pending_wheel.fetch_add(w, std::memory_order_relaxed);
            }
        }
    }
}

void handleKmMoveto(const char *command) {
    // 与 handleKmMoveCommand 保持一致: 必须初始化并检查 sscanf 返回值。
    // 原实现直接 `int x, y; sscanf(...)` 不检查 —— 收到 "km.moveto"(无参数)
    // 或格式错误时 sscanf 返回 0, x/y 保持未初始化, 随后 handleMoveto 用垃圾值
    // 算相对位移, 产生一次幅度不可预测的注入, 且属未定义行为。
    int x = 0, y = 0;
    if (sscanf(command + strlen("km.moveto") + 1, "%d,%d", &x, &y) < 2) return;
    handleMoveto(x, y);
}

void handleKmGetpos(const char *command) {
    // 与 km.version / km.buttons 保持一致: 支持 "km.getpos#<id>" 的 >>> #<id>: 追踪应答。
    // 原来直接走 handleGetPos() 的 Serial0.println, 会丢掉 track id, 上位机无法把
    // 应答与请求配对(高频查询绝对坐标时尤其致命)。
    int16_t px, py;
    if (s_btn_mtx && xSemaphoreTake(s_btn_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        px = mouseX; py = mouseY;
        xSemaphoreGive(s_btn_mtx);
    } else {
        px = 0; py = 0;
    }
    sendTrackedResponse(command,
        "km.pos(" + String(px) + "," + String(py) + ")");
}

// 按键命令统一入口。
//
// 原来 km.left/right/middle/side1/side2 的按下与抬起各写一个函数, 共 10 个
// 只差 (掩码, 状态) 两个常量的孪生函数。现合并为一个: 从命令串自身解析出
// 位掩码与目标状态, 行为与原实现逐字节一致。
//
// 识别依据是命令里的方位词与括号内的 0/1, 与表项字面量一一对应:
//   km.left(1) right(0) middle(1) side1(0) side2(1) ...
void handleKmMouseButton(const char *command) {
    uint8_t bit;
    if      (strncmp(command, "km.left",   7) == 0) bit = 0x01;
    else if (strncmp(command, "km.right",  8) == 0) bit = 0x02;
    else if (strncmp(command, "km.middle", 9) == 0) bit = 0x04;
    else if (strncmp(command, "km.side1",  8) == 0) bit = 0x08;
    else if (strncmp(command, "km.side2",  8) == 0) bit = 0x10;
    else return;

    const char *paren = strchr(command, '(');
    const bool down = (paren != nullptr) && (*(paren + 1) == '1');
    applyBtn(bit, down);
}

void handleKmWheel(const char *command) {
    // 同样必须初始化 + 检查返回值(原实现未初始化读, 见 handleKmMoveto 的说明)。
    int wheelMovement = 0;
    if (sscanf(command + strlen("km.wheel") + 1, "%d", &wheelMovement) != 1) return;
    handleMouseWheel(wheelMovement);
}

// 把已发出的位移记账到虚拟坐标. 所有对 mouseX/mouseY 的修改都走这里, 避免丢失更新.
static void addVirtualPos(int32_t dx, int32_t dy) {
    if (!s_btn_mtx || xSemaphoreTake(s_btn_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return;
    mouseX = (int16_t)(mouseX + dx);
    mouseY = (int16_t)(mouseY + dy);
    xSemaphoreGive(s_btn_mtx);
}

// 核心非阻塞安全位移输出
//
// 修复要点(原实现的三个缺陷):
//  1) 记账口径: 原来无论实际发出多少都写 mouseX += x。Mouse.move() 会阻塞等主机取走报文,
//     一个 USB 轮询周期只能吃一帧, 所以 x=1000 拆成 8 步时实际生效远少于 8 步,
//     而坐标却按 1000 记账 => km.getpos/km.moveto 的绝对瞄准链静默失准。
//     现在只累加"已发出"的步进量。
//  2) 不可中断: 拆分回放跑到一半无法停下, 于是 0x05 MOVE_CANCEL 的"松手即停"失效。
//     现在每步之间检查 s_cancel_move。
//  3) 阻塞: 进入 Mouse.move() 前先过 isUsbReadyToTransfer() 闸门, 不满足就把余额还给
//     队列交给 mouseMoveTask, 避免在 Core1 高优先级任务里做最长 100ms 的忙等。
void handleMove(int x, int y) {
    // 进入本次输出: 清掉可能在排队期间就已置位的停火标志
    s_cancel_move = false;

    if (!isUsbReadyToTransfer()) {
        s_pending_dx.fetch_add(x, std::memory_order_relaxed);
        s_pending_dy.fetch_add(y, std::memory_order_relaxed);
        if (mouseMoveTaskHandle != NULL) xTaskNotifyGive(mouseMoveTaskHandle);
        return;
    }

    int32_t sentX = 0, sentY = 0;

    if (x >= -127 && x <= 127 && y >= -127 && y <= 127) {
        // ★ 只有真正发出去才记账; 发不出去就还回队列, 由 mouseMoveTask 稍后重发
        if (emitMouseMove(static_cast<int8_t>(x), static_cast<int8_t>(y))) {
            sentX = x; sentY = y;
        } else {
            s_pending_dx.fetch_add(x, std::memory_order_relaxed);
            s_pending_dy.fetch_add(y, std::memory_order_relaxed);
            if (mouseMoveTaskHandle != NULL) xTaskNotifyGive(mouseMoveTaskHandle);
            return;
        }
    } else {
        int32_t remainingX = x, remainingY = y;
        while ((remainingX != 0 || remainingY != 0)) {
            if (!isUsbReadyToTransfer()) {
                // USB 忙: 余额还给队列, 不再阻塞
                s_pending_dx.fetch_add(remainingX, std::memory_order_relaxed);
                s_pending_dy.fetch_add(remainingY, std::memory_order_relaxed);
                if (mouseMoveTaskHandle != NULL) xTaskNotifyGive(mouseMoveTaskHandle);
                break;
            }
            if (s_cancel_move) {
                // 上位机停火: 丢弃剩余余额, 让"松手即停"真正生效
                s_cancel_move = false;
                break;
            }
            int8_t stepX = (remainingX > 127) ? 127 : ((remainingX < -127) ? -127 : (int8_t)remainingX);
            int8_t stepY = (remainingY > 127) ? 127 : ((remainingY < -127) ? -127 : (int8_t)remainingY);
            if (!emitMouseMove(stepX, stepY)) {
                // ★ 发不出去: 把剩下的余额(含本步)全还给队列, 不记账
                s_pending_dx.fetch_add(remainingX, std::memory_order_relaxed);
                s_pending_dy.fetch_add(remainingY, std::memory_order_relaxed);
                if (mouseMoveTaskHandle != NULL) xTaskNotifyGive(mouseMoveTaskHandle);
                break;
            }
            sentX += stepX; sentY += stepY;
            remainingX -= stepX; remainingY -= stepY;
        }
    }

    // 只记账真正发出的部分
    if (sentX != 0 || sentY != 0) addVirtualPos(sentX, sentY);
}

void handleMoveto(int x, int y) {
    int16_t curX, curY;
    if (s_btn_mtx && xSemaphoreTake(s_btn_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        curX = mouseX; curY = mouseY;
        xSemaphoreGive(s_btn_mtx);
    } else {
        return;
    }
    handleMove(x - curX, y - curY);
}

void handleMouseWheel(int wheelMovement) {
    if (wheelMovement == 0) return;

    // 快路径: USB 就绪且无积压时直接发，零任务唤醒。
    // 注意: 必须把超出一帧的余量交回队列 —— 原实现在 |w|>127 时 clamp 后直接 return,
    // 余量被静默丢弃(km.wheel(300) 只发出 127, 丢 173)。
    if (isUsbReadyToTransfer() && s_pending_wheel.load(std::memory_order_relaxed) == 0) {
        int step = (wheelMovement > 127) ? 127 : ((wheelMovement < -127) ? -127 : wheelMovement);
        if (!emitMouseWheel(static_cast<int8_t>(step))) {
            // ★ 发不出去 -> 整份(含本步)还回队列
            s_pending_wheel.fetch_add(wheelMovement, std::memory_order_relaxed);
            if (mouseMoveTaskHandle != NULL) xTaskNotifyGive(mouseMoveTaskHandle);
            return;
        }
        int32_t rest = wheelMovement - step;
        if (rest != 0) {
            s_pending_wheel.fetch_add(rest, std::memory_order_relaxed);
            if (mouseMoveTaskHandle != NULL) xTaskNotifyGive(mouseMoveTaskHandle);
        }
        return;
    }
    // 慢路径: 排队等 mouseMoveTask 回放，不再静默丢弃
    s_pending_wheel.fetch_add(wheelMovement, std::memory_order_relaxed);
    if (mouseMoveTaskHandle != NULL) {
        xTaskNotifyGive(mouseMoveTaskHandle);
    }
}
