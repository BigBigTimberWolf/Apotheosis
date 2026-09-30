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
    QString captureStreamUrl() const;
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
    int crosshairCloseRadius() const;
    void setCrosshairCloseRadius(int v);

    struct ColorProfile {
        QString name;
        bool enabled = true;
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
    double targetMatchCenterRatio() const;
    void setTargetMatchCenterRatio(double v);
    double targetAreaRatioTol() const;
    void setTargetAreaRatioTol(double v);
    double targetKSnapMult() const;
    void setTargetKSnapMult(double v);
    double targetMinAspect() const;
    void setTargetMinAspect(double v);
    double targetMaxAspect() const;
    void setTargetMaxAspect(double v);

    QString activeHotkeyGroup() const;
    void setActiveHotkeyGroup(const QString& v);

    int hotkeyCount() const;

    struct HotkeyData {
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

        bool   ctlEnabled = false;
        double ctlKpX = 35.0, ctlKpY = 35.0;
        double ctlKiX = 0.0,  ctlKiY = 0.0;
        double ctlKdX = 0.0,  ctlKdY = 0.0;
        double ctlTauUnwindSec = 0.030;
        double ctlTauDerivSec = 0.020;
        double ctlIMax = 0.0;
        int    ctlMaxOutputCounts = 200;
        double ctlPFullScalePx = 0.0;

        // 在途补偿（预测提前量）。leadMs == 0 ⇒ 预测整体关闭，
        // 另两个参数不生效（0 = 不限制）。
        double ctlPredictLeadMs = 0.0;
        double ctlPredictMaxVelocityPxPerSec = 0.0;
        double ctlPredictMaxLeadRatio = 0.0;

        // 灵敏度折算系数 k (像素/计数)：修正预测吃到的目标速度里的自身运动污染。
        double ctlKPxPerCount = 0.0;

        // 在途自身位移补偿 (Smith)：把已下发但画面未显现的自身位移从输出里扣掉。
        double ctlInflightBeta = 1.6;
        double ctlInflightDeadTimeMs = 46.0;

        double ctlYOffset = 0.5;
        double ctlYOffsetMax = 0.5;
        double ctlXOffset = 0.5;
        double ctlXOffsetMax = 0.5;
        double ctlHysteresisRatio = 1.3;
        double ctlMaxDistancePx = 0.0;
        int    ctlRandomSeed = 0;
        double ctlMatchCenterRatio = 0.5;
        double ctlAreaRatioTol = 2.0;
        double ctlKSnapMult = 1.15;
        double ctlMinAspect = 0.2;
        double ctlMaxAspect = 5.0;
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
