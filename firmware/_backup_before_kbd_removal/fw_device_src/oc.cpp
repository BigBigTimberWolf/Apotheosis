// ============================================================
// oc.cpp - CPU超频管理器实现
// ============================================================
#include "oc.h"
#include <Arduino.h>
#include <esp_system.h>
// ESP32-S3 Arduino核心提供:
#include <esp32-hal-cpu.h>

namespace oc {

static constexpr uint32_t SAFE_MHZ   = 240;
static constexpr uint32_t DEFAULT_OC = 260;
static constexpr uint32_t RTC_MAGIC  = 0x4D414B44; // "MAKD" v2: 换版本即作废旧状态

// .noinit段: 软复位(看门狗/panic)不清零, 断电清零 — 正好做崩溃计数
//
// 修复: 原实现里 crash_count 永远不可能达到 2 —— 它在 30s 后被 confirmStable()
// 无条件清零, 而任何一次异常复位都发生在 30s 之后(上电 30s 内就崩属于极少数),
// 于是"连续崩 2 次就降档"的保护形同虚设。改用 boot_in_progress 标志判定:
// 每次启动置位, 只有走完 30s 自检才清除; 启动时看到它还置着, 就说明上次是异常复位。
struct OcState {
    uint32_t magic;
    uint8_t  crash_count;
    uint8_t  enabled;
    uint8_t  boot_in_progress;
    uint8_t  _pad;
    uint32_t target_mhz;
};

static OcState s_state __attribute__((section(".noinit"), used));

static OcState& state() { return s_state; }

static void init_state() {
    if (state().magic != RTC_MAGIC) {
        state().magic = RTC_MAGIC;
        state().crash_count = 0;
        state().enabled = 1;              // 默认允许超频
        state().boot_in_progress = 0;
        state().target_mhz = DEFAULT_OC;
    }
    // .noinit 内容是未定义的, 顺手夹一次范围, 防止脏值被当成真频率喂给 setCpuFrequencyMhz
    if (state().target_mhz < 80 || state().target_mhz > 320) {
        state().target_mhz = DEFAULT_OC;
    }
}

bool apply(uint32_t mhz) {
    if (mhz < 80)  mhz = 80;
    if (mhz > 320) mhz = 320;
    setCpuFrequencyMhz(mhz);
    delay(5);
    return getCpuFrequencyMhz() == mhz;
}

void bootSelfTest() {
    init_state();

    // 上次启动没走完自检 => 异常复位(看门狗/panic)
    const bool booted_clean = (state().boot_in_progress == 0);
    if (!booted_clean) {
        if (state().crash_count < 0xFF) state().crash_count++;
    }
    state().boot_in_progress = 1;         // 本次启动开始记账, 由 confirmStable() 清除

    if (state().crash_count >= 2) {
        // 连续两轮异常复位 -> 永久降回安全档
        state().enabled = 0;
        state().crash_count = 0;
        apply(SAFE_MHZ);
        Serial1.printf("[OC] unstable (%u faults), fallback to %luMHz, OC disabled\n",
                       (unsigned)state().crash_count, (unsigned long)SAFE_MHZ);
        return;
    }

    if (!state().enabled) {
        apply(SAFE_MHZ);
        return;
    }

    // 刚崩过的这一轮不升频, 让系统先在安全频率下走完自检再谈超频
    if (!booted_clean) {
        apply(SAFE_MHZ);
        Serial1.printf("[OC] previous boot did not finish selftest, staying at %luMHz (faults=%u)\n",
                       (unsigned long)SAFE_MHZ, (unsigned)state().crash_count);
        return;
    }

    if (state().target_mhz > SAFE_MHZ) {
        if (apply(state().target_mhz)) {
            Serial1.printf("[OC] running @ %lu MHz\n", (unsigned long)getCpuFrequencyMhz());
        } else {
            state().enabled = 0;
            apply(SAFE_MHZ);
            Serial1.println("[OC] target freq not achievable, disabled");
        }
    }
}

void confirmStable() {
    init_state();
    if (state().crash_count != 0) state().crash_count = 0;
    state().boot_in_progress = 0;         // 本轮启动已走完自检
}

uint32_t currentMhz() { return (uint32_t)getCpuFrequencyMhz(); }

bool enabled() { init_state(); return state().enabled != 0; }

void setEnabled(bool on, uint32_t target_mhz) {
    init_state();
    state().enabled = on ? 1 : 0;
    state().target_mhz = target_mhz;
    state().crash_count = 0;
    apply(on ? target_mhz : SAFE_MHZ);
}

} // namespace oc
