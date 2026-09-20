#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class MetricCard;
class TelemetryChart;

class OverviewPage : public QWidget {
    Q_OBJECT

public:
    explicit OverviewPage(QWidget* parent = nullptr);

    void setFps(double fps);
    void setSourceFps(double fps);
    void setInferenceLatency(double ms);
    void setTotalLatency(double ms);
    void setDetectionCount(int boxes, int locked);
    void setCaptureChainDiagnostics(int deviceAgeUs, double capToDetectMs, double inferMs,
                                    double publishToAimMs, double endToEndMs);
    void setSessionState(bool running, const QString& model,
                         const QString& backend, const QString& uptime);

signals:
    void startStopRequested();
    void previewRequested();

private:
    QLabel* m_heroChip{};
    QLabel* m_heroTitle{};
    QLabel* m_heroSub{};
    QPushButton* m_startBtn{};

    MetricCard* m_mFps{};
    MetricCard* m_mInfer{};
    MetricCard* m_mTotal{};
    MetricCard* m_mTargets{};

    TelemetryChart* m_chart{};
    QLabel* m_chartValue{};

    QLabel* m_diagDeviceAge{};
    QLabel* m_diagCapToDetect{};
    QLabel* m_diagInfer{};
    QLabel* m_diagPublishToAim{};
    QLabel* m_diagEndToEnd{};

    bool m_running = false;
};
