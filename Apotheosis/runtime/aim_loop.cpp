#include "runtime/aim_loop.h"

#include "control/aim_controller.h"
#include "control/sensitivity_calibrator.h"

#include "mouse/aim_path.h"
#include "mouse/auto_stop.h"
#include "mouse/trigger_fsm.h"
#include "mouse/trigger_scope.h"

#include "Apotheosis.h"   // 设备指针 (makcuSerial / makcuNewSerial / kmboxNetSerial)
#include "config/config.h"
#include "crosshair/crosshair_runtime.h"
#include "detector/detection_buffer.h"
#include "mouse/mouse.h"
#include "runtime/active_hotkey.h"
#include "runtime/config_snapshot.h"
#include "runtime/latency_probe.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>   // GetAsyncKeyState —— 自动急停读物理方向键
#endif

namespace runtime::aim_loop
{

namespace
{

std::mutex g_mtx;
std::unique_ptr<control::AimController> g_controller;

std::unique_ptr<MouseThread> g_mouse;
std::mutex g_mouse_mtx;

MouseThread* ensureMouse()
{
    std::lock_guard<std::mutex> lk(g_mouse_mtx);
    if (g_mouse)
        return g_mouse.get();

    MouseRuntimeParams params;
    {
        const auto snap = runtime_config::read();
        params.detection_resolution = snap ? snap->detection_resolution : 320;
    }
    MakcuConnection* makcu = nullptr;
    MakcuNewConnection* makcuNew = nullptr;
    MakcuNewConnection* makcuNewKbd = nullptr;
    KmboxNetConnection* kmboxNet = nullptr;
    {
        std::lock_guard<std::mutex> lkDev(inputDeviceMutex);
        makcu = makcuSerial;
        makcuNew = makcuNewSerial;
        makcuNewKbd = makcuNewSerialKbd;
        kmboxNet = kmboxNetSerial;
    }
    if (!makcu && !makcuNew && !kmboxNet)
        return nullptr;

    g_mouse = std::make_unique<MouseThread>(params, makcu, makcuNew, kmboxNet, makcuNewKbd);
    return g_mouse.get();
}
std::chrono::steady_clock::time_point g_last_tick{};
bool g_first_tick = true;
uint64_t g_frame_index = 0;

boss::AimPathDriver      g_path;
boss::TriggerFsm         g_trigger;
boss::ScopeController    g_scope;
boss::AutoStopController g_autoStop;

boss::AimPathDriver::Params pathParamsFrom(const HotkeyProfile& hk)
{
    boss::AimPathDriver::Params p;
    p.mode = static_cast<boss::AimPathDriver::Mode>(
        std::clamp(hk.aim_path_mode, 0, 3));
    p.strength = std::clamp(hk.aim_path_influence, 0, 100) / 100.0;
    p.cx1 = hk.aim_path_bezier_cx1;
    p.cy1 = hk.aim_path_bezier_cy1;
    p.cx2 = hk.aim_path_bezier_cx2;
    p.cy2 = hk.aim_path_bezier_cy2;
    p.custom_samples = hk.aim_path_custom_samples;
    p.wind_gravity  = hk.aim_path_wind_gravity;
    p.wind_wind     = hk.aim_path_wind_wind;
    p.wind_step     = hk.aim_path_wind_step;
    p.wind_distance = hk.aim_path_wind_distance;
    p.wind_threshold_px = hk.aim_path_wind_threshold;
    return p;
}

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ── 在途补偿观测日志 ─────────────────────────────────────────────────────
//
// 目的：让用户【不用猜】就能定两个上限 ——
//   · v          → 目标正常跑多快，用来定 ctl_predict_max_velocity_px_s
//   · auto       → 程序自己测得的链路延迟，用来对照自己实测的全延迟是否合理
//   · lead/dx/dy → 实际用的提前时间与推进量，验证是否符合预期
//
// ★ 注意：auto 只是【对照参考】，不参与计算。
//   实际提前时间完全以 ctl_predict_lead_ms（用户实测的全延迟）为准。
//
// 默认降频（每 30 帧一行），避免正常跑的时候刷屏；
// 调参时把 kPredictLogEveryFrame 改成 true 可以切成每帧。
namespace
{
constexpr bool kPredictLogEveryFrame = false;
constexpr int  kPredictLogIntervalFrames = 30;
}

}

void logPredictor(const control::ControlOutput& out)
{
    using IdleReason = control::PredictorResult::IdleReason;

    const control::PredictorResult& p = out.predictor;

    // 关闭状态下完全不打日志 —— 保持"没开这个功能就什么都没变"。
    if (p.idleReason == IdleReason::Disabled)
        return;

    if (!kPredictLogEveryFrame)
    {
        static uint64_t counter = 0;
        if ((counter++ % kPredictLogIntervalFrames) != 0)
            return;
    }

    const char* why = "";
    switch (p.idleReason)
    {
    case IdleReason::None:           why = "";             break;
    case IdleReason::Disabled:       return;
    case IdleReason::NotInitialized: why = "no-velocity";  break;
    }

    const double speed = p.rawVelocity.norm();
    const double clampedSpeed = p.clampedVelocity.norm();

    std::string line;
    char buf[256];
    if (!p.applied)
    {
        std::snprintf(buf, sizeof(buf),
                      "[predict] idle (%s)  v=%.1f px/s", why, speed);
        line = buf;
    }
    else
    {
        const std::string capStr = (p.maxLeadPx > 0.0)
            ? (std::to_string(static_cast<int>(p.maxLeadPx)) + "px")
            : std::string("inf");
        const std::string speedStr = p.velocityClamped
            ? ("CLAMP->" + std::to_string(static_cast<int>(clampedSpeed)) + "px/s")
            : std::string("off");

        // 程序自测的链路延迟：仅供对照，不参与计算。
        const double autoMs = runtime::latency::autoLeadLatencyMs();

        std::snprintf(buf, sizeof(buf),
                      "[predict] v=%.1f px/s  lead=%.1fms (ref auto %.1f)"
                      "  dx=%+.1f dy=%+.1f px  cap=%s  speed=%s%s",
                      speed,
                      p.leadSec * 1000.0,
                      autoMs,
                      p.lead.x, p.lead.y,
                      capStr.c_str(),
                      speedStr.c_str(),
                      p.leadClamped ? " [LEAD CLAMPED]" : "");
        line = buf;
    }

    if (runtime::latency::enabled())
        runtime::latency::detail::logLine(line);
    else
        std::cout << line << std::endl;
}

namespace
{

// 说明: 原先这里有个 readPhysicalMoveKeys(), 用 GetAsyncKeyState 读玩家按住的
// W/A/S/D —— 那是"注入反向键刹车"方案才需要的输入(要判断该反哪个方向)。
// 现在急停改成"整段屏蔽真实键盘", 与按了哪个方向无关, 该函数已无调用者, 删除。

control::Vec2 resolveCrosshair(const Config& cfg, const HotkeyProfile& hk, bool& fresh)
{
    const double center = static_cast<double>(cfg.detection_resolution) * 0.5;

    if (!hk.crosshair_detect_enabled)
    {
        fresh = true;
        return control::Vec2{ center, center };
    }

    const auto snap = crosshair_runtime::read();
    const auto now = std::chrono::steady_clock::now();
    const bool usable =
        snap.valid &&
        snap.ts.time_since_epoch().count() != 0 &&
        std::chrono::duration<double, std::milli>(now - snap.ts).count() <=
            static_cast<double>(crosshair_runtime::kFreshnessMs);

    if (usable)
    {
        fresh = true;
        return control::Vec2{ snap.x, snap.y };
    }

    fresh = false;
    return control::Vec2{ center, center };
}

}

bool tick()
{
    const auto snapshot = runtime_config::read();
    if (!snapshot)
        return false;
    const Config& cfg = *snapshot;

    const int activeIdx = runtime::g_active_hotkey_index.load();
    if (activeIdx < 0)
        return false;
    if (activeIdx >= static_cast<int>(cfg.hotkeys.size()))
        return false;
    const HotkeyProfile& hk = cfg.hotkeys[static_cast<size_t>(activeIdx)];

    if (!hk.ctl_enabled)
        return false;

    std::vector<control::Candidate> candidates;
    bool detectionFresh = false;
    {
        std::lock_guard<std::mutex> lk(detectionBuffer.mutex);
        if (detectionBuffer.boxes.empty())
            return false;
        detectionFresh = !detectionBuffer.staleLocked();
        const size_t n = detectionBuffer.boxes.size();
        candidates.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            const bool hasPrecise =
                (i < detectionBuffer.precise_boxes.size() &&
                 detectionBuffer.precise_boxes[i].width > 0.0f &&
                 detectionBuffer.precise_boxes[i].height > 0.0f);
            const cv::Rect& r = detectionBuffer.boxes[i];
            cv::Rect2f pr = hasPrecise ? detectionBuffer.precise_boxes[i]
                                       : cv::Rect2f(static_cast<float>(r.x),
                                                    static_cast<float>(r.y),
                                                    static_cast<float>(r.width),
                                                    static_cast<float>(r.height));
            control::Candidate c;
            c.box = control::Box{ static_cast<double>(pr.x), static_cast<double>(pr.y),
                                  static_cast<double>(pr.width), static_cast<double>(pr.height) };
            c.classId = (i < detectionBuffer.classes.size()) ? detectionBuffer.classes[i] : -1;
            c.confidence = (i < detectionBuffer.confidences.size())
                ? static_cast<double>(detectionBuffer.confidences[i]) : 0.0;
            candidates.push_back(c);
        }
    }
    if (candidates.empty())
        return false;

    bool crossFresh = false;
    const control::Vec2 cross = resolveCrosshair(cfg, hk, crossFresh);

    const auto now = std::chrono::steady_clock::now();
    double dtSec = 0.0;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_first_tick)
        {
            g_last_tick = now;
            g_first_tick = false;
            if (!g_controller)
                g_controller = std::make_unique<control::AimController>();
            g_controller->setConfig(toControllerConfig(flattenProfile(hk, cfg.detection_resolution, cfg.class_filters, cfg)));
            g_controller->reset();
            return false;
        }
        dtSec = std::chrono::duration<double>(now - g_last_tick).count();
    }
    if (!dtIsUsable(dtSec))
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_controller)
            g_controller->reset();
        g_last_tick = now;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_last_tick = now;
    }

    control::ControlOutput out;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_controller)
            g_controller = std::make_unique<control::AimController>();
        g_controller->setConfig(toControllerConfig(flattenProfile(hk, cfg.detection_resolution, cfg.class_filters, cfg)));

        control::ControlInput in;
        in.candidates = std::move(candidates);
        in.cross = cross;
        in.dtSec = dtSec;
        in.frameIndex = ++g_frame_index;
        in.detectionFresh = detectionFresh;
        in.crosshairFresh = crossFresh;

        out = g_controller->update(in);
    }

    logPredictor(out);

    MouseThread* mouse = ensureMouse();

    if (!out.engaged)
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        const auto act = g_scope.flushUp();
        if (mouse && act.release_right)
            mouse->releaseRightButton();
        return false;
    }

    int move_x = out.counts.x;
    int move_y = out.counts.y;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_path.configure(pathParamsFrom(hk));
        if (hk.aim_path_mode != 0)
        {
            const auto shaped = g_path.step(
                  out.anchor.x,        out.anchor.y,
                  cross.x,             cross.y,
                  dtSec,
                  out.targetId,
                  static_cast<double>(out.counts.x),
                  static_cast<double>(out.counts.y));
            move_x = static_cast<int>(std::lround(shaped.move_x));
            move_y = static_cast<int>(std::lround(shaped.move_y));
        }
    }

    const int64_t ms = nowMs();
    if (mouse)
    {
        std::lock_guard<std::mutex> lk(g_mtx);

        bool inZone = false;
        if (hk.trigger_enabled && out.hasTarget)
        {
            inZone = boss::TriggerFsm::inHitZone(
                cross.x, cross.y,
                out.targetBox.x, out.targetBox.y,
                out.targetBox.w, out.targetBox.h,
                hk.trigger_y_percent);
        }

        const bool scopeAllowed = std::none_of(
            hk.keys.begin(), hk.keys.end(),
            [](const std::string& k) { return k == "RightMouseButton"; });
        const int scopeMode = std::clamp(hk.trigger_auto_scope, 0, 2);
        const int scopeDelay = std::max(0, hk.trigger_scope_delay_ms);

        {
            const auto scopeAct = g_scope.tick(inZone, scopeAllowed, scopeMode, scopeDelay, ms);
            if (scopeAct.release_right) mouse->releaseRightButton();
            if (scopeAct.press_right)   mouse->pressRightButton();

            const bool scopeReady = g_scope.ready(scopeAllowed, scopeMode, scopeDelay, ms);

            const bool holdMode = (hk.trigger_fire_duration <= 0);

            boss::TriggerFsm::Input tin;
            tin.in_zone  = inZone;
            tin.track_id = out.targetId;
            tin.now_ms   = ms;

            boss::TriggerFsm::Action tAct;
            if (hk.trigger_enabled && scopeReady)
            {
                tAct = g_trigger.tick(tin, holdMode,
                    hk.trigger_fire_delay, hk.trigger_fire_duration,
                    hk.trigger_fire_interval, hk.trigger_switch_cooldown_ms,
                    hk.trigger_delay_jitter_ms, hk.trigger_duration_jitter_ms,
                    hk.trigger_interval_jitter_ms);
            }
            else if (!hk.trigger_enabled)
            {
                if (g_trigger.reset())
                    mouse->releaseLeftButton();
            }

            if (tAct.release_left) mouse->releaseLeftButton();
            if (tAct.press_left)   mouse->pressLeftButton();

            if (tAct.fired)
            {
                const auto snap = runtime_config::read();
                const bool methodOk = snap && (snap->input_method == "MAKCUNEW" ||
                                               snap->input_method == "KMBOXNET");

                // 自动急停需要【真的能屏蔽真实键盘】。光看 input_method 不够:
                // MAKCUNEW 下如果没接键盘硬件(第二台), 屏蔽命令无处可发。
                // 此时必须整体跳过 —— 既不下发, 也不影响鼠标的任何行为。
                // 驱动的 capabilities() 只在键盘硬件确实存在时才带 kCapKeyboard,
                // 所以这里查能力位就能如实反映"接了没有"。
                const bool kbCapable = methodOk && mouse &&
                                       mouse->supports(mouse_driver::kCapKeyboard);

                if (hk.trigger_auto_stop > 0 && kbCapable)
                {
                    const int stopMs = std::clamp(hk.trigger_stop_ms, 20, 300);

                    // 屏蔽真实键盘 stopMs, 而不是注入反向键。
                    //
                    // 旧做法: 读玩家按住的方向键(W/A/S/D), 然后注入【相反】的键
                    // (按 W 就点一下 S)去"刹车"。副作用明显 —— 注入的反向键本身
                    // 会被游戏当成一次真实按键, 造成松手后仍有一小段反向位移,
                    // 而且它要求把键盘事件送到被控机, 与玩家自己的输入叠加。
                    //
                    // 新做法: 直接把真实键盘输入屏蔽掉 stopMs。玩家按住的方向键
                    // 在这段时间内不进入被控机, 角色凭游戏自身的停止行为停住, 不
                    // 注入任何键 —— 没有任何残余反向位移, 也不干扰玩家的真实操作。
                    // 命令只从【键盘那台硬件】发出(真实键盘插在它上面), 绝不落到
                    // 鼠标硬件上, 否则会连带把真实鼠标输入一起屏蔽。
                    if (g_autoStop.maskActive(ms))
                    {
                        // 已在屏蔽窗口内: 不重复下发, 保持当前窗口自然到期。
                    }
                    else if (mouse->maskRealKeyboard(stopMs))
                    {
                        g_autoStop.markMasked(ms, stopMs);
                    }
                }
            }
        }
    }

    if (move_x == 0 && move_y == 0)
        return false;

    // 馈送标定器 (如果用户正在前台测算灵敏度 k)
    if (control::globalSensitivityCalibrator().isRunning() && out.hasTarget)
    {
        control::globalSensitivityCalibrator().feed(out.anchor.x, move_x, dtSec);
    }

    if (MouseThread* mouse = ensureMouse())
        mouse->sendRawMove(move_x, move_y);
    return true;
}

void reset()
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_controller)
            g_controller->reset();
        g_path.reset();
        g_trigger.reset();
        g_scope.forceRelease();
        g_autoStop.reset();
        g_first_tick = true;
        g_frame_index = 0;
        g_last_tick = std::chrono::steady_clock::time_point{};
    }
    std::lock_guard<std::mutex> lk(g_mouse_mtx);
    if (g_mouse)
    {
        g_mouse->releaseLeftButton();
        g_mouse->releaseRightButton();
        g_mouse->clearQueuedMoves();
        g_mouse.reset();
    }
}

bool active()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_controller != nullptr;
}

}
