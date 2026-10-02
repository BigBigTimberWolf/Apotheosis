#include "pages/StatsPage.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "runtime/aim_telemetry.h"

#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

FpsGraphWidget::FpsGraphWidget(QWidget* parent)
    : QWidget(parent) {
    setMinimumHeight(200);
    setFixedHeight(200);
}

void FpsGraphWidget::addDataPoint(double value) {
    m_data.append(value);
    if (m_data.size() > kMaxPoints) {
        m_data.removeFirst();
    }
    update();
}

void FpsGraphWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QColor kAccent(QStringLiteral("#D5B56B"));
    const QColor kGrid(213, 181, 107, 28);
    const QColor kAxisText(QStringLiteral("#A49E90"));

    const int w = width();
    const int h = height();
    const int margin = 40;
    const int graphW = w - margin * 2;
    const int graphH = h - margin * 2;

    double maxVal = 1.0;
    for (auto v : m_data) {
        if (v > maxVal) maxVal = v;
    }
    maxVal = std::ceil(maxVal / 10.0) * 10.0;
    if (maxVal < 10.0) maxVal = 10.0;

    constexpr int kGridLines = 5;
    for (int i = 0; i <= kGridLines; ++i) {
        int y = margin + graphH - (i * graphH / kGridLines);

        p.setPen(kGrid);
        p.drawLine(margin, y, margin + graphW, y);

        p.setPen(kAxisText);
        double label = maxVal * i / kGridLines;
        p.drawText(0, y - 8, margin - 4, 16, Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(label, 'f', 0));
    }

    if (m_data.size() < 2) return;

    int count = m_data.size();
    double stepX = static_cast<double>(graphW) / (kMaxPoints - 1);
    int startX = margin + static_cast<int>((kMaxPoints - count) * stepX);

    auto pointAt = [&](int i) {
        double x = startX + i * stepX;
        double y = margin + graphH - (m_data[i] / maxVal) * graphH;
        return QPointF(x, y);
    };

    QPainterPath area;
    area.moveTo(pointAt(0).x(), margin + graphH);
    for (int i = 0; i < count; ++i) {
        area.lineTo(pointAt(i));
    }
    area.lineTo(pointAt(count - 1).x(), margin + graphH);
    area.closeSubpath();

    QColor fill = kAccent;
    fill.setAlpha(28);
    p.fillPath(area, fill);

    QPen linePen(kAccent, 2);
    linePen.setJoinStyle(Qt::RoundJoin);
    p.setPen(linePen);
    QPointF prev;
    for (int i = 0; i < count; ++i) {
        QPointF pt = pointAt(i);
        if (i > 0) {
            p.drawLine(prev, pt);
        }
        prev = pt;
    }

    if (!m_data.isEmpty()) {
        p.setPen(kAccent);
        QFont valueFont = p.font();
        valueFont.setBold(true);
        p.setFont(valueFont);
        p.drawText(margin + graphW - 80, margin - 22, 80, 18,
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(m_data.last(), 'f', 1));
    }
}

StatsPage::StatsPage(QWidget* parent)
    : QWidget(parent) {
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outerLayout->addWidget(scroll);

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(14);
    scroll->setWidget(content);

    auto* perfCard = new CardWidget(QString::fromUtf8(u8"实时性能"), QStringLiteral("gauge"));

    auto* metricGrid = new QGridLayout;
    metricGrid->setHorizontalSpacing(12);
    metricGrid->setVerticalSpacing(16);
    metricGrid->setContentsMargins(0, 4, 0, 4);

    auto makeMetricCell = [](const QString& caption, QLabel*& valueOut) {
        auto* cell = new QWidget;
        auto* v = new QVBoxLayout(cell);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);

        auto* captionLabel = new QLabel(caption);
        captionLabel->setProperty("class", "secondary");

        auto* value = new QLabel(QStringLiteral("--"));
        value->setObjectName(QStringLiteral("metricValue"));

        v->addWidget(captionLabel);
        v->addWidget(value);

        valueOut = value;
        return cell;
    };

    metricGrid->addWidget(makeMetricCell(QString::fromUtf8(u8"采集 FPS"), m_fpsValue),         0, 0);
    metricGrid->addWidget(makeMetricCell(QString::fromUtf8(u8"产帧 FPS"), m_sourceFpsValue),   0, 1);
    metricGrid->addWidget(makeMetricCell(QString::fromUtf8(u8"采集延迟"), m_captureLatency),   1, 0);
    metricGrid->addWidget(makeMetricCell(QString::fromUtf8(u8"GPU 链路延迟"), m_inferenceLatency), 1, 1);
    metricGrid->addWidget(makeMetricCell(QString::fromUtf8(u8"总延迟"),   m_totalLatency),     2, 0);
    metricGrid->setColumnStretch(0, 1);
    metricGrid->setColumnStretch(1, 1);

    perfCard->contentLayout()->addLayout(metricGrid);
    layout->addWidget(perfCard);

    auto* graphCard = new CardWidget(QString::fromUtf8(u8"性能图表"), QStringLiteral("chart-line"));
    m_graph = new FpsGraphWidget;
    graphCard->contentLayout()->addWidget(m_graph);
    layout->addWidget(graphCard);

    auto* rxCard = new CardWidget(QString::fromUtf8(u8"采集诊断 (采集卡)"), QStringLiteral("activity"));
    rxCard->setCollapsible(true);

    auto addRxRow = [rxCard](const QString& caption, QLabel*& outLabel) {
        outLabel = new QLabel(QStringLiteral("--"));
        outLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rxCard->contentLayout()->addWidget(FormKit::fieldRow(caption, outLabel));
    };
    addRxRow(QString::fromUtf8(u8"设备帧龄 (需驱动时间戳)"), m_diagDeviceAge);
    addRxRow(QString::fromUtf8(u8"接收→取帧"),          m_diagCapToDetect);
    addRxRow(QString::fromUtf8(u8"推理 (含前后处理)"),  m_diagInfer);
    addRxRow(QString::fromUtf8(u8"发布→消费"),          m_diagPublishToAim);
    addRxRow(QString::fromUtf8(u8"全链路 (下界)"),      m_diagEndToEnd);

    layout->addWidget(rxCard);

    auto* controlCard = new CardWidget(QString::fromUtf8(u8"控制时序（当前帧）"), QStringLiteral("activity"));
    controlCard->setCollapsible(true);
    auto addControlRow = [controlCard](const QString& caption, QLabel*& outLabel) {
        outLabel = new QLabel(QStringLiteral("--"));
        outLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        controlCard->contentLayout()->addWidget(FormKit::fieldRow(caption, outLabel));
    };
    addControlRow(QString::fromUtf8(u8"控制循环间隔"), m_controlDt);
    addControlRow(QString::fromUtf8(u8"采集→控制帧龄"), m_controlCaptureAge);
    addControlRow(QString::fromUtf8(u8"D 项原值 X / Y"), m_derivativeRaw);
    layout->addWidget(controlCard);

    auto* pipelineCard = new CardWidget(QString::fromUtf8(u8"推理链路分段"), QStringLiteral("activity"));
    pipelineCard->setCollapsible(true);
    auto* pipelineNote = new QLabel(QString::fromUtf8(u8"Graph 开启时显示总链路；分段项仅在普通模式可用。"));
    pipelineNote->setProperty("class", "secondary");
    pipelineCard->contentLayout()->addWidget(pipelineNote);
    auto addPipelineRow = [pipelineCard](const QString& caption, QLabel*& outLabel) {
        outLabel = new QLabel(QStringLiteral("--"));
        outLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        pipelineCard->contentLayout()->addWidget(FormKit::fieldRow(caption, outLabel));
    };
    addPipelineRow(QString::fromUtf8(u8"GPU 总链路"), m_gpuPipeline);
    addPipelineRow(QString::fromUtf8(u8"GPU 前处理"), m_gpuPreprocess);
    addPipelineRow(QString::fromUtf8(u8"模型执行"), m_gpuEngine);
    addPipelineRow(QString::fromUtf8(u8"输出回传"), m_gpuCopy);
    addPipelineRow(QString::fromUtf8(u8"CPU 后处理"), m_cpuPostprocess);
    addPipelineRow(QString::fromUtf8(u8"控制 Tick 平均"), m_aimTick);
    addPipelineRow(QString::fromUtf8(u8"控制 Tick 峰值"), m_aimTickPeak);
    layout->addWidget(pipelineCard);

    auto* mouseCard = new CardWidget(QString::fromUtf8(u8"鼠标发送队列"), QStringLiteral("activity"));
    mouseCard->setCollapsible(true);
    auto addMouseRow = [mouseCard](const QString& caption, QLabel*& output) {
        output = new QLabel(QStringLiteral("--"));
        output->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        mouseCard->contentLayout()->addWidget(FormKit::fieldRow(caption, output));
    };
    addMouseRow(QString::fromUtf8(u8"发送延迟"), m_mouseQueueLatency);
    addMouseRow(QString::fromUtf8(u8"队列积压"), m_mouseQueueBacklog);
    addMouseRow(QString::fromUtf8(u8"发送失败"), m_mouseSendFailures);
    layout->addWidget(mouseCard);

    auto* mouseTimer = new QTimer(this);
    mouseTimer->setInterval(250);
    connect(mouseTimer, &QTimer::timeout, this, [this] {
        const auto controlState = runtime::readAimOverlay();
        const double stateAgeMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - controlState.ts).count();
        const bool fresh = controlState.seq > 0 && stateAgeMs >= 0.0 &&
            stateAgeMs <= runtime::kAimOverlayStaleMs;
        auto formatMs = [fresh](double value) {
            return fresh && value >= 0.0
                ? QStringLiteral("%1 ms").arg(value, 0, 'f', 2)
                : QStringLiteral("--");
        };
        auto formatD = [fresh](double x, double y) {
            return fresh ? QStringLiteral("%1 / %2").arg(x, 0, 'f', 2).arg(y, 0, 'f', 2)
                         : QStringLiteral("--");
        };
        m_controlDt->setText(formatMs(controlState.control_dt_ms));
        m_controlCaptureAge->setText(formatMs(controlState.capture_age_ms));
        m_derivativeRaw->setText(formatD(controlState.derivative_raw_x, controlState.derivative_raw_y));
        m_mouseQueueLatency->setText(
            QString::number(g_mouse_queue_latency_ms.load(), 'f', 2) + QStringLiteral(" ms"));
        m_mouseQueueBacklog->setText(QString::number(g_mouse_queue_backlog.load()));
        m_mouseSendFailures->setText(QString::number(g_mouse_send_failures.load()));
    });
    mouseTimer->start();

    auto* sysCard = new CardWidget(QString::fromUtf8(u8"系统资源"), QStringLiteral("cpu"));
    sysCard->setCollapsible(true);

    m_gpuMemory = new QLabel(QStringLiteral("--"));
    m_gpuMemory->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sysCard->contentLayout()->addWidget(FormKit::fieldRow(QString::fromUtf8(u8"GPU 显存预留"), m_gpuMemory));

    m_cpuCores = new QLabel(QStringLiteral("--"));
    m_cpuCores->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sysCard->contentLayout()->addWidget(FormKit::fieldRow(QString::fromUtf8(u8"CPU 核心预留"), m_cpuCores));

    layout->addWidget(sysCard);
    layout->addStretch();
}

void StatsPage::setFps(double fps) {
    m_fpsValue->setText(QString::number(fps, 'f', 1));
    m_graph->addDataPoint(fps);
}

void StatsPage::setSourceFps(double fps) {
    if (!m_sourceFpsValue) return;
    if (fps > 0.5)
        m_sourceFpsValue->setText(QString::number(fps, 'f', 1));
    else
        m_sourceFpsValue->setText(QStringLiteral("--"));
}

static QString fmtLatencyMs(double ms) {
    if (ms < 0.0) return QStringLiteral("--");
    return QStringLiteral("%1 ms").arg(ms, 0, 'f', 1);
}

void StatsPage::setCaptureLatency(double ms) {
    if (m_captureLatency) m_captureLatency->setText(fmtLatencyMs(ms));
}

void StatsPage::setInferenceLatency(double ms) {
    if (m_inferenceLatency) m_inferenceLatency->setText(fmtLatencyMs(ms));
}

void StatsPage::setTotalLatency(double ms) {
    if (m_totalLatency) m_totalLatency->setText(fmtLatencyMs(ms));
}

void StatsPage::setGpuMemory(const QString& text) {
    m_gpuMemory->setText(text);
}

void StatsPage::setCpuCores(const QString& text) {
    m_cpuCores->setText(text);
}

void StatsPage::setCaptureChainDiagnostics(int deviceAgeUs, double capToDetectMs, double inferMs,
                                           double publishToAimMs, double endToEndMs) {
    auto setMs = [](QLabel* lbl, double ms) {
        if (lbl) lbl->setText(fmtLatencyMs(ms));
    };
    if (m_diagDeviceAge) {
        m_diagDeviceAge->setText(deviceAgeUs < 0
            ? QString::fromUtf8(u8"不可测")
            : QStringLiteral("%1 ms").arg(deviceAgeUs / 1000.0, 0, 'f', 2));
    }
    setMs(m_diagCapToDetect, capToDetectMs);
    setMs(m_diagInfer, inferMs);
    if (m_diagPublishToAim) m_diagPublishToAim->setText(publishToAimMs < 0.0
        ? QString::fromUtf8(u8"等待控制") : fmtLatencyMs(publishToAimMs));
    if (m_diagEndToEnd) m_diagEndToEnd->setText(endToEndMs < 0.0
        ? QString::fromUtf8(u8"等待发送") : fmtLatencyMs(endToEndMs));
}

void StatsPage::setPipelineDiagnostics(double totalGpuMs, double preprocessMs, double engineMs, double copyMs,
                                       double postprocessMs, double aimTickMs, double aimTickPeakMs,
                                       bool graphMode) {
    auto preciseMs = [](double ms) {
        return ms < 0.0 ? QStringLiteral("--")
                        : QStringLiteral("%1 ms").arg(ms, 0, 'f', 3);
    };
    m_gpuPipeline->setText(preciseMs(totalGpuMs));
    const QString graphText = QString::fromUtf8(u8"Graph 内部");
    m_gpuPreprocess->setText(graphMode ? graphText : preciseMs(preprocessMs));
    m_gpuEngine->setText(graphMode ? graphText : preciseMs(engineMs));
    m_gpuCopy->setText(graphMode ? graphText : preciseMs(copyMs));
    m_cpuPostprocess->setText(preciseMs(postprocessMs));
    m_aimTick->setText(preciseMs(aimTickMs));
    m_aimTickPeak->setText(preciseMs(aimTickPeakMs));
}
