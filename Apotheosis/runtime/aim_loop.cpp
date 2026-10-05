#include "runtime/aim_loop.h"
#include "macro/macro_engine.h"

#include "control/recovered_aim_controller.h"
#include "control/head_body_fusion.h"
#include "capture/auto_capture.h"

#include "mouse/aim_path.h"
#include "mouse/auto_stop.h"
#include "mouse/am_trigger.h"
#include "mouse/trigger_prearm.h"
#include "mouse/trigger_target.h"
#include "mouse/trigger_flash_post.h"
#include "mouse/trigger_release.h"
#include "mouse/trigger_scope.h"

#include "Apotheosis.h"   // 设备指针 (makcuSerial / makcuNewSerial / kmboxNetSerial)
#include "config/config.h"
#include "crosshair/crosshair_runtime.h"
#include "detector/detection_buffer.h"
#include "mouse/mouse.h"
#include "mouse/windows_driver.h"
#include "mouse/cpbox_driver.h"
#include "runtime/active_hotkey.h"
#include "runtime/aimpoint_recoil.h"
#include "keyboard/keyboard_listener.h"
#include "mouse/switch31_shot_gate.h"
#include "runtime/aim_path_config.h"
#include "runtime/aim_telemetry.h"
#include "runtime/config_snapshot.h"
#include "runtime/latency_probe.h"
#include "runtime/motion_feedback_window.h"
#include "runtime/ff_calibration_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <windows.h>   // GetAsyncKeyState —— 自动急停读物理方向键
#endif

namespace runtime::aim_loop
{

namespace
{

std::mutex g_mtx;
std::unique_ptr<control::RecoveredAimController> g_controller;
runtime::AimpointRecoilGate g_aimpointRecoilGate;
std::shared_ptr<const Config> g_appliedConfigSnapshot;
int g_appliedConfigHotkey = -1;
bool g_appliedConfigScope = false;
bool g_appliedConfigSecondary = false;
bool g_appliedConfigUnlockY = false;

// Called with g_mtx held. Compiling a profile builds several vectors, and
// setConfig copies them into the controller, so do it only when inputs change.
void configureController(const std::shared_ptr<const Config>& snapshot,
                         int hotkeyIndex, bool scopeActive, bool secondaryActive,
                         bool unlockYActive)
{
    if (!g_controller)
    {
        g_controller = std::make_unique<control::RecoveredAimController>();
        g_appliedConfigSnapshot.reset();
    }
    if (g_appliedConfigSnapshot == snapshot
        && g_appliedConfigHotkey == hotkeyIndex
        && g_appliedConfigScope == scopeActive
        && g_appliedConfigSecondary == secondaryActive
        && g_appliedConfigUnlockY == unlockYActive)
        return;

    const Config& cfg = *snapshot;
    const HotkeyProfile& hk = cfg.hotkeys[static_cast<size_t>(hotkeyIndex)];
    if (g_appliedConfigHotkey != hotkeyIndex || g_appliedConfigScope != scopeActive ||
        g_appliedConfigSecondary != secondaryActive)
        g_controller->resetCompensation();
    auto pid = pidForProfile(hk, scopeActive, secondaryActive);
    pid.maskX = hk.unlock_x;
    pid.maskY = unlockYActive;
    if (g_appliedConfigUnlockY != unlockYActive)
        g_controller->resetPidAxes(false, true);
    g_controller->setConfig(
        toControllerConfig(flattenProfile(
            hk, cfg.detection_resolution, cfg.class_filters, cfg)),
        pid);
    g_appliedConfigSnapshot = snapshot;
    g_appliedConfigHotkey = hotkeyIndex;
    g_appliedConfigScope = scopeActive;
    g_appliedConfigSecondary = secondaryActive;
    g_appliedConfigUnlockY = unlockYActive;
}

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
    std::shared_ptr<mouse_driver::IDriver> dhzbox;
    std::shared_ptr<mouse_driver::IDriver> ferrumDriverLocal;
    std::shared_ptr<mouse_driver::IDriver> catDriverLocal;
    std::shared_ptr<mouse_driver::CpboxDriver> cpboxDriverLocal;
    std::shared_ptr<mouse_driver::IDriver> windowsDriverLocal;
    {
        std::lock_guard<std::mutex> lkDev(inputDeviceMutex);
        makcu = makcuSerial;
        makcuNew = makcuNewSerial;
        makcuNewKbd = makcuNewSerialKbd;
        kmboxNet = kmboxNetSerial;
        dhzbox = dhzboxDriver;
        ferrumDriverLocal = ferrumDriver;
        catDriverLocal = catDriver;
        cpboxDriverLocal = cpboxDriver;
        windowsDriverLocal = windowsDriver;
    }
    if (!makcu && !makcuNew && !kmboxNet && !dhzbox && !ferrumDriverLocal &&
        !catDriverLocal && !cpboxDriverLocal && !windowsDriverLocal)
        return nullptr;

    g_mouse = std::make_unique<MouseThread>(params, makcu, makcuNew, kmboxNet,
        makcuNewKbd, windowsDriverLocal ? windowsDriverLocal : cpboxDriverLocal ? cpboxDriverLocal :
        dhzbox ? dhzbox : catDriverLocal ? catDriverLocal : ferrumDriverLocal);
    return g_mouse.get();
}
std::chrono::steady_clock::time_point g_last_tick{};
bool g_first_tick = true;
int64_t g_last_capture_ns = 0;
runtime::MotionFeedbackWindow g_pidfFeedback;
bool g_wasCalibrating = false;
uint64_t g_frame_index = 0;
int g_last_active_hotkey = -1;
int g_flash_target_id = -1;
int g_flash_hotkey = -1;
bool g_flash_above = false;
int64_t g_flash_last_fire_ms = 0;
double g_flash_threshold = -1.0;
std::string g_flash_key;

void resetAutoFlash() {
    if (g_flash_target_id >= 0 || g_flash_above)
        macros::cancelAutoFlash();
    g_flash_target_id = -1;
    g_flash_hotkey = -1;
    g_flash_above = false;
}
int64_t g_hotkey_activated_ms = 0;
std::atomic<int> g_target_aim_hotkey{-1};
std::atomic<int64_t> g_target_aim_updated_ms{0};
int g_aim_delay_target_id = -1;
int64_t g_aim_delay_started_ms = 0;
bool g_aim_delay_ready = false;
int g_aim_delay_setting_ms = 0;

boss::AimPathDriver      g_path;
bool g_pathMaskX = false, g_pathMaskY = false;
boss::AmTriggerFsm         g_trigger;
boss::TriggerPrearm      g_prearm;
boss::TriggerFlashPostController g_flashPost;
int g_lastTriggerMode = 0;
int64_t g_flashLastFrameNs = 0;
int64_t g_flashRequireCaptureNs = 0;
boss::TriggerTargetSelector g_triggerTargetSelector;
boss::ScopeController    g_scope{true};
boss::AutoStopController g_autoStop;

// 上一拍是否在用【开镜档】—— 只为"切档时打一行日志", 不参与控制。
// ★ 必须在 reset() 里归位, 否则新会话的第一拍会漏打/误打那行。
bool g_scopeCtlLast = false;
bool g_secondaryCtlLast = false;
bool g_secondaryTriggerLast = false;

// 自动开镜【点按档(mode 1)】的粘性状态: 按过一次右键之后, 游戏就一直在镜内。
// 3-1 切枪会关镜并清除此状态；否则粘到热键松开为止。
//
// ★ 为什么不能直接用它自己的 engaged(): 那一拍的 in_zone 一假就翻回 false,
//   但游戏里镜头还开着 —— 参数会跟着命中区来回翻, 与实机状态对不上。
// ★ mode 2(长按)不用它: 那种档右键真的按着才算在镜内, engaged() 就是真值。
bool g_scopeTapped = false;

mouse_async::Switch31ShotGate g_pendingSwitch31;
bool g_warnedSwitch31Unavailable = false;
int64_t nowMs();

// 只在本次自动扳机真的按下并松开左键后启动切枪。键盘线程独立运行，
// 即使之后丢框或松开热键，已经开出的这一枪仍会完成 3 → 1 → 1 → 1。
void finishSwitch31Shot(MouseThread* mouse)
{
    const int delayMs = g_pendingSwitch31.onRelease(nowMs());
    if (delayMs < 0) return;
    if (!mouse)
    {
        std::cerr << "[Switch31] could not start keyboard sequence." << std::endl;
        return;
    }
    // 切枪会关镜。清掉点按档的粘性状态，让持续按住热键时下一枪重新开镜。
    // 长按档在发送切枪键之前也必须松开右键。
    const auto scopeRelease = g_scope.forceRelease();
    if (scopeRelease.release_right) mouse->releaseRightButton();
    g_scopeTapped = false;
    if (!mouse->requestWeaponSwitch31(delayMs))
        std::cerr << "[Switch31] could not start keyboard sequence." << std::endl;
}

// 已经下发左键按下的这一枪必须完成最短按住时间。目标丢失、换档或松热键
// 只能阻止下一枪，不能把本枪的切枪阶段截断。
void completeShot(MouseThread* mouse)
{
    if (!mouse) return;
    const int remainingMs = g_pendingSwitch31.remainingHoldMs(nowMs());
    if (remainingMs > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(remainingMs));
    mouse->releaseLeftButton();
    finishSwitch31Shot(mouse);
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
    g_prearm.reset();
    g_flashPost.reset();
    g_flashLastFrameNs = g_flashRequireCaptureNs = 0;
    const auto act = g_scope.forceRelease();
    if (MouseThread* mouse = ensureMouse())
    {
        mouse->clearQueuedMoves();
        if (act.release_right)
            mouse->releaseRightButton();
        if (releaseLeft)
            completeShot(mouse);
        if (g_autoStop.preparing() || g_autoStop.continuous())
        {
            if (g_autoStop.preparing()) g_autoStop.markFired(nowMs(), 0);
            if (mouse->maskRealKeyboard(0)) g_autoStop.reset();
        }
    }
    else
        g_pendingSwitch31.cancel();
}

void releaseTargetButtons(int scopeMode, const TriggerParams& trigger, bool allowGrace)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    g_prearm.reset();
    const bool retain = allowGrace && trigger.trigger_enabled &&
        !trigger.trigger_weapon_switch31 && !g_pendingSwitch31.armed;
    boss::TargetLossRelease release;
    if (retain) {
        release = boss::releaseOnTargetLoss(g_trigger, g_scope, scopeMode,
            nowMs(), trigger.trigger_loss_delay_ms, false);
    } else {
        release.left = g_trigger.reset();
        release.right = g_scope.forceRelease().release_right;
    }
    if (MouseThread* mouse = ensureMouse())
    {
        if (release.right && !mouse->releaseRightButton())
            g_scope.retryTapRelease(nowMs());
        if (release.left)
            completeShot(mouse);
        if (g_autoStop.preparing() || g_autoStop.continuous())
        {
            if (g_autoStop.preparing()) g_autoStop.markFired(nowMs(), 0);
            if (mouse->maskRealKeyboard(0)) g_autoStop.reset();
        }
    }
    else
        g_pendingSwitch31.cancel();
}

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}

namespace
{

// 说明: 原先这里有个 readPhysicalMoveKeys(), 用 GetAsyncKeyState 读玩家按住的
// W/A/S/D —— 那是"注入反向键刹车"方案才需要的输入(要判断该反哪个方向)。
// 现在急停改成"整段屏蔽真实键盘", 与按了哪个方向无关, 该函数已无调用者, 删除。

runtime::CrosshairFrameHold g_crosshairHold;

control::Vec2 resolveCrosshair(const Config& cfg, const HotkeyProfile& hk, bool& fresh,
                              const runtime::FrameContext& frame,
                              const runtime::FrameCrosshair& pivot, int activeIdx, int batchVersion)
{
    const double center = static_cast<double>(cfg.detection_resolution) * 0.5;

    if (!hk.crosshair_detect_enabled && !hk.laser_detect_enabled)
    {
        g_crosshairHold.reset();
        fresh = true;
        return control::Vec2{ center, center };
    }

    if (hk.crosshair_detect_enabled)
    {
        const auto resolved = g_crosshairHold.resolve(frame,pivot,activeIdx,
            cfg.detection_resolution,cfg.crosshair_algorithm,batchVersion);
        fresh = resolved.source == runtime::CrosshairFrameHold::Source::CurrentFrame;
        return {resolved.x,resolved.y};
    }
    g_crosshairHold.reset();
    // Laser retains its existing asynchronous freshness policy.
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

bool tick(int* consumedVersion)
{
    std::lock_guard<std::recursive_mutex> outputLock(macros::outputMutex());
    auto& calibration = runtime::FfCalibrationSession::instance();
    if (macros::ownsOutput()) {
        if (calibration.active()) calibration.fail("宏接管了输出，标定中止");
        g_target_aim_hotkey = -1; resetAutoFlash(); return false;
    }
    if (g_wasCalibrating && !calibration.active()) {
        if (auto* mouse = ensureMouse()) { mouse->clearQueuedMoves(); mouse->consumeMovementFeedback(); }
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_controller) g_controller->reset();
        g_path.reset(); g_pidfFeedback.reset(); g_first_tick = true; g_last_capture_ns = 0;
        g_wasCalibrating = false;
    }
    // The post-shot timer must keep running even when the hotkey is released
    // or no detection is available on this frame.
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_autoStop.shouldCancel(nowMs()))
        {
            MouseThread* mouse = ensureMouse();
            if (!mouse || mouse->maskRealKeyboard(0)) g_autoStop.reset();
        }
    }
    const auto snapshot = runtime_config::read();
    if (!snapshot)
    {
        if (calibration.active()) calibration.fail("运行配置不可用，标定中止");
        g_target_aim_hotkey = -1;
        resetAutoFlash();
        return false;
    }
    const Config& cfg = *snapshot;

    auto deactivateHotkey = [] {
        g_crosshairHold.reset();
        g_target_aim_hotkey = -1;
        resetAutoFlash();
        g_aim_delay_target_id = -1;
        g_aim_delay_started_ms = 0;
        g_aim_delay_ready = false;
        g_aim_delay_setting_ms = 0;
        if (g_last_active_hotkey < 0)
            return;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (g_controller) g_controller->reset();
            g_triggerTargetSelector.reset();
            g_path.reset();
            g_first_tick = true;
            g_last_capture_ns = 0;
            g_pidfFeedback.reset();
            g_last_active_hotkey = -1;
            g_hotkey_activated_ms = 0;
            g_aimpointRecoilGate.reset();
        }
        g_scopeTapped = false;
        g_secondaryTriggerLast = false;
        releaseHeldButtons();
    };

    const int activeIdx = runtime::g_active_hotkey_index.load();
    if (activeIdx < 0)
    {
        if (calibration.active()) calibration.released();
        deactivateHotkey();
        return false;
    }
    if (activeIdx >= static_cast<int>(cfg.hotkeys.size()))
    {
        if (calibration.active()) calibration.fail("所选热键已不存在，标定中止");
        deactivateHotkey();
        return false;
    }
    const HotkeyProfile& hk = cfg.hotkeys[static_cast<size_t>(activeIdx)];

    const bool secondarySelected = runtime::g_secondary_aim_hotkey_index.load() == activeIdx;
    const TriggerParams primaryTrigger = triggerParamsOf(hk);
    const TriggerParams& trigger = secondarySelected && hk.secondary_trigger_custom
        ? hk.secondary_trigger : primaryTrigger;
    if (!hk.ctl_enabled && !trigger.trigger_enabled)
    {
        if (calibration.active()) calibration.fail("所选热键已停用，标定中止");
        deactivateHotkey();
        return false;
    }
    if (secondarySelected != g_secondaryTriggerLast)
    {
        releaseHeldButtons();
        g_scopeTapped = false;
        g_secondaryTriggerLast = secondarySelected;
    }
    const int triggerMode = trigger.trigger_enabled ? std::clamp(trigger.trigger_mode, 0, 2) : 0;
    if (triggerMode != g_lastTriggerMode)
    {
        releaseHeldButtons();
        if (MouseThread* mouse = ensureMouse()) mouse->consumeMovementFeedback();
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_pidfFeedback.reset();
        }
        g_lastTriggerMode = triggerMode;
        g_path.reset();
    }

    if (activeIdx != g_last_active_hotkey)
    {
        g_target_aim_hotkey = -1;
        resetAutoFlash();
        g_aim_delay_target_id = -1;
        g_aim_delay_started_ms = 0;
        g_aim_delay_ready = false;
        g_aim_delay_setting_ms = 0;
        const bool hadOldHotkey = g_last_active_hotkey >= 0;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (g_controller) g_controller->reset();
            g_triggerTargetSelector.reset();
            g_path.reset();
            g_first_tick = true;
            g_last_capture_ns = 0;
            g_pidfFeedback.reset();
            g_last_active_hotkey = activeIdx;
            g_hotkey_activated_ms = nowMs();
            g_aimpointRecoilGate.reset();
        }
        if (hadOldHotkey)
        {
            g_scopeTapped = false;
            releaseHeldButtons();
        }
    }

    std::vector<control::Candidate> candidates;
    std::vector<control::Candidate> triggerCaptureDetections;
    const bool captureOnTrigger = cfg.auto_capture_enabled && cfg.auto_capture_trigger_only;
    bool detectionFresh = false;
    bool detectionFrameFresh = false;
    int64_t captureNs = 0;
    int64_t publishNs = 0;
    int detectedVersion = 0;
    runtime::FrameContext detectedFrame;
    runtime::FrameCrosshair detectedCrosshair;
    {
        std::lock_guard<std::mutex> lk(detectionBuffer.mutex);
        if (consumedVersion) {
            if (*consumedVersion == detectionBuffer.version) return false;
            *consumedVersion = detectionBuffer.version;
        }
        detectedFrame = detectionBuffer.frame_context;
        detectedVersion = detectionBuffer.version;
        detectedCrosshair = detectionBuffer.frame_crosshair;
        captureNs = detectionBuffer.frame_context.captured_ns > 0
            ? detectionBuffer.frame_context.captured_ns : detectionBuffer.frame_stamp_ns;
        publishNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            detectionBuffer.stamp.time_since_epoch()).count();
        const size_t n = detectionBuffer.boxes.size();
        detectionFrameFresh = !detectionBuffer.staleLocked();
        detectionFresh = n > 0 && detectionFrameFresh;
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
            if (captureOnTrigger) {
                auto label = c;
                // Keep the detector's original label geometry, before aim fusion/filtering.
                label.box = {double(r.x), double(r.y), double(r.width), double(r.height)};
                triggerCaptureDetections.push_back(label);
            }
            if (c.confidence <= cfg.confidence_threshold) continue;
            candidates.push_back(c);
        }
    }
    // Trigger class selection uses the raw detections. Head/body fusion may
    // intentionally remove a head box from the aim path, but the trigger's
    // independently configured head class must remain available.
    std::vector<control::Candidate> triggerCandidates;
    triggerCandidates = candidates;
    if (cfg.head_body_fusion_enabled)
        control::fuseHeadBody(candidates, cfg.head_body_head_class_id,
                              cfg.head_body_body_class_id);
    bool crossFresh = false;
    const control::Vec2 cross = resolveCrosshair(cfg, hk, crossFresh,
                                               detectedFrame, detectedCrosshair, activeIdx, detectedVersion);
    if (calibration.active()) {
        MouseThread* mouse = ensureMouse();
        if (!mouse || !mouse->supports(mouse_driver::kCapMove) || mouse->weaponSwitch31Busy()) {
            calibration.fail("没有可用的鼠标位移设备，或设备正在切枪");
            return false;
        }
        if (calibration.needsStart()) {
            mouse->clearQueuedMoves(); mouse->consumeMovementFeedback();
            releaseHeldButtons(); resetAutoFlash();
            std::lock_guard<std::mutex> lk(g_mtx);
            if (g_controller) g_controller->reset();
            g_trigger.reset(); g_scope.forceRelease(); g_path.reset(); g_pidfFeedback.reset();
            g_first_tick = true; g_last_capture_ns = 0;
        }
        g_wasCalibrating = true; g_target_aim_hotkey = -1;
        std::vector<control::Candidate> selected;
        if (detectionFrameFresh) for (const auto& c : candidates) {
            const bool allowed = hk.aim_classes.empty() || std::any_of(hk.aim_classes.begin(), hk.aim_classes.end(),
                [&](const HotkeyAimClass& a) { return a.class_id == c.classId && c.confidence >= a.min_conf; });
            const auto center = c.box.center();
            if (allowed && std::abs(center.x-cross.x) <= hk.fovX*0.5 &&
                           std::abs(center.y-cross.y) <= hk.fovY*0.5) selected.push_back(c);
        }
        const auto feedback = mouse->consumeMovementFeedback();
        std::vector<control::FfCalibrationMove> moves;
        for (const auto& e : feedback.events) moves.push_back({e.timestamp_us, {e.dx, e.dy}});
        const auto move = calibration.update(activeIdx, runtime::ffCalibrationNowUs(), captureNs/1000,
                                              selected, moves, feedback.failed);
        const bool sent = calibration.dispatch(move, [&](control::Counts counts) {
            mouse->sendRawMove(counts.x, counts.y, captureNs);
        });
        if (!calibration.active()) mouse->clearQueuedMoves();
        return sent;
    }
    const bool flashTurning = triggerMode == 2 &&
        g_flashPost.phase() == boss::TriggerFlashPostController::Phase::Turn;
    if (flashTurning) {
        // A fast camera turn yields motion-blurred, outdated boxes. Reacquire
        // only after the turn has finished and a post-move frame arrives.
        candidates.clear();
        triggerCandidates.clear();
        detectionFresh = false;
    }
    const std::vector<control::Candidate> flashCandidates = triggerMode == 0
        ? std::vector<control::Candidate>{}
        : (hk.trigger_classes.empty() ? candidates : triggerCandidates);

    // ── 自动开镜是否生效 ──────────────────────────────────────────────────
    //
    // 自动开镜(自动扳机按住的右键)生效 + 热键仍被按住 ⇒ 控制器改用【开镜档】
    // 那一整组参数, 而不是热键自己的默认档。
    //
    // ★ "算不算在镜内"按开镜方式分两种, 与实机镜头状态对齐:
    //   · mode 1 点按: 按过一次右键后保持镜内，切枪或松热键时清掉状态;
    //   · mode 2 长按:          右键真的按着才算 ⇒ 用开镜控制器自己的 engaged。
    //   mode 0(关闭) 两边都为假, 永远不切档。
    //
    // ★ 为什么读的是【上一拍】的状态: 开镜判定要用本拍的目标框(命中区),
    //   而目标框是控制器算完才有的 —— 本拍必然读不到本拍的判定。慢一拍 ≈ 几
    //   毫秒, 对"镜内换一套增益"没有影响; 也不去改动扳机/开镜的既有相位。
    const int scopeMode = std::clamp(trigger.trigger_auto_scope, 0, 2);
    if (scopeMode != 1)
        g_scopeTapped = false;   // 换档/关掉 ⇒ 点按档的粘性状态作废
    bool scopeEngaged = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        scopeEngaged = trigger.trigger_enabled &&
                       ((scopeMode == 1) ? g_scopeTapped : g_scope.engaged());
    }
    // 本拍是否真的在用【开镜档】那一组参数 (日志与预览都用它)。
    const bool scopeCtlActive = scopeEngaged && hk.scope_ctl_enabled != 0;
    const bool secondaryCtlActive = !scopeCtlActive && secondarySelected;
    // 只统计驱动报告成功的位移；路径整形、队列覆盖和发送失败都在此之后结算。
    MouseThread::MovementFeedback movementFeedback;
    if (MouseThread* mouse = ensureMouse())
        movementFeedback = mouse->consumeMovementFeedback();
    if (triggerMode != 0 && g_flashPost.shotTargetLocked()) {
        // Keep the ordinary aim and trigger selectors on the enemy already
        // being fired at. Another visible enemy must wait for return or for
        // the current direction's next acquisition window.
        const control::Counts latestMove{movementFeedback.dx, movementFeedback.dy};
        auto keepCurrentTarget = [&](std::vector<control::Candidate>& list) {
            size_t best = list.size();
            double bestDistance = std::numeric_limits<double>::infinity();
            for (size_t i = 0; i < list.size(); ++i) {
                const double distance = g_flashPost.shotTargetDistance(list[i], latestMove);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = i;
                }
            }
            if (best == list.size()) list.clear();
            else list = {list[best]};
        };
        keepCurrentTarget(candidates);
        keepCurrentTarget(triggerCandidates);
        detectionFresh = detectionFrameFresh && !candidates.empty();
    }

    const auto now = std::chrono::steady_clock::now();
    const bool unlockYBeforeUpdate = hk.unlock_y &&
        (hk.unlock_y_delay_ms <= 0 ||
         (g_aim_delay_target_id >= 0 && g_aim_delay_started_ms > 0 &&
          nowMs() - g_aim_delay_started_ms >= std::clamp(hk.unlock_y_delay_ms, 0, 5000)));
    double dtSec = 0.0;
    double trackingDtSec = 0.0;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_first_tick)
        {
            g_last_tick = now;
            g_first_tick = false;
            configureController(snapshot, activeIdx, scopeCtlActive, secondaryCtlActive,
                                unlockYBeforeUpdate);
            g_controller->reset();
            g_target_aim_hotkey = -1;
            // Seed controller dt, but evaluate the trigger on this first
            // observation too. Trigger deadlines use the monotonic clock.
            dtSec = 1.0 / std::max(60, cfg.capture_fps);
        }
        else dtSec = std::chrono::duration<double>(now - g_last_tick).count();
        // Frozen second-port timing: E8/0x168 velocity follows capture-frame
        // timestamps, while the PID derivative/integral uses the control tick.
        trackingDtSec = dtSec;
        if (captureNs > 0) {
            if (g_last_capture_ns > 0 && captureNs > g_last_capture_ns) {
                const double frameDt = (captureNs - g_last_capture_ns) * 1e-9;
                if (dtIsUsable(frameDt)) trackingDtSec = frameDt;
            }
            if (captureNs > g_last_capture_ns) g_last_capture_ns = captureNs;
        }
    }
    if (!dtIsUsable(dtSec))
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_controller) g_controller->reset();
        g_last_capture_ns = 0;
        g_pidfFeedback.reset();
        g_aimpointRecoilGate.reset();
        g_target_aim_hotkey = -1;
        if (g_autoStop.continuous())
            if (MouseThread* mouse = ensureMouse(); mouse && mouse->maskRealKeyboard(0))
                g_autoStop.reset();
        g_last_tick = now;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_last_tick = now;
    }
    const int64_t aimNs = runtime::latency::markAimConsume(captureNs, publishNs);

    // Physical input is sampled outside g_mtx, matching the keyboard
    // listener's device-lock order. The trigger's accepted press is sampled
    // below under g_mtx alongside the controller state.
    const bool physicalFireHeld = hk.aimpoint_recoil_enabled &&
        isAnyKeyPressed({cfg.aimpoint_recoil_fire_key});

    if (MouseThread* mouse = ensureMouse(); mouse && mouse->weaponSwitch31Busy())
    {
        // Lua 切枪期间暂停整条瞄准/扳机链。清掉 PID 与轨迹余量，避免切回 1 后
        // 把切枪期间积累的误差一下子发出去；时钟仍逐拍更新。
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_controller) g_controller->reset();
        g_aimpointRecoilGate.reset();
        g_target_aim_hotkey = -1;
        if (g_autoStop.continuous() && mouse->maskRealKeyboard(0))
            g_autoStop.reset();
        g_path.reset();
        return false;
    }

    {
        // 只在【切档那一刻】打一行, 不刷屏 —— 用户要能确认它真的切了。
        if (scopeCtlActive != g_scopeCtlLast || secondaryCtlActive != g_secondaryCtlLast)
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (g_controller) g_controller->reset();
            g_path.reset();
            g_scopeCtlLast = scopeCtlActive;
            g_secondaryCtlLast = secondaryCtlActive;
            std::cout << (scopeCtlActive ? "[Aim] 开镜独立参数"
                       : secondaryCtlActive ? "[Aim] 第二套瞄准参数"
                                            : "[Aim] 默认瞄准参数")
                      << std::endl;
        }
    }

    control::ControlOutput out;
    boss::TriggerTarget triggerTarget;
    const bool recordReplay = runtime::ReplayBuffer::instance().enabled();
    std::vector<control::Candidate> replayCandidates;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (flashTurning) {
            if (g_controller) g_controller->reset();
            g_triggerTargetSelector.reset();
        }
        configureController(snapshot, activeIdx, scopeCtlActive, secondaryCtlActive,
                            unlockYBeforeUpdate);

        control::ControlInput in;
        in.macro=macros::readDirective();
        in.candidates = std::move(candidates);
        in.cross = cross;
        in.dtSec = dtSec;
        in.trackingDtSec = trackingDtSec;
        const int64_t frameUs = captureNs > 0 ? captureNs / 1000
            : std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()).count();
        for (const auto& event : movementFeedback.events)
            g_pidfFeedback.add({event.dx, event.dy, event.timestamp_us, event.source});
        const auto& feedbackPid = scopeCtlActive ? hk.recovered_scope_pid
            : secondaryCtlActive ? hk.recovered_secondary_pid : hk.recovered_pid;
        in.motionEventSum = g_pidfFeedback.sample(frameUs, true, feedbackPid.motionDelayMs);
        in.pendingMotionPx = g_pidfFeedback.pendingCorrection(frameUs,
            feedbackPid.motionDelayMs, feedbackPid.motionPixelsPerCountX,
            feedbackPid.motionPixelsPerCountY);
        in.observationTimeUs = frameUs;
        in.frameIndex = ++g_frame_index;
        in.detectionFresh = detectionFresh;
        in.crosshairFresh = crossFresh;
        in.autoFire = trigger.trigger_enabled;
        in.aimpointRecoilYpx = g_aimpointRecoilGate.update(
            hk.aimpoint_recoil_enabled,
            physicalFireHeld || (trigger.trigger_enabled && g_trigger.pressed()),
            nowMs(), cfg.aimpoint_recoil_speed_px_s, cfg.aimpoint_recoil_max_px);

        out = g_controller->update(in);
        const auto fireMode = trigger.trigger_weapon_switch31
            ? boss::AmFireMode::SmartClick
            : boss::amFireMode(trigger.trigger_fire_mode, trigger.trigger_fire_duration);
        if (g_trigger.configure(fireMode, trigger.trigger_loss_delay_ms))
            if (auto* device = ensureMouse()) completeShot(device);
        auto rules = hk.trigger_classes;
        if (rules.empty()) {
            for (const auto& candidate : triggerCandidates) {
                if (!hk.aim_classes.empty() && std::none_of(hk.aim_classes.begin(), hk.aim_classes.end(),
                    [&](const auto& rule) { return rule.class_id == candidate.classId; })) continue;
                if (std::any_of(rules.begin(), rules.end(), [&](const auto& rule) {
                    return rule.class_id == candidate.classId; })) continue;
                rules.push_back({candidate.classId, 0.5f, 0.5f,
                    trigger.trigger_y_percent, trigger.trigger_y_percent});
            }
        }
        triggerTarget = g_triggerTargetSelector.select(
            triggerCandidates, rules, cross, hk.fovX, hk.fovY,
            cfg.confidence_threshold, detectionFresh);
        if (recordReplay)
            replayCandidates = std::move(in.candidates);
    }

    // Start only when both conditions overlap. A lost or changed target starts
    // a new wait; the tracker continues to update, while PID carry is discarded.
    bool waitingForAim = false;
    bool clearPendingMoves = false;
    if (!out.engaged || !out.hasTarget) {
        g_target_aim_hotkey = -1;
        g_aim_delay_target_id = -1;
        g_aim_delay_started_ms = 0;
        g_aim_delay_ready = false;
    } else {
        const int64_t ms = nowMs();
        const int delayMs = std::clamp(hk.aim_delay_ms, 0, 2000);
        if (g_aim_delay_target_id != out.targetId || g_aim_delay_setting_ms != delayMs) {
            g_aim_delay_target_id = out.targetId;
            g_aim_delay_started_ms = ms;
            g_aim_delay_ready = false;
            g_aim_delay_setting_ms = delayMs;
            clearPendingMoves = delayMs > 0;
        }
        if (!g_aim_delay_ready && ms - g_aim_delay_started_ms < delayMs) {
            waitingForAim = true;
            out.counts = {};
            std::lock_guard<std::mutex> lk(g_mtx);
            g_controller->resetPidAxes(true, true);
            g_controller->resetCompensation();
        } else {
            g_aim_delay_ready = true;
            g_target_aim_updated_ms = ms;
            g_target_aim_hotkey = hk.ctl_enabled ? activeIdx : -1;
        }
        if (waitingForAim) g_target_aim_hotkey = -1;
    }
    if (clearPendingMoves)
        if (MouseThread* mouse = ensureMouse()) mouse->clearQueuedMoves();

    // Auto flash watches the same locked box as the aim controller. It queues
    // a short press on a different thread, so this control tick never sleeps.
    if (triggerMode != 0 || !cfg.auto_flash_enabled || cfg.auto_flash_key.empty() ||
        !out.engaged || !out.hasTarget || cfg.detection_resolution <= 0 ||
        out.targetBox.w <= 0.0 || out.targetBox.h <= 0.0) {
        resetAutoFlash();
    } else {
        const double threshold = std::clamp(cfg.auto_flash_area_percent, 0.1, 100.0);
        const double frameArea = static_cast<double>(cfg.detection_resolution) *
                                 cfg.detection_resolution;
        const double areaPercent = 100.0 * out.targetBox.w * out.targetBox.h / frameArea;
        if (g_flash_target_id != out.targetId || g_flash_hotkey != activeIdx ||
            g_flash_threshold != threshold || g_flash_key != cfg.auto_flash_key) {
            resetAutoFlash();
            g_flash_target_id = out.targetId;
            g_flash_hotkey = activeIdx;
            g_flash_threshold = threshold;
            g_flash_key = cfg.auto_flash_key;
        }
        if (areaPercent < threshold) {
            g_flash_above = false;
        } else if (!g_flash_above) {
            g_flash_above = true;
            const int64_t ms = nowMs();
            if (ms - g_flash_last_fire_ms >= 250) {
                macros::requestAutoFlash(cfg.auto_flash_key, activeIdx);
                g_flash_last_fire_ms = ms;
            }
        }
    }

    const bool unlockYActive = hk.unlock_y && out.engaged && out.hasTarget &&
        (hk.unlock_y_delay_ms <= 0 ||
         nowMs() - g_aim_delay_started_ms >= std::clamp(hk.unlock_y_delay_ms, 0, 5000));

    if (recordReplay)
    {
        runtime::ReplayFrame frame;
        frame.ts = now;
        frame.resolution = cfg.detection_resolution;
        frame.cross_x = cross.x;
        frame.cross_y = cross.y;
        frame.fov_radius_x = out.fovRadii.x;
        frame.fov_radius_y = out.fovRadii.y;
        frame.mask_x = hk.mask_x;
        frame.unlock_x = hk.unlock_x;
        frame.mask_y = hk.mask_y;
        frame.unlock_y = unlockYActive;
        frame.pivot_x = out.controlAnchor.x;
        frame.pivot_y = out.controlAnchor.y;
        frame.base_anchor_x = out.anchor.x;
        frame.base_anchor_y = out.anchor.y;
        frame.follow_strength_x = out.followStrength.x;
        frame.follow_strength_y = out.followStrength.y;
        frame.follow_motion_x = out.followMotion.x;
        frame.follow_motion_y = out.followMotion.y;
        frame.follow_preset_x = out.followPreset.x;
        frame.follow_preset_y = out.followPreset.y;
        frame.follow_state_x = out.followStateX;
        frame.follow_state_y = out.followStateY;
        frame.scope_params = scopeCtlActive;
        frame.secondary_params = secondaryCtlActive;
        frame.error_x = out.hasTarget ? out.controlAnchor.x - cross.x : 0.0;
        frame.error_y = out.hasTarget ? out.controlAnchor.y - cross.y : 0.0;
        frame.derivative_raw_x = out.derivativeRaw.x;
        frame.derivative_raw_y = out.derivativeRaw.y;
        frame.requested_dx = out.counts.x;
        frame.requested_dy = out.counts.y;
        // The feedback belongs to successful device sends since the previous tick.
        frame.mouse_dx = movementFeedback.dx;
        frame.mouse_dy = movementFeedback.dy;
        frame.hotkey_active = true;
        frame.locked_track_id = out.hasTarget ? out.targetId : -1;
        frame.boxes.reserve(replayCandidates.size() + (out.hasTarget ? 1 : 0));
        frame.class_ids.reserve(frame.boxes.capacity());
        frame.track_ids.reserve(frame.boxes.capacity());
        if (out.hasTarget)
        {
            frame.boxes.emplace_back(static_cast<int>(std::lround(out.targetBox.x)),
                                     static_cast<int>(std::lround(out.targetBox.y)),
                                     static_cast<int>(std::lround(out.targetBox.w)),
                                     static_cast<int>(std::lround(out.targetBox.h)));
            frame.class_ids.push_back(out.targetClassId);
            frame.track_ids.push_back(out.targetId);
        }
        for (const auto& candidate : replayCandidates)
        {
            frame.boxes.emplace_back(static_cast<int>(std::lround(candidate.box.x)),
                                     static_cast<int>(std::lround(candidate.box.y)),
                                     static_cast<int>(std::lround(candidate.box.w)),
                                     static_cast<int>(std::lround(candidate.box.h)));
            frame.class_ids.push_back(candidate.classId);
            frame.track_ids.push_back(-1);
        }
        runtime::ReplayBuffer::instance().push(std::move(frame));
    }

    // 预览跟踪框、瞄点和未输出原因；叠加只读，不参与控制。
    runtime::AimOverlayState s;
    {
        s.valid           = out.hasTarget;
        s.engaged         = out.engaged;
        s.box             = cv::Rect(static_cast<int>(std::lround(out.targetBox.x)),
                                     static_cast<int>(std::lround(out.targetBox.y)),
                                     static_cast<int>(std::lround(out.targetBox.w)),
                                     static_cast<int>(std::lround(out.targetBox.h)));
        s.filtered_cx     = out.filteredCenter.x;
        s.filtered_cy     = out.filteredCenter.y;
        s.anchor_x        = out.controlAnchor.x;
        s.anchor_y        = out.controlAnchor.y;
        s.base_anchor_x   = out.anchor.x;
        s.base_anchor_y   = out.anchor.y;
        s.base_error_x    = out.error.x;
        s.base_error_y    = out.error.y;
        s.cross_x = cross.x;
        s.cross_y = cross.y;
        s.fov_radius_x = out.fovRadii.x;
        s.fov_radius_y = out.fovRadii.y;
        s.mask_x = hk.mask_x;
        s.unlock_x = hk.unlock_x;
        s.mask_y = hk.mask_y;
        s.unlock_y = unlockYActive;
        s.follow_strength_x = out.followStrength.x;
        s.follow_strength_y = out.followStrength.y;
        s.follow_motion_x = out.followMotion.x;
        s.follow_motion_y = out.followMotion.y;
        s.follow_preset_x = out.followPreset.x;
        s.follow_preset_y = out.followPreset.y;
        s.follow_state_x = out.followStateX;
        s.follow_state_y = out.followStateY;
        s.target_id       = out.targetId;
        s.target_class_id = out.targetClassId;
        s.verdict         = static_cast<int>(out.lockState);
        s.idle_reason     = static_cast<int>(out.idleReason);
        s.scope_params    = scopeCtlActive;
        s.secondary_params = secondaryCtlActive;
        s.trigger_valid = trigger.trigger_enabled && triggerTarget.valid;
        s.trigger_in_zone = s.trigger_valid && triggerTarget.contains(cross);
        s.trigger_reason = !trigger.trigger_enabled
            ? runtime::TriggerOverlayReason::Disabled
            : !triggerTarget.valid ? runtime::TriggerOverlayReason::NoTarget
            : !s.trigger_in_zone ? runtime::TriggerOverlayReason::OutsideZone
            : runtime::TriggerOverlayReason::Ready;
        s.trigger_class_id = triggerTarget.classId;
        s.trigger_point_x = triggerTarget.point.x;
        s.trigger_point_y = triggerTarget.point.y;
        s.trigger_half_width = triggerTarget.halfWidth;
        s.trigger_half_height = triggerTarget.halfHeight;
        s.control_dt_ms   = dtSec * 1000.0;
        s.capture_age_ms  = captureNs > 0 && aimNs >= captureNs
            ? static_cast<double>(aimNs - captureNs) * 1e-6 : -1.0;
        s.derivative_raw_x = out.derivativeRaw.x;
        s.derivative_raw_y = out.derivativeRaw.y;
        runtime::publishAimOverlay(s);
    }

    MouseThread* mouse = ensureMouse();

    if (triggerMode != 0)
    {
        boss::TriggerFlashPostController::Settings settings;
        settings.mode = triggerMode;
        settings.pixelsPerCount = trigger.trigger_snap_px_per_count;
        settings.maxCounts = 500;
        settings.returnCountsPerSecond = std::max(12000, trigger.trigger_return_counts_per_second);
        settings.returnYPercent = trigger.trigger_return_y_percent;
        settings.spinCountsPerTurn = trigger.trigger_spin_counts_per_turn;
        settings.turnStepDegrees = trigger.trigger_spin_step_degrees;
        settings.turnDurationMs = trigger.trigger_spin_step_ms;
        settings.turnHoldMs = trigger.trigger_spin_hold_ms;
        settings.disappearMs = trigger.trigger_flash_disappear_ms;
        g_flashPost.configure(settings);

        // Ordinary aiming can send moves more frequently than new captures.
        // Requiring every observation to postdate the latest aim move would
        // starve the trigger of fresh frames. The stepped turn waits for
        // a capture after its own movement. A turn ignores detections until it
        // has finished and a settled post-turn capture becomes available.
        const auto flashPhase = g_flashPost.phase();
        const bool postTurnCaptureRequired = flashPhase == boss::TriggerFlashPostController::Phase::Return ||
            flashPhase == boss::TriggerFlashPostController::Phase::ReturnSettle ||
            flashPhase == boss::TriggerFlashPostController::Phase::Turn ||
            flashPhase == boss::TriggerFlashPostController::Phase::Settle ||
            flashPhase == boss::TriggerFlashPostController::Phase::Stopped;
        if (postTurnCaptureRequired)
            for (const auto& event : movementFeedback.events)
                g_flashRequireCaptureNs = std::max(g_flashRequireCaptureNs,
                                                   event.timestamp_us * 1000 + 20000000);
        else g_flashRequireCaptureNs = 0;
        const int64_t observationNs = captureNs > 0 ? captureNs : publishNs;
        const bool newFrame = detectionFrameFresh && publishNs > g_flashLastFrameNs &&
            (!postTurnCaptureRequired || observationNs > g_flashRequireCaptureNs);
        if (newFrame) g_flashLastFrameNs = publishNs;
        boss::TriggerFlashPostController::Input flashInput;
        flashInput.fresh = newFrame;
        flashInput.selected = triggerTarget.valid;
        flashInput.selectedClassId = triggerTarget.classId;
        flashInput.selectedBox = triggerTarget.box;
        flashInput.candidates = flashCandidates;
        for (const auto& candidate : flashCandidates) {
            if (!candidate.box.valid()) continue;
            bool allowed = false;
            if (!hk.trigger_classes.empty()) {
                allowed = std::any_of(hk.trigger_classes.begin(), hk.trigger_classes.end(),
                    [&](const TriggerAimClass& rule) { return rule.class_id == candidate.classId; });
            } else {
                allowed = std::any_of(hk.aim_classes.begin(), hk.aim_classes.end(),
                    [&](const HotkeyAimClass& rule) { return rule.class_id == candidate.classId; });
            }
            if (!allowed) continue;
            const auto center = candidate.box.center();
            if (std::abs(center.x - cross.x) <= hk.fovX * 0.5 + candidate.box.w * 0.5 &&
                std::abs(center.y - cross.y) <= hk.fovY * 0.5 + candidate.box.h * 0.5) {
                flashInput.scanCandidateVisible = true;
                break;
            }
        }
        flashInput.confirmedMove = {movementFeedback.dx, movementFeedback.dy};
        flashInput.feedbackArrived = !movementFeedback.events.empty();
        flashInput.nowMs = nowMs();
        const auto flash = g_flashPost.tick(flashInput);
        if (flash.cancelPendingMove && mouse) mouse->clearQueuedMoves();
        if (flash.blockNormal)
        {
            g_target_aim_hotkey = -1;
            releaseTargetButtons(scopeMode, trigger, flash.targetMissing);
            s.trigger_reason = runtime::TriggerOverlayReason::Cooldown;
            runtime::publishAimOverlay(s);
            if (mouse && (flash.move.x || flash.move.y))
                mouse->sendRawMove(flash.move.x, flash.move.y, captureNs, aimNs);
            return flash.move.x != 0 || flash.move.y != 0;
        }
    }

    const bool aimReady = hk.ctl_enabled && out.engaged && !waitingForAim;
    bool continuousTriggerActive = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        continuousTriggerActive = g_trigger.continuousActive();
    }
    const bool triggerReady = trigger.trigger_enabled &&
        (triggerTarget.valid || continuousTriggerActive);
    if (!aimReady && !triggerReady)
    {
        releaseTargetButtons(scopeMode, trigger, !triggerTarget.valid);
        return false;
    }

    int move_x = aimReady ? out.counts.x : 0;
    int move_y = aimReady ? out.counts.y : 0;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_pathMaskX != hk.unlock_x || g_pathMaskY != unlockYActive) {
            g_path.reset();
            if (mouse) mouse->clearQueuedMoves();
        }
        g_pathMaskX = hk.unlock_x;
        g_pathMaskY = unlockYActive;
        g_path.configure(pathParamsFrom(hk));
        if (aimReady && hk.aim_path_mode != 0)
        {
            const auto shaped = g_path.step(
                  hk.unlock_x ? cross.x : out.controlAnchor.x,
                  unlockYActive ? cross.y : out.controlAnchor.y,
                  cross.x,             cross.y,
                  dtSec,
                  out.targetId,
                  static_cast<double>(out.counts.x),
                  static_cast<double>(out.counts.y));
            move_x = static_cast<int>(std::lround(shaped.move_x));
            move_y = static_cast<int>(std::lround(shaped.move_y));
        }
    }
    // Path noise/curves must not reintroduce movement on a disabled axis.
    if (hk.unlock_x) move_x = 0;
    if (unlockYActive) move_y = 0;

    const int64_t ms = nowMs();
    if (mouse)
    {
        std::lock_guard<std::mutex> lk(g_mtx);

        bool inZone = false;
        if (trigger.trigger_enabled)
            inZone = triggerTarget.contains(cross);

        const bool rightHotkey = std::any_of(
            hk.keys.begin(), hk.keys.end(),
            [](const std::string& k) { return k == "RightMouseButton"; });
        const bool scopeAllowed = !rightHotkey;
        // scopeMode 已在上面(切档判定)算过, 这里复用同一个值。

        {
            const bool switchCapable = trigger.trigger_weapon_switch31 &&
                mouse->supports(mouse_driver::kCapKeyboard);
            if (trigger.trigger_weapon_switch31 && !switchCapable &&
                !g_warnedSwitch31Unavailable)
            {
                std::cerr << "[Switch31] keyboard device unavailable; firing without weapon switch."
                          << std::endl;
                g_warnedSwitch31Unavailable = true;
            }
            if (!trigger.trigger_weapon_switch31 || switchCapable)
                g_warnedSwitch31Unavailable = false;

            const bool switchBusy = mouse->weaponSwitch31Busy();
            const bool switchPulse = switchCapable || g_pendingSwitch31.armed;
            const bool holdMode = g_trigger.holding() && !switchPulse;
            const bool effectiveInZone = g_trigger.holdZoneOnBriefMiss(
                inZone, triggerTarget.trackId, ms,
                trigger.trigger_enabled && holdMode && !switchBusy
                    ? trigger.trigger_loss_delay_ms : 0);
            const bool shotZone = effectiveInZone && !switchBusy;
            g_prearm.reset();
            const bool scopeZone = shotZone;
            // AM tap opening uses press duration (minimum 30 ms); holding
            // right has no extra delay before the left press.
            const int scopeDelay = scopeMode == 1 ? trigger.trigger_fire_duration : 0;
            const int previousScopeMode = g_scope.mode();
            const auto scopeAct = g_scope.tick(scopeZone,
                                               scopeAllowed && trigger.trigger_enabled, scopeMode,
                                               scopeDelay, ms, trigger.trigger_delay_jitter_ms);
            if (previousScopeMode != scopeMode) {
                g_prearm.reset();
                if (g_trigger.reset()) completeShot(mouse);
            }
            if (scopeAct.release_right && !mouse->releaseRightButton())
                g_scope.retryTapRelease(ms);
            if (scopeAct.press_right)
            {
                if (mouse->pressRightButton())
                {
                    // 只在右键命令成功下发后记录开镜状态。
                    if (scopeMode == 1) g_scopeTapped = true;
                }
                else
                {
                    const auto rollback = g_scope.forceRelease();
                    if (rollback.release_right) mouse->releaseRightButton();
                }
            }

            const bool scopeReady = g_scope.ready(scopeAllowed, scopeMode, scopeDelay, ms);
            if (inZone && !switchBusy && !scopeReady)
                s.trigger_reason = runtime::TriggerOverlayReason::ScopeWait;

            // 开火后即使用户临时关掉切枪或键盘断开，本次已按下的左键仍按短按
            // 语义完成，不能中途退回「长按直到离区」。
            const int fireDuration = switchPulse
                ? std::max(trigger.trigger_fire_duration, 20)
                : trigger.trigger_fire_duration;
            const bool methodOk = cfg.input_method == "MAKCU" ||
                                  cfg.input_method == "MAKCUNEW" ||
                                  cfg.input_method == "KMBOXNET" ||
                                  cfg.input_method == "FERRUM" ||
                                  cfg.input_method == "DHZBOX_MINI";
            const bool maskCapable = methodOk &&
                mouse->supports(mouse_driver::kCapKeyboardMask);
            const bool timedStopEnabled = trigger.trigger_auto_stop == 1 && maskCapable;
            const bool continuousStopEnabled = trigger.trigger_auto_stop == 2 && maskCapable;
            const bool continuousWanted = continuousStopEnabled && trigger.trigger_enabled &&
                shotZone && !g_pendingSwitch31.armed;
            if (g_autoStop.continuous() && !continuousWanted &&
                mouse->maskRealKeyboard(0))
                g_autoStop.reset();
            if (continuousWanted)
            {
                if (g_autoStop.active() && !g_autoStop.continuous() &&
                    mouse->maskRealKeyboard(0))
                    g_autoStop.reset();
                if (mouse->keyboardMaskExpires() &&
                    g_autoStop.continuousNeedsRefresh(ms) &&
                    mouse->maskRealKeyboard(0))
                    g_autoStop.reset();
                if (!g_autoStop.active() && mouse->maskRealKeyboard(2000))
                    g_autoStop.markContinuous(ms);
            }
            const int stopBeforeMs = timedStopEnabled
                ? std::clamp(trigger.trigger_stop_before_ms, 0, 1000) : 0;
            const int stopAfterMs = timedStopEnabled
                ? std::clamp(trigger.trigger_stop_after_ms, 0, 1000) : 0;

            boss::AmTriggerFsm::Input tin;
            tin.in_zone  = inZone && !switchBusy; // Keep the raw hit so grace has one fixed deadline.
            tin.prerequisite_ready = scopeReady;
            tin.track_id = triggerTarget.trackId;
            tin.now_ms   = ms;

            boss::AmTriggerFsm::Action tAct;
            if (trigger.trigger_enabled)
            {
                tAct = g_trigger.tick(tin, holdMode,
                    trigger.trigger_fire_delay, fireDuration,
                    trigger.trigger_fire_interval,
                    trigger.trigger_switch_cooldown_ms,
                    trigger.trigger_delay_jitter_ms,
                    switchPulse ? 0 : trigger.trigger_duration_jitter_ms,
                    trigger.trigger_interval_jitter_ms, stopBeforeMs,
                    0);
            }
            else if (g_trigger.reset())
                completeShot(mouse);

            if ((!timedStopEnabled || !trigger.trigger_enabled ||
                 g_trigger.phase() != boss::TriggerPhase::Delay) &&
                !tAct.fired &&
                g_autoStop.preparing())
            {
                g_autoStop.markFired(ms, 0);
                if (mouse->maskRealKeyboard(0)) g_autoStop.reset();
            }

            if (tAct.prepare_fire && timedStopEnabled)
            {
                // Firmware does not extend an active mask window. Replace it
                // if a new shot begins during the previous post-shot window.
                if (g_autoStop.active() && mouse->maskRealKeyboard(0))
                    g_autoStop.reset();
                if (!g_autoStop.active() && mouse->maskRealKeyboard(2000))
                    g_autoStop.markPrepared();
            }
            if (tAct.fired && timedStopEnabled && stopBeforeMs == 0)
            {
                if (!g_autoStop.preparing())
                {
                    if (g_autoStop.active() && mouse->maskRealKeyboard(0))
                        g_autoStop.reset();
                    if (!g_autoStop.active() && mouse->maskRealKeyboard(2000))
                        g_autoStop.markPrepared();
                }
            }

            if (tAct.release_left)
                completeShot(mouse);
            const bool stopReady = (!timedStopEnabled || g_autoStop.preparing()) &&
                                   (!continuousStopEnabled || g_autoStop.continuous());
            const bool sentShot = tAct.press_left && stopReady &&
                                  !mouse->weaponSwitch31Busy() &&
                                  mouse->pressLeftButton();
            if (trigger.trigger_enabled && inZone && !switchBusy && scopeReady)
            {
                if (sentShot)
                    s.trigger_reason = runtime::TriggerOverlayReason::Pressed;
                else if (tAct.press_left)
                    s.trigger_reason = !stopReady
                        ? runtime::TriggerOverlayReason::AutoStopWait
                        : mouse->weaponSwitch31Busy()
                            ? runtime::TriggerOverlayReason::SwitchBusy
                            : runtime::TriggerOverlayReason::DriverRejected;
                else if (g_trigger.pressed())
                    s.trigger_reason = runtime::TriggerOverlayReason::Pressed;
                else if (g_trigger.phase() == boss::TriggerPhase::Delay)
                    s.trigger_reason = runtime::TriggerOverlayReason::FirstShotDelay;
                else if (g_trigger.phase() == boss::TriggerPhase::Cooldown ||
                         g_trigger.phase() == boss::TriggerPhase::SwitchCooldown)
                    s.trigger_reason = runtime::TriggerOverlayReason::Cooldown;
            }
            if (tAct.press_left && !sentShot)
            {
                // 状态机提出开枪请求不等于驱动已经发出左键。失败时撤销
                // 本次射击准备，不能留下急停，也不能把它算成切枪资格。
                g_trigger.reset();
                if (g_autoStop.preparing() && mouse->maskRealKeyboard(0))
                    g_autoStop.reset();
            }
            if (sentShot && timedStopEnabled && g_autoStop.preparing())
                g_autoStop.markFired(nowMs(), stopAfterMs);
            if (sentShot && g_autoStop.shouldCancel(nowMs()) &&
                mouse->maskRealKeyboard(0))
                g_autoStop.reset();
            if (sentShot)
            {
                if (captureOnTrigger)
                    AutoCapture::notify_trigger(detectedFrame, std::move(triggerCaptureDetections));
                if (triggerMode != 0)
                    g_flashPost.shotSent(triggerTarget.classId, triggerTarget.box);
                g_pendingSwitch31.onPress(true, switchCapable,
                    trigger.trigger_switch31_delay_ms, fireDuration, nowMs());
                if (switchCapable)
                {
                    // A completed shot owns its release and keyboard switch.
                    // Do not wait for another detector tick: target loss,
                    // hotkey release or a new target can otherwise interrupt
                    // this cycle before the keyboard job is submitted.
                    completeShot(mouse);
                    g_trigger.reset();
                }
            }
        }
    }
    else if (s.trigger_in_zone)
        s.trigger_reason = runtime::TriggerOverlayReason::NoDriver;
    runtime::publishAimOverlay(s);

    if (MouseThread* mouse = ensureMouse(); mouse && mouse->weaponSwitch31Busy())
    {
        g_target_aim_hotkey = -1;
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_controller) g_controller->reset();
        if (g_autoStop.continuous() && mouse->maskRealKeyboard(0))
            g_autoStop.reset();
        g_path.reset();
        return false;
    }

    if (move_x == 0 && move_y == 0)
        return false;

    if (MouseThread* mouse = ensureMouse())
        mouse->sendRawMove(move_x, move_y, captureNs, aimNs);
    return true;
}

void reset()
{
    macros::DevicePause macroPause;
    std::lock_guard<std::recursive_mutex> outputLock(macros::outputMutex());
    runtime::FfCalibrationSession::instance().cancel("运行状态已重置，标定取消");
    g_wasCalibrating = false;
    g_crosshairHold.reset();
    g_target_aim_hotkey = -1;
    g_aim_delay_target_id = -1;
    g_aim_delay_started_ms = 0;
    g_aim_delay_ready = false;
    g_aim_delay_setting_ms = 0;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_controller)
            g_controller->reset();
        g_aimpointRecoilGate.reset();
        g_appliedConfigSnapshot.reset();
        g_appliedConfigHotkey = -1;
        g_appliedConfigScope = false;
        g_appliedConfigUnlockY = false;
        g_path.reset();
        g_triggerTargetSelector.reset();
        g_trigger.reset();
        g_prearm.reset();
        g_flashPost.reset();
        g_lastTriggerMode = 0;
        g_flashLastFrameNs = g_flashRequireCaptureNs = 0;
        g_scope.forceRelease();
        if (g_autoStop.active())
            if (MouseThread* mouse = ensureMouse()) mouse->maskRealKeyboard(0);
        g_autoStop.reset();
        g_first_tick = true;
        g_last_capture_ns = 0;
        g_pidfFeedback.reset();
        g_frame_index = 0;
        g_last_active_hotkey = -1;
        g_hotkey_activated_ms = 0;
        g_last_tick = std::chrono::steady_clock::time_point{};
        g_scopeCtlLast = false;
        g_secondaryCtlLast = false;
        g_secondaryTriggerLast = false;
        g_scopeTapped = false;
        g_pendingSwitch31.cancel();
        g_warnedSwitch31Unavailable = false;
    }
    resetMouse();
}

void resetMouse()
{
    std::lock_guard<std::recursive_mutex> outputLock(macros::outputMutex());
    runtime::FfCalibrationSession::instance().cancel("鼠标设备已重置，标定取消");
    std::lock_guard<std::mutex> stateLock(g_mtx);
    g_aimpointRecoilGate.reset();
    g_flashPost.reset();
    g_flashLastFrameNs = g_flashRequireCaptureNs = 0;
    if (g_controller) g_controller->resetCompensation();
    g_pendingSwitch31.cancel();
    std::lock_guard<std::mutex> lk(g_mouse_mtx);
    if (g_mouse)
    {
        if (g_autoStop.active())
            g_mouse->maskRealKeyboard(0);
        g_mouse->releaseLeftButton();
        g_mouse->releaseRightButton();
        g_mouse->clearQueuedMoves();
        g_mouse.reset();
    }
    g_autoStop.reset();
}

bool active()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_controller != nullptr;
}

int activeTargetHotkey()
{
    const int index = g_target_aim_hotkey.load();
    if (index < 0 || nowMs() - g_target_aim_updated_ms.load() > 100) return -1;
    return index;
}

bool prepareForMacro()
{
    std::lock_guard<std::recursive_mutex> outputLock(macros::outputMutex());
    g_target_aim_hotkey = -1;
    g_aim_delay_target_id = -1;
    g_aim_delay_started_ms = 0;
    g_aim_delay_ready = false;
    std::lock_guard<std::mutex> lock(g_mtx);
    auto* mouse = ensureMouse();
    if (mouse && mouse->weaponSwitch31Busy()) return false;
    const bool left = g_trigger.reset();
    g_prearm.reset();
    g_flashPost.reset();
    g_flashLastFrameNs = g_flashRequireCaptureNs = 0;
    const auto scope = g_scope.forceRelease();
    if (mouse) {
        mouse->suspendAutomaticMoves(true);
        if (left) mouse->releaseLeftButton();
        if (scope.release_right) mouse->releaseRightButton();
        if (g_autoStop.active()) mouse->maskRealKeyboard(0);
    }
    g_pendingSwitch31.cancel();
    g_autoStop.reset();
    g_path.reset();
    g_aimpointRecoilGate.reset();
    g_scopeTapped = false;
    return true;
}

void finishMacroControl(bool moved)
{
    std::lock_guard<std::recursive_mutex> outputLock(macros::outputMutex());
    std::lock_guard<std::mutex> lock(g_mtx);
    // A macro move is outside the tracking feedback chain. Reacquire rather
    // than interpreting the displaced image as target velocity on resumption.
    if (g_controller) {
        if (moved) g_controller->reset();
        else g_controller->seedPidDerivativeAfterPause();
    }
    if (moved) g_first_tick = true;
    g_last_tick = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> mouseLock(g_mouse_mtx);
    if (g_mouse) {
        g_mouse->consumeMovementFeedback();
        g_mouse->suspendAutomaticMoves(false);
    }
}

void resetPidAxes(bool x, bool y)
{
    std::lock_guard<std::recursive_mutex> outputLock(macros::outputMutex());
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_controller) g_controller->resetPidAxes(x,y);
}

}
