#include "runtime/aim_loop.h"

#include "control/aim_controller.h"
#include "control/sensitivity_calibrator.h"

#include "mouse/aim_path.h"
#include "mouse/auto_stop.h"
#include "mouse/trigger_fsm.h"
#include "mouse/trigger_release.h"
#include "mouse/trigger_scope.h"

#include "Apotheosis.h"   // 设备指针 (makcuSerial / makcuNewSerial / kmboxNetSerial)
#include "config/config.h"
#include "crosshair/crosshair_runtime.h"
#include "detector/detection_buffer.h"
#include "mouse/mouse.h"
#include "runtime/active_hotkey.h"
#include "runtime/aim_path_config.h"
#include "runtime/aim_telemetry.h"
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

// 上一拍是否在用【开镜档】—— 只为"切档时打一行日志", 不参与控制。
// ★ 必须在 reset() 里归位, 否则新会话的第一拍会漏打/误打那行。
bool g_scopeCtlLast = false;

// 自动开镜【点按档(mode 1)】的粘性状态: 按过一次右键之后, 游戏就一直在镜内
// (该档不收镜, 由玩家自己负责), 所以"算不算在镜内"要粘到热键松开为止。
//
// ★ 为什么不能直接用它自己的 engaged(): 那一拍的 in_zone 一假就翻回 false,
//   但游戏里镜头还开着 —— 参数会跟着命中区来回翻, 与实机状态对不上。
// ★ mode 2(长按)不用它: 那种档右键真的按着才算在镜内, engaged() 就是真值。
bool g_scopeTapped = false;

struct PendingSwitch31
{
    bool armed = false;
    int delayMs = 50;
    int stepMs = 20;
};
PendingSwitch31 g_pendingSwitch31;
bool g_warnedSwitch31Unavailable = false;

// 只在本次自动扳机真的按下并松开左键后启动切枪。键盘线程独立运行，
// 即使之后丢框或松开热键，已经开出的这一枪仍会完成 3 → 1。
void finishSwitch31Shot(MouseThread* mouse)
{
    if (!g_pendingSwitch31.armed) return;
    const PendingSwitch31 pending = g_pendingSwitch31;
    g_pendingSwitch31 = {};
    if (!mouse)
    {
        std::cerr << "[Switch31] could not start keyboard sequence." << std::endl;
        return;
    }
    // 长按开镜必须在切枪前松右键；点按档的开镜状态仍由玩家自己负责。
    if (g_scope.mode() >= 2)
    {
        const auto scopeRelease = g_scope.forceRelease();
        if (scopeRelease.release_right) mouse->releaseRightButton();
    }
    if (!mouse->requestWeaponSwitch31(pending.delayMs, pending.stepMs))
        std::cerr << "[Switch31] could not start keyboard sequence." << std::endl;
}

// 热键松开 / 控制器关闭时把还按着的鼠标键放开。
//
// ★★ 这两种情况下 tick() 会在函数最前面直接 return —— g_trigger/g_scope 再
//   也没有机会跑到"离开命中区/换目标"那些分支去主动松手, 左键(自动扳机长按)
//   与右键(自动开镜长按档)会一直卡在按下状态, 直到下次热键触发时凑巧转回
//   松开分支, 或者整个检测会话停止(module reset() 才会调 forceRelease())。
// ★ 点按档(mode 1)的右键不受影响: ScopeController::forceRelease() 只在
//   mode>=2 时才返回 release_right, 点按档"开镜状态由玩家自己负责"的语义
//   保持不变 —— 这里只是让它的 engaged_ 状态跟 g_scopeTapped 一起清零, 对应
//   下面注释"点按档的'在镜内'粘性状态到此为止", 免得下次热键重新按下时
//   ScopeController 还记得上一轮已经点过, 不再补发新的一次点按。
void releaseHeldButtons()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    const bool releaseLeft = g_trigger.reset();
    const auto act = g_scope.forceRelease();
    if (MouseThread* mouse = ensureMouse())
    {
        if (act.release_right)
            mouse->releaseRightButton();
        if (releaseLeft)
        {
            mouse->releaseLeftButton();
            finishSwitch31Shot(mouse);
        }
    }
    else
        g_pendingSwitch31 = {};
}

void releaseTargetButtons(int scopeMode)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    const auto release = boss::releaseOnTargetLoss(g_trigger, g_scope, scopeMode);
    if (MouseThread* mouse = ensureMouse())
    {
        if (release.right) mouse->releaseRightButton();
        if (release.left)
        {
            mouse->releaseLeftButton();
            finishSwitch31Shot(mouse);
        }
    }
    else
        g_pendingSwitch31 = {};
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
    {
        // 热键已松: 点按档的"在镜内"粘性状态到此为止。
        g_scopeTapped = false;
        releaseHeldButtons();
        return false;
    }
    if (activeIdx >= static_cast<int>(cfg.hotkeys.size()))
    {
        g_scopeTapped = false;
        releaseHeldButtons();
        return false;
    }
    const HotkeyProfile& hk = cfg.hotkeys[static_cast<size_t>(activeIdx)];

    if (!hk.ctl_enabled)
    {
        g_scopeTapped = false;
        releaseHeldButtons();
        return false;
    }

    std::vector<control::Candidate> candidates;
    bool detectionFresh = false;
    {
        std::lock_guard<std::mutex> lk(detectionBuffer.mutex);
        const size_t n = detectionBuffer.boxes.size();
        detectionFresh = n > 0 && !detectionBuffer.staleLocked();
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
    {
        releaseTargetButtons(std::clamp(hk.trigger_auto_scope, 0, 2));
        return false;
    }

    bool crossFresh = false;
    const control::Vec2 cross = resolveCrosshair(cfg, hk, crossFresh);

    // ── 自动开镜是否生效 ──────────────────────────────────────────────────
    //
    // 自动开镜(自动扳机按住的右键)生效 + 热键仍被按住 ⇒ 控制器改用【开镜档】
    // 那一整组参数, 而不是热键自己的默认档。
    //
    // ★ "算不算在镜内"按开镜方式分两种, 与实机镜头状态对齐:
    //   · mode 1 点按(不收镜): 按过一次右键之后镜头一直开着 ⇒ 粘到热键松开;
    //   · mode 2 长按:          右键真的按着才算 ⇒ 用开镜控制器自己的 engaged。
    //   mode 0(关闭) 两边都为假, 永远不切档。
    //
    // ★ 为什么读的是【上一拍】的状态: 开镜判定要用本拍的目标框(命中区),
    //   而目标框是控制器算完才有的 —— 本拍必然读不到本拍的判定。慢一拍 ≈ 几
    //   毫秒, 对"镜内换一套增益"没有影响; 也不去改动扳机/开镜的既有相位。
    const int scopeMode = std::clamp(hk.trigger_auto_scope, 0, 2);
    if (scopeMode != 1)
        g_scopeTapped = false;   // 换档/关掉 ⇒ 点按档的粘性状态作废
    bool scopeEngaged = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        scopeEngaged = hk.trigger_enabled &&
                       ((scopeMode == 1) ? g_scopeTapped : g_scope.engaged());
    }
    // 本拍是否真的在用【开镜档】那一组参数 (日志与预览都用它)。
    const bool scopeCtlActive = scopeEngaged && hk.scope_ctl_enabled != 0;

    // 只统计驱动报告成功的位移；路径整形、队列覆盖和发送失败都在此之后结算。
    MouseThread::MovementFeedback movementFeedback;
    if (MouseThread* mouse = ensureMouse())
        movementFeedback = mouse->consumeMovementFeedback();

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
            g_controller->setConfig(toControllerConfig(
                flattenProfile(hk, cfg.detection_resolution, cfg.class_filters, cfg,
                               scopeEngaged)));
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

    if (MouseThread* mouse = ensureMouse(); mouse && mouse->weaponSwitch31Busy())
    {
        // Lua 切枪期间暂停整条瞄准/扳机链。清掉 PID 与轨迹余量，避免切回 1 后
        // 把切枪期间积累的误差一下子发出去；时钟仍逐拍更新。
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_controller) g_controller->reset();
        g_path.reset();
        return false;
    }

    {
        // 只在【切档那一刻】打一行, 不刷屏 —— 用户要能确认它真的切了。
        if (scopeCtlActive != g_scopeCtlLast)
        {
            g_scopeCtlLast = scopeCtlActive;
            std::cout << (scopeCtlActive
                    ? "[scope] 自动开镜生效 -> 切到【开镜档】瞄准参数"
                    : "[scope] 自动开镜结束 -> 切回热键【默认档】瞄准参数")
                      << std::endl;
        }
    }

    control::ControlOutput out;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_controller)
            g_controller = std::make_unique<control::AimController>();
        g_controller->setConfig(toControllerConfig(
            flattenProfile(hk, cfg.detection_resolution, cfg.class_filters, cfg, scopeEngaged)));

        control::ControlInput in;
        in.candidates = std::move(candidates);
        in.cross = cross;
        in.dtSec = dtSec;
        in.sentCounts = control::Counts{ movementFeedback.dx, movementFeedback.dy };
        in.frameIndex = ++g_frame_index;
        in.detectionFresh = detectionFresh;
        in.crosshairFresh = crossFresh;

        out = g_controller->update(in);
    }

    // ── 预览叠加: 把【稳定之后】的结果交给预览线程 ─────────────────────────
    //
    // 预览原来只画检测框(detectionBuffer 里的原始框) —— 那是稳定【之前】的东西:
    // 稳定器判定、锁的是哪个目标、滤波压掉了多少抖动, 全都看不见。
    // 这里把稳定器放行的框 + 滤波后的中心 + 最终瞄点发过去, 让调参能对着画面调。
    //
    // ★ 不管 engaged 都发: 没锁上的时候预览要能说出【为什么没锁】
    //   (丢帧/找色失效/被稳定器丢掉), 那是调参时最需要看的信息。
    // ★ 纯只读, 不参与控制。
    {
        runtime::AimOverlayState s;
        s.valid           = out.hasTarget;
        s.engaged         = out.engaged;
        s.box             = cv::Rect(static_cast<int>(std::lround(out.targetBox.x)),
                                     static_cast<int>(std::lround(out.targetBox.y)),
                                     static_cast<int>(std::lround(out.targetBox.w)),
                                     static_cast<int>(std::lround(out.targetBox.h)));
        s.filtered_cx     = out.filteredCenter.x;
        s.filtered_cy     = out.filteredCenter.y;
        s.anchor_x        = out.anchor.x;
        s.anchor_y        = out.anchor.y;
        s.target_id       = out.targetId;
        s.target_class_id = out.targetClassId;
        s.verdict         = static_cast<int>(out.stabVerdict);
        s.idle_reason     = static_cast<int>(out.idleReason);
        s.scope_params    = scopeCtlActive;
        runtime::publishAimOverlay(s);
    }

    logPredictor(out);

    MouseThread* mouse = ensureMouse();

    if (!out.engaged)
    {
        releaseTargetButtons(scopeMode);
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
        // scopeMode 已在上面(切档判定)算过, 这里复用同一个值。
        const int scopeDelay = std::max(0, hk.trigger_scope_delay_ms);

        {
            const bool switchCapable = hk.trigger_weapon_switch31 &&
                mouse->supports(mouse_driver::kCapKeyboard);
            if (hk.trigger_weapon_switch31 && !switchCapable &&
                !g_warnedSwitch31Unavailable)
            {
                std::cerr << "[Switch31] keyboard device unavailable; firing without weapon switch."
                          << std::endl;
                g_warnedSwitch31Unavailable = true;
            }
            if (!hk.trigger_weapon_switch31 || switchCapable)
                g_warnedSwitch31Unavailable = false;

            const auto scopeAct = g_scope.tick(inZone, scopeAllowed, scopeMode, scopeDelay, ms);
            if (scopeAct.release_right) mouse->releaseRightButton();
            if (scopeAct.press_right)
            {
                mouse->pressRightButton();
                // 点按档不收镜: 这一次点按之后镜头一直开着, 粘到热键松开为止
                // (下一拍起 scopeEngaged 才会看到它 —— 与"慢一拍"的口径一致)。
                if (scopeMode == 1) g_scopeTapped = true;
            }

            const bool scopeReady = g_scope.ready(scopeAllowed, scopeMode, scopeDelay, ms);

            // 开火后即使用户临时关掉切枪或键盘断开，本次已按下的左键仍按短按
            // 语义完成，不能中途退回「长按直到离区」。
            const bool switchPulse = switchCapable || g_pendingSwitch31.armed;
            const bool holdMode = (hk.trigger_fire_duration <= 0) && !switchPulse;
            const int fireDuration = switchPulse
                ? std::max(hk.trigger_fire_duration, 20)
                : hk.trigger_fire_duration;

            boss::TriggerFsm::Input tin;
            tin.in_zone  = inZone;
            tin.track_id = out.targetId;
            tin.now_ms   = ms;

            boss::TriggerFsm::Action tAct;
            if (hk.trigger_enabled && scopeReady)
            {
                tAct = g_trigger.tick(tin, holdMode,
                    hk.trigger_fire_delay, fireDuration,
                    hk.trigger_fire_interval, hk.trigger_switch_cooldown_ms,
                    hk.trigger_delay_jitter_ms,
                    switchPulse ? 0 : hk.trigger_duration_jitter_ms,
                    hk.trigger_interval_jitter_ms);
            }
            else if (boss::releaseTriggerIfUnavailable(
                         g_trigger, hk.trigger_enabled, scopeReady))
            {
                mouse->releaseLeftButton();
                finishSwitch31Shot(mouse);
            }

            if (tAct.release_left)
            {
                mouse->releaseLeftButton();
                finishSwitch31Shot(mouse);
            }
            if (tAct.press_left)   mouse->pressLeftButton();

            if (tAct.fired)
            {
                if (switchCapable)
                {
                    g_pendingSwitch31.armed = true;
                    g_pendingSwitch31.delayMs = hk.trigger_switch31_delay_ms;
                    g_pendingSwitch31.stepMs = hk.trigger_switch31_step_ms;
                }
                const auto snap = runtime_config::read();
                const bool methodOk = snap && (snap->input_method == "MAKCU" ||
                                               snap->input_method == "MAKCUNEW" ||
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

    // 标定器使用已确认发送的计数和本拍观测位置，不把尚未发送的 PID 输出当作实测。
    if (control::globalSensitivityCalibrator().isRunning() && out.hasTarget)
    {
        control::globalSensitivityCalibrator().feed(
            out.filteredCenter.x, movementFeedback.dx, dtSec);
    }

    if (MouseThread* mouse = ensureMouse(); mouse && mouse->weaponSwitch31Busy())
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_controller) g_controller->reset();
        g_path.reset();
        return false;
    }

    if (move_x == 0 && move_y == 0)
        return false;

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
        g_scopeCtlLast = false;
        g_scopeTapped = false;
        g_pendingSwitch31 = {};
        g_warnedSwitch31Unavailable = false;
    }
    resetMouse();
}

void resetMouse()
{
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
