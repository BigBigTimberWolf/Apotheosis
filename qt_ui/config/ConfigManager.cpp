#include "config/ConfigManager.h"

#include "config.h"   // kFixedMaxDetections

#include <QFileInfo>
#include <algorithm>

ConfigManager::ConfigManager()
    : QObject(nullptr) {}

ConfigManager& ConfigManager::instance() {
    static ConfigManager s;
    return s;
}

bool ConfigManager::load(const QString& path) {
    m_path = path;
    delete m_settings;
    m_settings = new QSettings(m_path + ".cache", QSettings::IniFormat, this);

    if (!QFileInfo::exists(m_path)) {
        setDetectionResolution(320);
        setCaptureFps(60);
        setCircleMask(false);

        setAiModel("sunxds_0.5.6.engine");
        setConfidenceThreshold(0.10f);
        setNmsThreshold(0.50f);
        setMaxDetections(kFixedMaxDetections);

        setInputMethod("MAKCU");

        HotkeyData hk;
        hk.name = "Aim";
        hk.keys = {"RightMouseButton"};
        addHotkey(hk);

        save();
    }

    emit configLoaded();
    return true;
}

bool ConfigManager::save() {
    return true;
}

QString ConfigManager::configPath() const {
    return m_path;
}

void ConfigManager::notifyRuntimeReloaded() {
    emit configLoaded();
}

QString ConfigManager::captureDevice() const {
    return m_settings->value("Capture/capture_device", "").toString();
}

void ConfigManager::setCaptureDevice(const QString& v) {
    m_settings->setValue("Capture/capture_device", v);
    emit configChanged();
}

QString ConfigManager::captureSource() const {
    return m_settings->value("Capture/capture_source", "device").toString();
}

void ConfigManager::setCaptureSource(const QString& v) {
    m_settings->setValue("Capture/capture_source", v);
    emit configChanged();
}

QString ConfigManager::captureStreamUrl() const {
    return m_settings->value("Capture/capture_stream_url", "").toString();
}

QString ConfigManager::captureNdiSource() const {
    return m_settings->value("Capture/capture_ndi_source", "").toString();
}

void ConfigManager::setCaptureNdiSource(const QString& v) {
    m_settings->setValue("Capture/capture_ndi_source", v);
    emit configChanged();
}

QString ConfigManager::captureDxgiOutput() const {
    return m_settings->value("Capture/capture_dxgi_output", "").toString();
}

void ConfigManager::setCaptureDxgiOutput(const QString& v) {
    m_settings->setValue("Capture/capture_dxgi_output", v);
    emit configChanged();
}

void ConfigManager::setCaptureStreamUrl(const QString& v) {
    m_settings->setValue("Capture/capture_stream_url", v);
    emit configChanged();
}

QString ConfigManager::captureFormat() const {
    return m_settings->value("Capture/capture_format", "").toString();
}

void ConfigManager::setCaptureFormat(const QString& v) {
    m_settings->setValue("Capture/capture_format", v);
    emit configChanged();
}

int ConfigManager::captureWidth() const {
    return m_settings->value("Capture/capture_width", 0).toInt();
}

void ConfigManager::setCaptureWidth(int v) {
    m_settings->setValue("Capture/capture_width", v);
    emit configChanged();
}

int ConfigManager::captureHeight() const {
    return m_settings->value("Capture/capture_height", 0).toInt();
}

void ConfigManager::setCaptureHeight(int v) {
    m_settings->setValue("Capture/capture_height", v);
    emit configChanged();
}

int ConfigManager::captureFps() const {
    return m_settings->value("Capture/capture_fps", 0).toInt();
}

void ConfigManager::setCaptureFps(int v) {
    m_settings->setValue("Capture/capture_fps", v);
    emit configChanged();
}

bool ConfigManager::captureGpuDecode() const {
    return m_settings->value("Capture/capture_gpu_decode", true).toBool();
}

void ConfigManager::setCaptureGpuDecode(bool v) {
    m_settings->setValue("Capture/capture_gpu_decode", v);
    emit configChanged();
}

int ConfigManager::detectionResolution() const {
    return m_settings->value("Capture/detection_resolution", 320).toInt();
}

void ConfigManager::setDetectionResolution(int v) {
    m_settings->setValue("Capture/detection_resolution", v);
    emit configChanged();
}

bool ConfigManager::circleMask() const {
    return m_settings->value("Capture/circle_mask", false).toBool();
}

void ConfigManager::setCircleMask(bool v) {
    m_settings->setValue("Capture/circle_mask", v);
    emit configChanged();
}

QString ConfigManager::inputMethod() const {
    const QString value = m_settings->value("Hardware/input_method", "MAKCU").toString();
    if (value == QStringLiteral("MAKCUNEW") || value == QStringLiteral("KMBOXNET") ||
        value == QStringLiteral("FERRUM") || value == QStringLiteral("DHZBOX_MINI") || value == QStringLiteral("WINDOWS") || value == QStringLiteral("CAT"))
        return value;
    return QStringLiteral("MAKCU");
}

void ConfigManager::setInputMethod(const QString& v) {
    m_settings->setValue("Hardware/input_method", v);
    emit configChanged();
}

QString ConfigManager::ferrumPort() const { return m_settings->value("Hardware/ferrum_port", "").toString(); }
void ConfigManager::setFerrumPort(const QString& v) { m_settings->setValue("Hardware/ferrum_port", v); emit configChanged(); }
int ConfigManager::ferrumBaudrate() const { return m_settings->value("Hardware/ferrum_baudrate", 3000000).toInt(); }
void ConfigManager::setFerrumBaudrate(int v) { m_settings->setValue("Hardware/ferrum_baudrate", v); emit configChanged(); }
QString ConfigManager::catIp() const { return m_settings->value("Hardware/cat_ip", "192.168.7.1").toString(); }
void ConfigManager::setCatIp(const QString& v) { m_settings->setValue("Hardware/cat_ip", v); emit configChanged(); }
int ConfigManager::catPort() const { return m_settings->value("Hardware/cat_port", 8888).toInt(); }
void ConfigManager::setCatPort(int v) { m_settings->setValue("Hardware/cat_port", v); emit configChanged(); }
QString ConfigManager::catUuid() const { return m_settings->value("Hardware/cat_uuid", "").toString(); }
void ConfigManager::setCatUuid(const QString& v) { m_settings->setValue("Hardware/cat_uuid", v); emit configChanged(); }
int ConfigManager::catMonitorPort() const { return m_settings->value("Hardware/cat_monitor_port", 1234).toInt(); }
void ConfigManager::setCatMonitorPort(int v) { m_settings->setValue("Hardware/cat_monitor_port", v); emit configChanged(); }
QString ConfigManager::dhzboxIp() const { return m_settings->value("Hardware/dhzbox_ip", "192.168.2.88").toString(); }
void ConfigManager::setDhzboxIp(const QString& v) { m_settings->setValue("Hardware/dhzbox_ip", v); emit configChanged(); }
int ConfigManager::dhzboxPort() const { return m_settings->value("Hardware/dhzbox_port", 8888).toInt(); }
void ConfigManager::setDhzboxPort(int v) { m_settings->setValue("Hardware/dhzbox_port", v); emit configChanged(); }
int ConfigManager::dhzboxKey() const { return m_settings->value("Hardware/dhzbox_key", 88).toInt(); }
void ConfigManager::setDhzboxKey(int v) { m_settings->setValue("Hardware/dhzbox_key", v); emit configChanged(); }

int ConfigManager::makcuBaudrate() const {
    return m_settings->value("Hardware/makcu_baudrate", 115200).toInt();
}

void ConfigManager::setMakcuBaudrate(int v) {
    m_settings->setValue("Hardware/makcu_baudrate", v);
    emit configChanged();
}

QString ConfigManager::makcuPort() const {
    return m_settings->value("Hardware/makcu_port", "COM0").toString();
}

void ConfigManager::setMakcuPort(const QString& v) {
    m_settings->setValue("Hardware/makcu_port", v);
    emit configChanged();
}

int ConfigManager::makcuNewBaudrate() const {
    return m_settings->value("Hardware/makcu_new_baudrate", 6000000).toInt();
}

void ConfigManager::setMakcuNewBaudrate(int v) {
    m_settings->setValue("Hardware/makcu_new_baudrate", v);
    emit configChanged();
}

QString ConfigManager::makcuNewPort() const {
    return m_settings->value("Hardware/makcu_new_port", "COM0").toString();
}

void ConfigManager::setMakcuNewPort(const QString& v) {
    m_settings->setValue("Hardware/makcu_new_port", v);
    emit configChanged();
}

// 第二台(键盘)。端口默认空 = 未配置, 此时键盘动作回落到第一台。
int ConfigManager::makcuNewBaudrateKbd() const {
    return m_settings->value("Hardware/makcu_new_baudrate_kbd", 6000000).toInt();
}

void ConfigManager::setMakcuNewBaudrateKbd(int v) {
    m_settings->setValue("Hardware/makcu_new_baudrate_kbd", v);
    emit configChanged();
}

QString ConfigManager::makcuNewPortKbd() const {
    return m_settings->value("Hardware/makcu_new_port_kbd", "").toString();
}

void ConfigManager::setMakcuNewPortKbd(const QString& v) {
    m_settings->setValue("Hardware/makcu_new_port_kbd", v);
    emit configChanged();
}

QString ConfigManager::kmboxNetIp() const {
    return m_settings->value("Hardware/kmbox_net_ip", "192.168.2.88").toString();
}

void ConfigManager::setKmboxNetIp(const QString& v) {
    m_settings->setValue("Hardware/kmbox_net_ip", v);
    emit configChanged();
}

QString ConfigManager::kmboxNetPort() const {
    return m_settings->value("Hardware/kmbox_net_port", "6234").toString();
}

void ConfigManager::setKmboxNetPort(const QString& v) {
    m_settings->setValue("Hardware/kmbox_net_port", v);
    emit configChanged();
}

QString ConfigManager::kmboxNetUuid() const {
    return m_settings->value("Hardware/kmbox_net_uuid", "").toString();
}

void ConfigManager::setKmboxNetUuid(const QString& v) {
    m_settings->setValue("Hardware/kmbox_net_uuid", v);
    emit configChanged();
}

QString ConfigManager::backend() const {
    return QStringLiteral("TRT");
}

QString ConfigManager::aiModel() const {
    return m_settings->value("AI/ai_model", "sunxds_0.5.6.engine").toString();
}

void ConfigManager::setAiModel(const QString& v) {
    m_settings->setValue("AI/ai_model", v);
    emit configChanged();
}

QString ConfigManager::enginePrecision() const {
    const QString v = m_settings->value("AI/engine_precision", "fp16").toString();
    // 非法值一律回落到 fp16, 与 Config::loadConfig 里的行为保持一致。
    return (v == QLatin1String("int8")) ? v : QStringLiteral("fp16");
}

void ConfigManager::setEnginePrecision(const QString& v) {
    m_settings->setValue("AI/engine_precision",
                         (v == QLatin1String("int8")) ? v : QStringLiteral("fp16"));
    emit configChanged();
}

QString ConfigManager::int8CalibDir() const {
    return m_settings->value("AI/int8_calib_dir", "calib").toString();
}

void ConfigManager::setInt8CalibDir(const QString& v) {
    m_settings->setValue("AI/int8_calib_dir", v);
    emit configChanged();
}

float ConfigManager::confidenceThreshold() const {
    return m_settings->value("AI/confidence_threshold", 0.15).toFloat();
}

void ConfigManager::setConfidenceThreshold(float v) {
    m_settings->setValue("AI/confidence_threshold", static_cast<double>(v));
    emit configChanged();
}

float ConfigManager::nmsThreshold() const {
    return m_settings->value("AI/nms_threshold", 0.50).toFloat();
}

void ConfigManager::setNmsThreshold(float v) {
    m_settings->setValue("AI/nms_threshold", static_cast<double>(v));
    emit configChanged();
}

int ConfigManager::maxDetections() const {
    return kFixedMaxDetections;
}

void ConfigManager::setMaxDetections(int v) {
    Q_UNUSED(v);
    m_settings->setValue("AI/max_detections", kFixedMaxDetections);
}

bool ConfigManager::smallTargetEnabled() const {
    return m_settings->value("AI/small_target_enabled", false).toBool();
}

void ConfigManager::setSmallTargetEnabled(bool v) {
    m_settings->setValue("AI/small_target_enabled", v);
    emit configChanged();
}

float ConfigManager::smallTargetConfidence() const {
    return m_settings->value("AI/small_target_confidence", 0.08).toFloat();
}

void ConfigManager::setSmallTargetConfidence(float v) {
    m_settings->setValue("AI/small_target_confidence", static_cast<double>(v));
    emit configChanged();
}

float ConfigManager::smallTargetAreaFrac() const {
    return m_settings->value("AI/small_target_area_frac", 0.012).toFloat();
}

void ConfigManager::setSmallTargetAreaFrac(float v) {
    m_settings->setValue("AI/small_target_area_frac", static_cast<double>(v));
    emit configChanged();
}

bool ConfigManager::showFps() const {
    return m_settings->value("Debug/show_fps", false).toBool();
}

void ConfigManager::setShowFps(bool v) {
    m_settings->setValue("Debug/show_fps", v);
    emit configChanged();
}

bool ConfigManager::verbose() const {
    return m_settings->value("Debug/verbose", false).toBool();
}

void ConfigManager::setVerbose(bool v) {
    m_settings->setValue("Debug/verbose", v);
    emit configChanged();
}

int ConfigManager::screenshotDelay() const {
    return m_settings->value("Debug/screenshot_delay", 100).toInt();
}

void ConfigManager::setScreenshotDelay(int v) {
    m_settings->setValue("Debug/screenshot_delay", v);
    emit configChanged();
}

QString ConfigManager::screenshotButton() const {
    return m_settings->value("Debug/screenshot_button", QStringLiteral("None")).toString();
}

void ConfigManager::setScreenshotButton(const QString& v) {
    m_settings->setValue("Debug/screenshot_button", v.isEmpty() ? QStringLiteral("None") : v);
    emit configChanged();
}

bool ConfigManager::showWindow() const {
    return m_settings->value("Debug/show_window", false).toBool();
}

void ConfigManager::setShowWindow(bool v) {
    m_settings->setValue("Debug/show_window", v);
    emit configChanged();
}

bool ConfigManager::replayRecordEnabled() const {
    return m_settings->value("Debug/replay_record_enabled", false).toBool();
}

void ConfigManager::setReplayRecordEnabled(bool v) {
    m_settings->setValue("Debug/replay_record_enabled", v);
    emit configChanged();
}

int ConfigManager::replaySeconds() const {
    return m_settings->value("Debug/replay_seconds", 10).toInt();
}

void ConfigManager::setReplaySeconds(int v) {
    m_settings->setValue("Debug/replay_seconds", v);
    emit configChanged();
}

float ConfigManager::replayPlaybackSpeed() const {
    return m_settings->value("Debug/replay_playback_speed", 0.25).toFloat();
}

void ConfigManager::setReplayPlaybackSpeed(float v) {
    m_settings->setValue("Debug/replay_playback_speed", static_cast<double>(v));
    emit configChanged();
}

bool ConfigManager::macroEnabled() const {
    return m_settings->value("Macro/macro_enabled", false).toBool();
}

void ConfigManager::setMacroEnabled(bool v) {
    m_settings->setValue("Macro/macro_enabled", v);
    emit configChanged();
}

QString ConfigManager::macroScriptPath() const {
    return m_settings->value("Macro/macro_script_path", "").toString();
}

void ConfigManager::setMacroScriptPath(const QString& v) {
    m_settings->setValue("Macro/macro_script_path", v);
    emit configChanged();
}

bool ConfigManager::macroPrimaryButtonEvents() const {
    return m_settings->value("Macro/macro_primary_button_events", false).toBool();
}

void ConfigManager::setMacroPrimaryButtonEvents(bool v) {
    m_settings->setValue("Macro/macro_primary_button_events", v);
    emit configChanged();
}

int ConfigManager::crosshairRectW() const {
    return m_settings->value("Crosshair/crosshair_rect_w", 40).toInt();
}

void ConfigManager::setCrosshairRectW(int v) {
    m_settings->setValue("Crosshair/crosshair_rect_w", v);
    emit configChanged();
}

int ConfigManager::crosshairRectH() const {
    return m_settings->value("Crosshair/crosshair_rect_h", 40).toInt();
}

void ConfigManager::setCrosshairRectH(int v) {
    m_settings->setValue("Crosshair/crosshair_rect_h", v);
    emit configChanged();
}

int ConfigManager::crosshairOffsetY() const {
    return m_settings->value("Crosshair/crosshair_offset_y", 0).toInt();
}

void ConfigManager::setCrosshairOffsetY(int v) {
    m_settings->setValue("Crosshair/crosshair_offset_y", v);
    emit configChanged();
}

int ConfigManager::crosshairMinPixelCount() const {
    return m_settings->value("Crosshair/crosshair_min_pixel_count", 4).toInt();
}

void ConfigManager::setCrosshairMinPixelCount(int v) {
    m_settings->setValue("Crosshair/crosshair_min_pixel_count", v);
    emit configChanged();
}

int ConfigManager::crosshairAlgorithm() const {
    return std::clamp(m_settings->value("Crosshair/crosshair_algorithm", 0).toInt(), 0, 1);
}
void ConfigManager::setCrosshairAlgorithm(int v) {
    m_settings->setValue("Crosshair/crosshair_algorithm", std::clamp(v, 0, 1));
    emit configChanged();
}

int ConfigManager::crosshairCloseRadius() const {
    return m_settings->value("Crosshair/crosshair_close_radius", 1).toInt();
}

void ConfigManager::setCrosshairCloseRadius(int v) {
    m_settings->setValue("Crosshair/crosshair_close_radius", v);
    emit configChanged();
}

QList<ConfigManager::ColorProfile> ConfigManager::crosshairColors() const {
    QList<ColorProfile> result;
    int i = 0;
    while (m_settings->contains(
        QStringLiteral("crosshair_color.%1/name").arg(i))) {
        auto prefix = QStringLiteral("crosshair_color.%1/").arg(i);
        ColorProfile c;
        c.name    = m_settings->value(prefix + "name", "Color").toString();
        c.enabled = m_settings->value(prefix + "enabled", true).toBool();
        c.exactHsv = m_settings->value(prefix + "exact_hsv", false).toBool();
        c.hLow    = m_settings->value(prefix + "h_low", 0).toInt();
        c.hHigh   = m_settings->value(prefix + "h_high", 10).toInt();
        c.sMin    = m_settings->value(prefix + "s_min", 120).toInt();
        c.sMax    = m_settings->value(prefix + "s_max", 255).toInt();
        c.vMin    = m_settings->value(prefix + "v_min", 120).toInt();
        c.vMax    = m_settings->value(prefix + "v_max", 255).toInt();
        result.append(c);
        ++i;
    }
    if (result.isEmpty()) {
        ColorProfile low;
        low.name = QStringLiteral("Red-Low");  low.hLow = 0;   low.hHigh = 10;
        ColorProfile hi;
        hi.name  = QStringLiteral("Red-High"); hi.hLow  = 160; hi.hHigh  = 179;
        result.append(low);
        result.append(hi);
    }
    return result;
}

void ConfigManager::setCrosshairColors(const QList<ColorProfile>& colors) {
    int old = 0;
    while (m_settings->contains(
        QStringLiteral("crosshair_color.%1/name").arg(old))) {
        m_settings->remove(QStringLiteral("crosshair_color.%1").arg(old));
        ++old;
    }
    for (int i = 0; i < colors.size(); ++i) {
        auto prefix = QStringLiteral("crosshair_color.%1/").arg(i);
        const auto& c = colors[i];
        m_settings->setValue(prefix + "name",    c.name);
        m_settings->setValue(prefix + "enabled", c.enabled);
        m_settings->setValue(prefix + "exact_hsv", c.exactHsv);
        m_settings->setValue(prefix + "h_low",   c.hLow);
        m_settings->setValue(prefix + "h_high",  c.hHigh);
        m_settings->setValue(prefix + "s_min",   c.sMin);
        m_settings->setValue(prefix + "s_max",   c.sMax);
        m_settings->setValue(prefix + "v_min",   c.vMin);
        m_settings->setValue(prefix + "v_max",   c.vMax);
    }
    emit configChanged();
}

#define LASER_INT_ACCESSOR(Name, Key, Default) \
int ConfigManager::laser##Name() const { return m_settings->value("Laser/" Key, Default).toInt(); } \
void ConfigManager::setLaser##Name(int v) { m_settings->setValue("Laser/" Key, v); emit configChanged(); }
LASER_INT_ACCESSOR(RectW, "laser_rect_w", 160)
LASER_INT_ACCESSOR(RectH, "laser_rect_h", 240)
LASER_INT_ACCESSOR(CenterX, "laser_center_x", 160)
LASER_INT_ACCESSOR(CenterY, "laser_center_y", 200)
LASER_INT_ACCESSOR(TargetCenterX, "laser_target_center_x", 160)
LASER_INT_ACCESSOR(TargetCenterY, "laser_target_center_y", 160)
LASER_INT_ACCESSOR(TargetRectW, "laser_target_rect_w", 60)
LASER_INT_ACCESSOR(TargetRectH, "laser_target_rect_h", 60)
LASER_INT_ACCESSOR(MinPixelCount, "laser_min_pixel_count", 10)
LASER_INT_ACCESSOR(CloseRadius, "laser_close_radius", 1)
#undef LASER_INT_ACCESSOR
float ConfigManager::laserMinElongation() const { return m_settings->value("Laser/laser_min_elongation", 3.0).toFloat(); }
void ConfigManager::setLaserMinElongation(float v) { m_settings->setValue("Laser/laser_min_elongation", v); emit configChanged(); }
float ConfigManager::laserSmooth() const { return m_settings->value("Laser/laser_smooth", 0.5).toFloat(); }
void ConfigManager::setLaserSmooth(float v) { m_settings->setValue("Laser/laser_smooth", v); emit configChanged(); }

QList<ConfigManager::ColorProfile> ConfigManager::laserColors() const {
    QList<ColorProfile> result;
    for (int i = 0; m_settings->contains(QStringLiteral("laser_color.%1/name").arg(i)); ++i) {
        const auto prefix = QStringLiteral("laser_color.%1/").arg(i);
        ColorProfile c;
        c.name = m_settings->value(prefix + "name", "Laser").toString();
        c.enabled = m_settings->value(prefix + "enabled", true).toBool();
        c.exactHsv = m_settings->value(prefix + "exact_hsv", false).toBool();
        c.hLow = m_settings->value(prefix + "h_low", 0).toInt();
        c.hHigh = m_settings->value(prefix + "h_high", 10).toInt();
        c.sMin = m_settings->value(prefix + "s_min", 120).toInt();
        c.sMax = m_settings->value(prefix + "s_max", 255).toInt();
        c.vMin = m_settings->value(prefix + "v_min", 120).toInt();
        c.vMax = m_settings->value(prefix + "v_max", 255).toInt();
        result.append(c);
    }
    if (result.isEmpty()) {
        ColorProfile low, high;
        low.name = QStringLiteral("Laser-Red-Low"); low.hLow = 0; low.hHigh = 10;
        high.name = QStringLiteral("Laser-Red-High"); high.hLow = 160; high.hHigh = 179;
        result.append(low); result.append(high);
    }
    return result;
}
void ConfigManager::setLaserColors(const QList<ColorProfile>& colors) {
    for (int i = 0; m_settings->contains(QStringLiteral("laser_color.%1/name").arg(i)); ++i)
        m_settings->remove(QStringLiteral("laser_color.%1").arg(i));
    for (int i = 0; i < colors.size(); ++i) {
        const auto prefix = QStringLiteral("laser_color.%1/").arg(i);
        const auto& c = colors[i];
        m_settings->setValue(prefix + "name", c.name);
        m_settings->setValue(prefix + "enabled", c.enabled);
        m_settings->setValue(prefix + "exact_hsv", c.exactHsv);
        m_settings->setValue(prefix + "h_low", c.hLow);
        m_settings->setValue(prefix + "h_high", c.hHigh);
        m_settings->setValue(prefix + "s_min", c.sMin);
        m_settings->setValue(prefix + "s_max", c.sMax);
        m_settings->setValue(prefix + "v_min", c.vMin);
        m_settings->setValue(prefix + "v_max", c.vMax);
    }
    emit configChanged();
}

double ConfigManager::targetHysteresisRatio() const {
    return m_settings ? m_settings->value("target_stabilizer/target_hysteresis_ratio", 1.3).toDouble() : 1.3;
}
void ConfigManager::setTargetHysteresisRatio(double v) {
    if (m_settings) m_settings->setValue("target_stabilizer/target_hysteresis_ratio", v);
    emit configChanged();
}
double ConfigManager::targetMaxDistancePx() const {
    return m_settings ? m_settings->value("target_stabilizer/target_max_distance_px", 0.0).toDouble() : 0.0;
}
void ConfigManager::setTargetMaxDistancePx(double v) {
    if (m_settings) m_settings->setValue("target_stabilizer/target_max_distance_px", v);
    emit configChanged();
}
QString ConfigManager::activeHotkeyGroup() const {
    return m_settings ? m_settings->value("active_hotkey_group",
        QStringLiteral("\xe9\xbb\x98\xe8\xae\xa4")).toString() : QStringLiteral("\xe9\xbb\x98\xe8\xae\xa4");
}
void ConfigManager::setActiveHotkeyGroup(const QString& v) {
    if (m_settings) m_settings->setValue("active_hotkey_group", v);
    emit configChanged();
}

int ConfigManager::hotkeyCount() const {
    if (!m_settings)
        return 0;

    int count = 0;
    while (m_settings->contains(
        QStringLiteral("Hotkey_%1/name").arg(count))) {
        ++count;
    }
    return count;
}

void ConfigManager::writeHotkeyToSettings(int index, const HotkeyData& data) {
    auto prefix = QStringLiteral("Hotkey_%1/").arg(index);
    m_settings->setValue(prefix + "name", data.name);
    m_settings->setValue(prefix + "group", data.group);
    m_settings->setValue(prefix + "keys", data.keys.join(","));
    m_settings->setValue(prefix + "fovX", data.fovX);
    m_settings->setValue(prefix + "fovY", data.fovY);
    m_settings->setValue(prefix + "aim_classes",         data.aimClasses);
    m_settings->setValue(prefix + "crosshair_detect_enabled", data.crosshairDetectEnabled);
    m_settings->setValue(prefix + "laser_detect_enabled", data.laserDetectEnabled && !data.crosshairDetectEnabled);
    m_settings->setValue(prefix + "dynamic_fov_enabled", data.dynamicFovEnabled);
    m_settings->remove(prefix + "dynamic_fov_strength");
    m_settings->setValue(prefix + "dynamic_fov_size", data.dynamicFovSize);
    m_settings->setValue(prefix + "dynamic_fov_expand_ms", data.dynamicFovExpandMs);
    m_settings->setValue(prefix + "dynamic_fov_shrink_ms", data.dynamicFovShrinkMs);
    m_settings->setValue(prefix + "mask_x", data.maskX);
    m_settings->setValue(prefix + "block_hotkey", data.blockHotkey);
    m_settings->setValue(prefix + "mask_y", data.maskY);
    m_settings->setValue(prefix + "unlock_x", data.unlockX);
    m_settings->setValue(prefix + "unlock_y", data.unlockY);
    m_settings->setValue(prefix + "unlock_y_delay_ms", data.unlockYDelayMs);
    m_settings->setValue(prefix + "aim_delay_ms", data.aimDelayMs);
    m_settings->setValue(prefix + "ctl_enabled", data.ctlEnabled);
    m_settings->setValue(prefix + "ctl_y_offset", static_cast<double>(data.ctlYOffset));
    m_settings->setValue(prefix + "ctl_y_offset_max", static_cast<double>(data.ctlYOffsetMax));
    m_settings->setValue(prefix + "ctl_x_offset", static_cast<double>(data.ctlXOffset));
    m_settings->setValue(prefix + "ctl_x_offset_max", static_cast<double>(data.ctlXOffsetMax));
    m_settings->setValue(prefix + "ctl_random_seed", data.ctlRandomSeed);
}

ConfigManager::HotkeyData ConfigManager::readHotkeyFromSettings(int index) const {
    HotkeyData data;
    auto prefix = QStringLiteral("Hotkey_%1/").arg(index);

    data.name = m_settings->value(prefix + "name", "Aim").toString();
    data.group = m_settings->value(prefix + "group", QString::fromUtf8(u8"\xe9\xbb\x98\xe8\xae\xa4")).toString();
    data.keys = m_settings->value(prefix + "keys", "RightMouseButton").toString().split(",", Qt::SkipEmptyParts);
    data.fovX = m_settings->value(prefix + "fovX", 106).toInt();
    data.fovY = m_settings->value(prefix + "fovY", 74).toInt();
    data.aimClasses      = m_settings->value(prefix + "aim_classes", QString()).toString();
    data.crosshairDetectEnabled = m_settings->value(prefix + "crosshair_detect_enabled", false).toBool();
    data.laserDetectEnabled = m_settings->value(prefix + "laser_detect_enabled", false).toBool()
                              && !data.crosshairDetectEnabled;
    data.dynamicFovEnabled = m_settings->value(prefix + "dynamic_fov_enabled", false).toBool();
    data.dynamicFovSize = std::clamp(m_settings->value(prefix + "dynamic_fov_size", 40).toInt(), 1, 4096);
    data.dynamicFovExpandMs = std::clamp(m_settings->value(prefix + "dynamic_fov_expand_ms", 120).toInt(), 0, 2000);
    data.dynamicFovShrinkMs = std::clamp(m_settings->value(prefix + "dynamic_fov_shrink_ms", 200).toInt(), 0, 2000);
    data.maskX = m_settings->value(prefix + "mask_x", false).toBool();
    data.blockHotkey = m_settings->value(prefix + "block_hotkey", false).toBool();
    data.maskY = m_settings->value(prefix + "mask_y", false).toBool();
    data.unlockX = m_settings->value(prefix + "unlock_x", false).toBool();
    data.unlockY = m_settings->value(prefix + "unlock_y", false).toBool();
    data.unlockYDelayMs = std::clamp(m_settings->value(prefix + "unlock_y_delay_ms", 0).toInt(), 0, 5000);
    data.aimDelayMs = std::clamp(m_settings->value(prefix + "aim_delay_ms", 0).toInt(), 0, 2000);
    data.ctlEnabled = m_settings->value(prefix + "ctl_enabled", false).toBool();
    data.ctlYOffset = m_settings->value(prefix + "ctl_y_offset", 0.5).toDouble();
    data.ctlYOffsetMax = m_settings->value(prefix + "ctl_y_offset_max", 0.5).toDouble();
    data.ctlXOffset = m_settings->value(prefix + "ctl_x_offset", 0.5).toDouble();
    data.ctlXOffsetMax = m_settings->value(prefix + "ctl_x_offset_max", 0.5).toDouble();
    data.ctlRandomSeed = m_settings->value(prefix + "ctl_random_seed", 0).toInt();

    return data;
}

ConfigManager::HotkeyData ConfigManager::hotkey(int index) const {
    if (index < 0 || index >= hotkeyCount())
        return {};
    return readHotkeyFromSettings(index);
}

void ConfigManager::setHotkey(int index, const HotkeyData& data) {
    if (index < 0 || index >= hotkeyCount())
        return;
    writeHotkeyToSettings(index, data);
    emit configChanged();
}

void ConfigManager::addHotkey(const HotkeyData& data) {
    int index = hotkeyCount();
    writeHotkeyToSettings(index, data);
    emit configChanged();
}

void ConfigManager::removeHotkey(int index) {
    int count = hotkeyCount();
    if (index < 0 || index >= count || count <= 1)
        return;

    m_settings->remove(QStringLiteral("Hotkey_%1").arg(index));
    reindexHotkeys();
    emit configChanged();
}

void ConfigManager::reindexHotkeys() {
    QVector<HotkeyData> all;
    int i = 0;
    while (m_settings->contains(QStringLiteral("Hotkey_%1/name").arg(i))) {
        all.append(readHotkeyFromSettings(i));
        ++i;
    }

    for (int k = 0; k < i; ++k)
        m_settings->remove(QStringLiteral("Hotkey_%1").arg(k));

    for (int k = 0; k < all.size(); ++k)
        writeHotkeyToSettings(k, all[k]);
}
