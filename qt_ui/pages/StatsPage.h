#pragma once

#include <QPainter>
#include <QVector>
#include <QWidget>

class QLabel;

class FpsGraphWidget : public QWidget {
    Q_OBJECT

public:
    explicit FpsGraphWidget(QWidget* parent = nullptr);
    void addDataPoint(double value);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<double> m_data;
    static constexpr int kMaxPoints = 120;
};

class StatsPage : public QWidget {
    Q_OBJECT

public:
    explicit StatsPage(QWidget* parent = nullptr);

    void setFps(double fps);
    void setSourceFps(double fps);
    void setCaptureLatency(double ms);
    void setInferenceLatency(double ms);
    void setTotalLatency(double ms);
    void setGpuMemory(const QString& text);
    void setCpuCores(const QString& text);
    void setCaptureChainDiagnostics(int deviceAgeUs, double capToDetectMs, double inferMs,
                                    double publishToAimMs, double endToEndMs);
    void setPipelineDiagnostics(double totalGpuMs, double preprocessMs, double engineMs, double copyMs,
                                double postprocessMs, double aimTickMs, double aimTickPeakMs,
                                bool graphMode);

private:
    QLabel* m_fpsValue{};
    QLabel* m_sourceFpsValue{};
    QLabel* m_captureLatency{};
    QLabel* m_inferenceLatency{};
    QLabel* m_totalLatency{};
    QLabel* m_gpuMemory{};
    QLabel* m_cpuCores{};
    QLabel* m_diagDeviceAge{};
    QLabel* m_diagCapToDetect{};
    QLabel* m_diagInfer{};
    QLabel* m_diagPublishToAim{};
    QLabel* m_diagEndToEnd{};
    QLabel* m_controlDt{};
    QLabel* m_controlCaptureAge{};
    QLabel* m_derivativeRaw{};
    QLabel* m_gpuPipeline{};
    QLabel* m_gpuPreprocess{};
    QLabel* m_gpuEngine{};
    QLabel* m_gpuCopy{};
    QLabel* m_cpuPostprocess{};
    QLabel* m_aimTick{};
    QLabel* m_aimTickPeak{};
    QLabel* m_mouseQueueLatency{};
    QLabel* m_mouseQueueBacklog{};
    QLabel* m_mouseSendFailures{};
    FpsGraphWidget* m_graph{};
};
