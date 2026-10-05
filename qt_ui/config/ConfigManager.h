#pragma once

#include <QList>
#include <QObject>
#include <QSettings>
#include <QStringList>

class ConfigManager : public QObject {
    Q_OBJECT

public:
    static ConfigManager& instance();

    bool load(const QString& path = "config.ini");
    bool save();
    QString configPath() const;

    void notifyRuntimeReloaded();

    QString captureDevice() const;
    void setCaptureDevice(const QString& v);
    QString captureSource() const;
    void setCaptureSource(const QString& v);
    QString captureDeviceApi() const;
    void setCaptureDeviceApi(const QString& v);
    QString captureStreamUrl() const;
    QString captureNdiSource() const;
    void setCaptureNdiSource(const QString& v);
    QString captureDxgiOutput() const;
    void setCaptureDxgiOutput(const QString& v);
    void setCaptureStreamUrl(const QString& v);
    QString captureFormat() const;
    void setCaptureFormat(const QString& v);
    int captureWidth() const;
    void setCaptureWidth(int v);
    int captureHeight() const;
    void setCaptureHeight(int v);
    int captureFps() const;
    void setCaptureFps(int v);
    bool captureGpuDecode() const;
    void setCaptureGpuDecode(bool v);
    int detectionResolution() const;
    void setDetectionResolution(int v);
    // 引擎精度: "fp16" | "int8"
    QString enginePrecision() const;
    void setEnginePrecision(const QString& v);
    QString int8CalibDir() const;
    void setInt8CalibDir(const QString& v);
    bool circleMask() const;
    void setCircleMask(bool v);
    QString inputMethod() const;
    void setInputMethod(const QString& v);
    int makcuBaudrate() const;
    void setMakcuBaudrate(int v);
    QString makcuPort() const;
    void setMakcuPort(const QString& v);
    int makcuNewBaudrate() const;
    void setMakcuNewBaudrate(int v);
    QString makcuNewPort() const;
    void setMakcuNewPort(const QString& v);
    // 第二台 MAKCUNEW(键盘那台): 自动急停的屏蔽命令从它发出
    int makcuNewBaudrateKbd() const;
    void setMakcuNewBaudrateKbd(int v);
    QString makcuNewPortKbd() const;
    void setMakcuNewPortKbd(const QString& v);
    QString kmboxNetIp() const;
    void setKmboxNetIp(const QString& v);
    QString kmboxNetPort() const;
    QString ferrumPort() const;
    void setFerrumPort(const QString& v);
    int ferrumBaudrate() const;
    void setFerrumBaudrate(int v);
    QString cpboxPort() const;
    void setCpboxPort(const QString& v);
    QString catIp() const;
    void setCatIp(const QString& value);
    int catPort() const;
    void setCatPort(int value);
    QString catUuid() const;
    void setCatUuid(const QString& value);
    int catMonitorPort() const;
    void setCatMonitorPort(int value);
    QString dhzboxIp() const;
    void setDhzboxIp(const QString& v);
    int dhzboxPort() const;
    void setDhzboxPort(int v);
    int dhzboxKey() const;
    void setDhzboxKey(int v);
    void setKmboxNetPort(const QString& v);
    QString kmboxNetUuid() const;
    void setKmboxNetUuid(const QString& v);
    QString backend() const;
    QString aiModel() const;
    void setAiModel(const QString& v);
    float confidenceThreshold() const;
    void setConfidenceThreshold(float v);
    float nmsThreshold() const;
    void setNmsThreshold(float v);
    int maxDetections() const;
    void setMaxDetections(int v);
    bool smallTargetEnabled() const;
    void setSmallTargetEnabled(bool v);
    float smallTargetConfidence() const;
    void setSmallTargetConfidence(float v);
    float smallTargetAreaFrac() const;
    void setSmallTargetAreaFrac(float v);

    bool macroEnabled() const;
    void setMacroEnabled(bool v);
    QString macroScriptPath() const;
    void setMacroScriptPath(const QString& v);
    bool macroPrimaryButtonEvents() const;
    void setMacroPrimaryButtonEvents(bool v);

    int crosshairRectW() const;
    void setCrosshairRectW(int v);
    int crosshairRectH() const;
    void setCrosshairRectH(int v);
    int crosshairOffsetY() const;
    void setCrosshairOffsetY(int v);
    int crosshairMinPixelCount() const;
    void setCrosshairMinPixelCount(int v);
    int crosshairAlgorithm() const;
    void setCrosshairAlgorithm(int v);
    int crosshairCloseRadius() const;
    void setCrosshairCloseRadius(int v);

    struct ColorProfile {
        QString name;
        bool enabled = true;
        bool exactHsv = false;
        int hLow = 0, hHigh = 10;
        int sMin = 120, sMax = 255;
        int vMin = 120, vMax = 255;
    };
    QList<ColorProfile> crosshairColors() const;
    void setCrosshairColors(const QList<ColorProfile>& colors);
    int laserRectW() const; void setLaserRectW(int);
    int laserRectH() const; void setLaserRectH(int);
    int laserCenterX() const; void setLaserCenterX(int);
    int laserCenterY() const; void setLaserCenterY(int);
    int laserTargetCenterX() const; void setLaserTargetCenterX(int);
    int laserTargetCenterY() const; void setLaserTargetCenterY(int);
    int laserTargetRectW() const; void setLaserTargetRectW(int);
    int laserTargetRectH() const; void setLaserTargetRectH(int);
    int laserMinPixelCount() const; void setLaserMinPixelCount(int);
    int laserCloseRadius() const; void setLaserCloseRadius(int);
    float laserMinElongation() const; void setLaserMinElongation(float);
    float laserSmooth() const; void setLaserSmooth(float);
    QList<ColorProfile> laserColors() const;
    void setLaserColors(const QList<ColorProfile>& colors);

    bool showFps() const;
    void setShowFps(bool v);
    bool verbose() const;
    void setVerbose(bool v);
    int screenshotDelay() const;
    void setScreenshotDelay(int v);
    QString screenshotButton() const;
    void setScreenshotButton(const QString& v);
    bool showWindow() const;
    void setShowWindow(bool v);
    bool replayRecordEnabled() const;
    void setReplayRecordEnabled(bool v);
    int replaySeconds() const;
    void setReplaySeconds(int v);
    float replayPlaybackSpeed() const;
    void setReplayPlaybackSpeed(float v);

    // 全局选靶与稳定器
    double targetHysteresisRatio() const;
    void setTargetHysteresisRatio(double v);
    double targetMaxDistancePx() const;
    void setTargetMaxDistancePx(double v);

    QString activeHotkeyGroup() const;
    void setActiveHotkeyGroup(const QString& v);

    int hotkeyCount() const;

    struct HotkeyData {
        bool enabled = true;
        QString name;
        QString group;
        QStringList keys;
        int fovX = 106, fovY = 74;
        QString aimClasses;
        bool crosshairDetectEnabled = false;
        bool laserDetectEnabled = false;
        bool dynamicFovEnabled = false;
        int dynamicFovSize = 40;
        int dynamicFovShrinkMs = 200;
        int dynamicFovExpandMs = 120;
        bool blockHotkey = false;
        bool maskX = false, maskY = false;
        bool unlockX = false, unlockY = false;
        int unlockYDelayMs = 0;
        int aimDelayMs = 0;
        int maskDelayMs = 0;

        bool   ctlEnabled = false;
        double ctlYOffset = 0.5;
        double ctlYOffsetMax = 0.5;
        double ctlXOffset = 0.5;
        double ctlXOffsetMax = 0.5;
        int    ctlRandomSeed = 0;
    };

    HotkeyData hotkey(int index) const;
    void setHotkey(int index, const HotkeyData& data);
    void addHotkey(const HotkeyData& data);
    void removeHotkey(int index);

signals:
    void configChanged();
    void configLoaded();

private:
    ConfigManager();
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    void writeHotkeyToSettings(int index, const HotkeyData& data);
    HotkeyData readHotkeyFromSettings(int index) const;
    void reindexHotkeys();

    QSettings* m_settings = nullptr;
    QString m_path;
};
