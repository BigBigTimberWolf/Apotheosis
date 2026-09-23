#include "config/config_bridge.h"
#include "config/ConfigManager.h"

#include <QSignalBlocker>
#include <QTimer>
#include <mutex>
#include <string>

#include "Apotheosis.h"
#include "config.h"
#include "capture.h"
#include "runtime/inference_session.h"
#include "runtime/config_snapshot.h"

extern std::atomic<bool> detector_model_changed;

ConfigBridge::ConfigBridge() : QObject(nullptr) {
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(400);
    connect(m_saveTimer, &QTimer::timeout, this, &ConfigBridge::onSaveTimeout);

    connect(&ConfigManager::instance(), &ConfigManager::configChanged,
            this, &ConfigBridge::syncToRuntime);
}

ConfigBridge& ConfigBridge::instance() {
    static ConfigBridge s;
    return s;
}

void ConfigBridge::markDirty() {
    runtime_config::publish();
    if (!m_saveTimer->isActive())
        m_saveTimer->start();
}

void ConfigBridge::flush() {
    if (m_saveTimer->isActive())
        m_saveTimer->stop();
    onSaveTimeout();
}

void ConfigBridge::onSaveTimeout() {
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    config.saveConfig();
}

void ConfigBridge::syncToRuntime() {
    auto& cm = ConfigManager::instance();
    std::lock_guard<std::recursive_mutex> lk(configMutex);

    auto qs = [](const QString& s) { return s.toStdString(); };

    const std::string oldCaptureDevice = config.capture_device;
    const std::string oldCaptureFormat = config.capture_format;
    const int  oldCaptureWidth  = config.capture_width;
    const int  oldCaptureHeight = config.capture_height;
    const int  oldCaptureFps    = config.capture_fps;
    const bool oldCaptureGpu    = config.capture_gpu_decode;

    config.capture_device     = qs(cm.captureDevice());
    config.capture_format     = qs(cm.captureFormat());
    config.capture_width      = cm.captureWidth();
    config.capture_height     = cm.captureHeight();
    config.capture_fps        = cm.captureFps();
    config.capture_gpu_decode = cm.captureGpuDecode();

    const int oldDetRes = config.detection_resolution;

    std::string oldInput = config.input_method;
    config.input_method      = qs(cm.inputMethod());
    config.makcu_baudrate = cm.makcuBaudrate();
    config.makcu_port     = qs(cm.makcuPort());
    config.makcu_new_baudrate = cm.makcuNewBaudrate();
    config.makcu_new_port     = qs(cm.makcuNewPort());
    config.makcu_new_baudrate_kbd = cm.makcuNewBaudrateKbd();
    config.makcu_new_port_kbd     = qs(cm.makcuNewPortKbd());
    config.kmbox_net_ip       = qs(cm.kmboxNetIp());
    config.kmbox_net_port     = qs(cm.kmboxNetPort());
    config.kmbox_net_uuid     = qs(cm.kmboxNetUuid());
    std::string oldModel = config.ai_model;
    const std::string oldPrecision = config.engine_precision;
    config.backend              = "TRT";
    config.ai_model             = qs(cm.aiModel());
    config.engine_precision     = qs(cm.enginePrecision());
    config.int8_calib_dir       = qs(cm.int8CalibDir());
    config.confidence_threshold = cm.confidenceThreshold();
    config.nms_threshold        = cm.nmsThreshold();
    config.max_detections       = kFixedMaxDetections;
    config.small_target_enabled    = cm.smallTargetEnabled();
    config.small_target_confidence = cm.smallTargetConfidence();
    config.small_target_area_frac  = cm.smallTargetAreaFrac();

    config.macro_enabled = cm.macroEnabled();
    config.macro_script_path = qs(cm.macroScriptPath());
    config.macro_primary_button_events = cm.macroPrimaryButtonEvents();

    config.crosshair_rect_w         = cm.crosshairRectW();
    config.crosshair_rect_h         = cm.crosshairRectH();
    config.crosshair_min_pixel_count = cm.crosshairMinPixelCount();
    config.crosshair_close_radius   = cm.crosshairCloseRadius();

    {
        auto qcolors = cm.crosshairColors();
        config.crosshair_colors.clear();
        for (const auto& qc : qcolors) {
            CrosshairColorProfileConfig c;
            c.name    = qs(qc.name);
            c.enabled = qc.enabled;
            c.h_low   = qc.hLow;
            c.h_high  = qc.hHigh;
            c.s_min   = qc.sMin;
            c.s_max   = qc.sMax;
            c.v_min   = qc.vMin;
            c.v_max   = qc.vMax;
            config.crosshair_colors.push_back(c);
        }
    }

    config.show_fps   = cm.showFps();
    config.verbose    = cm.verbose();
    config.screenshot_delay = cm.screenshotDelay();
    config.screenshot_button = { cm.screenshotButton().toStdString() };
    config.show_window      = cm.showWindow();
    config.replay_record_enabled = cm.replayRecordEnabled();
    config.replay_seconds        = cm.replaySeconds();
    config.replay_playback_speed = cm.replayPlaybackSpeed();

    // 全局选靶与稳定器
    config.target_hysteresis_ratio   = cm.targetHysteresisRatio();
    config.target_max_distance_px    = cm.targetMaxDistancePx();
    config.target_match_center_ratio = cm.targetMatchCenterRatio();
    config.target_area_ratio_tol     = cm.targetAreaRatioTol();
    config.target_k_snap_mult        = cm.targetKSnapMult();
    config.target_min_aspect         = cm.targetMinAspect();
    config.target_max_aspect         = cm.targetMaxAspect();

    const bool captureDeviceChanged =
        config.capture_device != oldCaptureDevice
        || config.capture_format != oldCaptureFormat
        || config.capture_width  != oldCaptureWidth
        || config.capture_height != oldCaptureHeight
        || config.capture_fps    != oldCaptureFps
        || config.capture_gpu_decode != oldCaptureGpu;
    if (captureDeviceChanged)
        capture_method_changed = true;
    if (config.detection_resolution != oldDetRes)
        detection_resolution_changed = true;
    if (config.capture_fps != oldCaptureFps)
        capture_fps_changed = true;
    if (config.ai_model != oldModel || config.engine_precision != oldPrecision) {
        // 精度变了也要重建引擎 —— .engine 是按精度烘死的, 只换模型名不够。
        detector_model_changed = true;
        std::string model_path = "models/" + config.ai_model;
        runtime::preload_model_metadata(model_path, false);
    }
    if (config.input_method != oldInput)
        input_method_changed = true;

    markDirty();
}

void ConfigBridge::syncFromRuntime()
{
    auto& cm = ConfigManager::instance();
    std::lock_guard<std::recursive_mutex> lk(configMutex);

    auto qstr = [](const std::string& s) { return QString::fromStdString(s); };

    QSignalBlocker blocker(&cm);

    cm.setCaptureDevice(qstr(config.capture_device));
    cm.setCaptureFormat(qstr(config.capture_format));
    cm.setCaptureWidth(config.capture_width);
    cm.setCaptureHeight(config.capture_height);
    cm.setCaptureFps(config.capture_fps);
    cm.setCaptureGpuDecode(config.capture_gpu_decode);
    cm.setDetectionResolution(config.detection_resolution);
    cm.setCircleMask(config.circle_mask);

    cm.setInputMethod(qstr(config.input_method));
    cm.setMakcuBaudrate(config.makcu_baudrate);
    cm.setMakcuPort(qstr(config.makcu_port));
    cm.setMakcuNewBaudrate(config.makcu_new_baudrate);
    cm.setMakcuNewPort(qstr(config.makcu_new_port));
    cm.setMakcuNewBaudrateKbd(config.makcu_new_baudrate_kbd);
    cm.setMakcuNewPortKbd(qstr(config.makcu_new_port_kbd));
    cm.setKmboxNetIp(qstr(config.kmbox_net_ip));
    cm.setKmboxNetPort(qstr(config.kmbox_net_port));
    cm.setKmboxNetUuid(qstr(config.kmbox_net_uuid));
    cm.setAiModel(qstr(config.ai_model));
    cm.setEnginePrecision(qstr(config.engine_precision));
    cm.setInt8CalibDir(qstr(config.int8_calib_dir));
    cm.setConfidenceThreshold(config.confidence_threshold);
    cm.setNmsThreshold(config.nms_threshold);
    cm.setMaxDetections(kFixedMaxDetections);
    cm.setSmallTargetEnabled(config.small_target_enabled);
    cm.setSmallTargetConfidence(config.small_target_confidence);
    cm.setSmallTargetAreaFrac(config.small_target_area_frac);

    cm.setMacroEnabled(config.macro_enabled);
    cm.setMacroScriptPath(qstr(config.macro_script_path));
    cm.setMacroPrimaryButtonEvents(config.macro_primary_button_events);

    cm.setCrosshairRectW(config.crosshair_rect_w);
    cm.setCrosshairRectH(config.crosshair_rect_h);
    cm.setCrosshairMinPixelCount(config.crosshair_min_pixel_count);
    cm.setCrosshairCloseRadius(config.crosshair_close_radius);
    {
        QList<ConfigManager::ColorProfile> qcolors;
        for (const auto& c : config.crosshair_colors) {
            ConfigManager::ColorProfile qc;
            qc.name    = qstr(c.name);
            qc.enabled = c.enabled;
            qc.hLow    = c.h_low;
            qc.hHigh   = c.h_high;
            qc.sMin    = c.s_min;
            qc.sMax    = c.s_max;
            qc.vMin    = c.v_min;
            qc.vMax    = c.v_max;
            qcolors.append(qc);
        }
        cm.setCrosshairColors(qcolors);
    }
    cm.setShowFps(config.show_fps);
    cm.setVerbose(config.verbose);
    cm.setScreenshotDelay(config.screenshot_delay);
    cm.setScreenshotButton(config.screenshot_button.empty()
        ? QStringLiteral("None")
        : QString::fromStdString(config.screenshot_button.front()));
    cm.setShowWindow(config.show_window);
    cm.setReplayRecordEnabled(config.replay_record_enabled);
    cm.setReplaySeconds(config.replay_seconds);
    cm.setReplayPlaybackSpeed(config.replay_playback_speed);

    // 全局选靶与稳定器
    cm.setTargetHysteresisRatio(config.target_hysteresis_ratio);
    cm.setTargetMaxDistancePx(config.target_max_distance_px);
    cm.setTargetMatchCenterRatio(config.target_match_center_ratio);
    cm.setTargetAreaRatioTol(config.target_area_ratio_tol);
    cm.setTargetKSnapMult(config.target_k_snap_mult);
    cm.setTargetMinAspect(config.target_min_aspect);
    cm.setTargetMaxAspect(config.target_max_aspect);

    cm.setActiveHotkeyGroup(qstr(config.active_hotkey_group));

    for (int i = static_cast<int>(config.hotkeys.size()); i < cm.hotkeyCount(); )
        cm.removeHotkey(cm.hotkeyCount() - 1);
    for (int i = 0; i < static_cast<int>(config.hotkeys.size()); ++i) {
        const auto& hp = config.hotkeys[i];
        ConfigManager::HotkeyData hd;
        hd.name = qstr(hp.name);
        hd.group = qstr(hp.group);
        hd.keys.clear();
        for (const auto& k : hp.keys)
            hd.keys.push_back(qstr(k));
        hd.fovX = hp.fovX;
        hd.fovY = hp.fovY;
        {
            QString joined;
            for (size_t ai = 0; ai < hp.aim_classes.size(); ++ai) {
                if (ai > 0) joined.append(';');
                const auto& ac = hp.aim_classes[ai];
                joined.append(QString::number(ac.class_id));
                joined.append(':');
                joined.append(QString::number(ac.y_offset, 'f', 3));
                joined.append(':');
                joined.append(QString::number(ac.y_offset_max, 'f', 3));
                joined.append(':');
                joined.append(QString::number(ac.min_conf, 'f', 3));
            }
            hd.aimClasses = joined;
        }
        hd.crosshairDetectEnabled  = hp.crosshair_detect_enabled;
        hd.dynamicFovEnabled  = hp.dynamic_fov_enabled;
        hd.dynamicFovStrength = hp.dynamic_fov_strength;
        hd.ctlEnabled          = hp.ctl_enabled;
        hd.ctlKpX              = hp.ctl_kp_x;
        hd.ctlKpY              = hp.ctl_kp_y;
        hd.ctlKiX              = hp.ctl_ki_x;
        hd.ctlKiY              = hp.ctl_ki_y;
        hd.ctlKdX              = hp.ctl_kd_x;
        hd.ctlKdY              = hp.ctl_kd_y;
        hd.ctlTauUnwindSec     = hp.ctl_tau_unwind_sec;
        hd.ctlTauDerivSec      = hp.ctl_tau_deriv_sec;
        hd.ctlIMax             = hp.ctl_i_max;
        hd.ctlMaxOutputCounts  = hp.ctl_max_output_counts;
        hd.ctlPFullScalePx     = hp.ctl_p_full_scale_px;
        hd.ctlPredictLeadMs             = hp.ctl_predict_lead_ms;
        hd.ctlPredictMaxVelocityPxPerSec = hp.ctl_predict_max_velocity_px_s;
        hd.ctlPredictMaxLeadRatio       = hp.ctl_predict_max_lead_ratio;
        hd.ctlKPxPerCount      = hp.ctl_k_px_per_count;
        hd.ctlInflightBeta     = hp.ctl_inflight_beta;
        hd.ctlInflightDeadTimeMs = hp.ctl_inflight_dead_time_ms;
        hd.ctlYOffset          = hp.ctl_y_offset;
        hd.ctlYOffsetMax       = hp.ctl_y_offset_max;
        hd.ctlHysteresisRatio  = hp.ctl_hysteresis_ratio;
        hd.ctlMaxDistancePx    = hp.ctl_max_distance_px;
        hd.ctlRandomSeed       = hp.ctl_random_seed;
        hd.ctlMatchCenterRatio = hp.ctl_match_center_ratio;
        hd.ctlAreaRatioTol     = hp.ctl_area_ratio_tol;
        hd.ctlKSnapMult        = hp.ctl_k_snap_mult;
        hd.ctlMinAspect        = hp.ctl_min_aspect;
        hd.ctlMaxAspect        = hp.ctl_max_aspect;
        if (i < cm.hotkeyCount())
            cm.setHotkey(i, hd);
        else
            cm.addHotkey(hd);
    }
}
