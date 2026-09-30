#include "pages/CapturePage.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/IconFont.h"
#include "widgets/ToggleSwitch.h"

#include "capture/capture_card_probe.h"
#include "capture/magewell_capture.h"
#include "capture/stream_capture.h"

#include <algorithm>
#include <utility>
#include <QComboBox>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPoint>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {

QIcon iconFromGlyph(const QString& name, int px, const QString& color) {
    if (!IconFont::available())
        return QIcon();
    const qreal dpr = 2.0;
    QPixmap pm(QSize(static_cast<int>(px * dpr), static_cast<int>(px * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setFont(IconFont::font(px));
    p.setPen(QColor(color));
    p.drawText(QRectF(0, 0, px, px), Qt::AlignCenter,
               QString(IconFont::glyph(name)));
    p.end();
    return QIcon(pm);
}

}

CapturePage::CapturePage(QWidget* parent)
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

    buildCardCard(layout);

    layout->addStretch();

    auto& cfg = ConfigManager::instance();
    connect(&cfg, &ConfigManager::configLoaded, this, &CapturePage::onLoadConfig);
    onLoadConfig();
}

void CapturePage::buildCardCard(QVBoxLayout* layout) {
    m_cardCard = new CardWidget(
        QStringLiteral("采集卡"),
        QStringLiteral("device-camera-phone"));

    m_error = new QLabel;
    m_error->setWordWrap(true);
    m_error->setStyleSheet(QStringLiteral("color:#ff6b6b;"));
    m_cardCard->contentLayout()->addWidget(m_error);
    m_error->hide();

    m_sourceCombo = new QComboBox;
    m_sourceCombo->addItem(QStringLiteral("采集卡"), QStringLiteral("device"));
    m_sourceCombo->addItem(QStringLiteral("OBS / FFmpeg · UDP"), QStringLiteral("udp"));
    m_sourceCombo->addItem(QStringLiteral("OBS / FFmpeg · TCP"), QStringLiteral("tcp"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("采集来源"), m_sourceCombo));
    connect(m_sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CapturePage::onSourceChanged);

    m_streamUrl = new QLineEdit;
    m_streamUrl->setPlaceholderText(QStringLiteral("udp://0.0.0.0:23000"));
    m_streamUrl->setToolTip(QStringLiteral(
        "UDP 接收示例：udp://0.0.0.0:23000?fifo_size=500000&overrun_nonfatal=1\n"
        "TCP 监听示例：tcp://0.0.0.0:23000（自动监听）\n"
        "先启动 Apotheosis，再在 OBS/FFmpeg 中开始向同一端口发送 MPEG-TS 视频流。"));
    m_streamRow = FormKit::fieldRow(QStringLiteral("接收地址"), m_streamUrl);
    m_cardCard->contentLayout()->addWidget(m_streamRow);
    connect(m_streamUrl, &QLineEdit::editingFinished,
            this, &CapturePage::onStreamUrlEdited);

    const QString streamNote = QStringLiteral(
        "OBS：输出模式选“高级” → 自定义输出 (FFmpeg) → 输出到 URL；"
        "容器选 MPEG-TS。UDP 发送到 udp://本机IP:23000；"
        "TCP 发送到 tcp://本机IP:23000（OBS 端不要加 listen=1）。");
    m_streamUrl->setToolTip(m_streamUrl->toolTip() + "\n\n" + streamNote);

    m_devCombo = new QComboBox;
    m_devCombo->setToolTip(tr(
        "系统实际枚举到的视频采集卡。\n"
        "只有一张卡也请显式选择 —— 你选的那张不在时程序不会自动换一张。"));

    m_refreshBtn = new QPushButton;
    m_refreshBtn->setCursor(Qt::PointingHandCursor);
    m_refreshBtn->setFixedSize(28, 28);
    m_refreshBtn->setToolTip(tr(
        "重新枚举采集卡, 并读取每张卡真实支持的 格式 / 分辨率 / 帧率。\n"
        "每张卡需要 20-300ms, 所以只在打开本页或手动点击时执行。"));
    m_refreshBtn->setStyleSheet(QStringLiteral(
        "QPushButton { border:1px solid #D8DEE9; border-radius:14px;"
        "              background:#FFFFFF; }"
        "QPushButton:hover { background:#F1F4F9; }"
        "QPushButton:pressed { background:#E4E9F2; }"));
    if (IconFont::available()) {
        m_refreshBtn->setIcon(iconFromGlyph(QStringLiteral("refresh"), 15,
                                           QStringLiteral("#5B6472")));
        m_refreshBtn->setIconSize(QSize(15, 15));
    } else {
        m_refreshBtn->setText(QStringLiteral("\u21bb"));
    }
    connect(m_refreshBtn, &QPushButton::clicked, this, &CapturePage::refreshDevices);

    auto* devRow = new QWidget;
    auto* devRowLayout = new QHBoxLayout(devRow);
    devRowLayout->setContentsMargins(0, 0, 0, 0);
    devRowLayout->setSpacing(8);
    devRowLayout->addWidget(m_devCombo, 1);
    devRowLayout->addWidget(m_refreshBtn, 0);

    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("采集卡"), devRow));
    connect(m_devCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CapturePage::onDeviceChanged);

    m_fmtCombo = new QComboBox;
    m_fmtCombo->setToolTip(tr(
        "当前采集卡真实支持的像素格式, 按延迟从低到高排列。\n"
        "NV12 / YUY2 / RGB32 是未压缩格式, 没有编解码往返, 延迟最低;\n"
        "MJPG 需要卡内编码 + 进程内解码, 但在高分辨率高帧率下更省带宽。"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("像素格式"), m_fmtCombo));
    connect(m_fmtCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CapturePage::onFormatChanged);

    m_resCombo = new QComboBox;
    m_resCombo->setToolTip(tr(
        "当前格式下设备真实支持的分辨率。换成别的格式, 这里的可选项会跟着变。"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("分辨率"), m_resCombo));
    connect(m_resCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CapturePage::onResolutionChanged);

    m_fpsCombo = new QComboBox;
    m_fpsCombo->setToolTip(tr(
        "当前 格式 + 分辨率 下设备真实支持的帧率。\n"
        "换个格式或分辨率, 这里的可选项会跟着变 —— 不支持的帧率不会出现。"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("帧率"), m_fpsCombo));
    connect(m_fpsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CapturePage::onFpsChanged);

    m_gpuDecode = new ToggleSwitch;
    m_gpuDecode->setToolTip(tr(
        "MJPG 用 nvJPEG 在 GPU 上解码, 原始格式用 NPP / CUDA kernel 转换,\n"
        "产出 GPU 帧直送 TensorRT, 延迟最低。"));
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("GPU 解码"), m_gpuDecode));
    connect(m_gpuDecode, &ToggleSwitch::toggled,
            this, [](bool on) { ConfigManager::instance().setCaptureGpuDecode(on); });

    m_capSummary = new QLabel;
    m_capSummary->setWordWrap(true);
    m_capSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("设备能力"), m_capSummary));

    m_recommend = new QLabel;
    m_recommend->setWordWrap(true);
    m_recommend->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_cardCard->contentLayout()->addWidget(
        FormKit::fieldRow(QStringLiteral("推荐配置"), m_recommend));

    layout->addWidget(m_cardCard);
}

const MFDeviceInfo* CapturePage::currentDevice() const {
    const int i = m_devCombo->currentIndex();
    if (i < 0 || i >= static_cast<int>(m_devices.size()))
        return nullptr;
    return &m_devices[static_cast<size_t>(i)];
}

void CapturePage::showError(const QString& text) {
    m_error->setText(text);
    m_error->setVisible(!text.isEmpty());
}

void CapturePage::clearError() { showError(QString()); }

void CapturePage::updateSourceUi() {
    const bool network = m_sourceCombo->currentData().toString() != QStringLiteral("device");
    m_streamRow->setVisible(network);
    m_streamUrl->setEnabled(network);
    m_devCombo->setEnabled(!network);
    m_refreshBtn->setEnabled(!network);
    m_fmtCombo->setEnabled(!network);
    m_resCombo->setEnabled(!network);
    m_fpsCombo->setEnabled(!network);
    m_gpuDecode->setEnabled(!network &&
        !(currentDevice() && magewell::IsDeviceKey(currentDevice()->friendly_name)));
    updateCapabilitySummary();
}

void CapturePage::onSourceChanged(int) {
    if (m_restoring || m_sourceCombo->currentIndex() < 0) return;
    const QString source = m_sourceCombo->currentData().toString();
    auto& cfg = ConfigManager::instance();
    if (source != QStringLiteral("device")) {
        const QString prefix = source + QStringLiteral("://");
        if (!m_streamUrl->text().startsWith(prefix) ||
            !stream_capture::ValidateUrl(source.toStdString(),
                                         m_streamUrl->text().trimmed().toStdString()))
            m_streamUrl->setText(source == QStringLiteral("udp")
                ? QStringLiteral("udp://0.0.0.0:23000?fifo_size=500000&overrun_nonfatal=1")
                : QStringLiteral("tcp://0.0.0.0:23000"));
        cfg.setCaptureStreamUrl(m_streamUrl->text().trimmed());
    }
    cfg.setCaptureSource(source);
    clearError();
    updateSourceUi();
    if (source == QStringLiteral("device")) refreshDevices();
}

void CapturePage::onStreamUrlEdited() {
    const QString source = m_sourceCombo->currentData().toString();
    if (source == QStringLiteral("device")) return;
    const QString url = m_streamUrl->text().trimmed();
    std::string reason;
    if (!stream_capture::ValidateUrl(source.toStdString(), url.toStdString(), &reason)) {
        showError(QStringLiteral("接收地址无效：%1").arg(QString::fromStdString(reason)));
        return;
    }
    ConfigManager::instance().setCaptureStreamUrl(url);
    clearError();
    updateSourceUi();
}

void CapturePage::refreshDevices() {
    if (m_sourceCombo->currentData().toString() != QStringLiteral("device")) {
        updateSourceUi();
        return;
    }
    const bool prevRestoring = m_restoring;
    m_restoring = true;

    struct RestoreGuard {
        bool& flag;
        bool  prev;
        ~RestoreGuard() { flag = prev; }
    } restoreGuard{ m_restoring, prevRestoring };

    QString want = m_devCombo->currentIndex() >= 0
        ? m_devCombo->currentData().toString()
        : ConfigManager::instance().captureDevice();
    if (want.isEmpty())
        want = ConfigManager::instance().captureDevice();

    m_devices = capture_card::ProbeAll();
    for (const auto& sdk : magewell::EnumerateDevices()) {
        MFDeviceInfo d;
        d.index = -1;
        d.name = sdk.name;
        d.friendly_name = sdk.key;
        d.caps_probed = true;
        if (sdk.signal_width > 0 && sdk.signal_height > 0) {
            MFCapability cap;
            cap.format = "SDK BGR24";
            cap.width = sdk.signal_width;
            cap.height = sdk.signal_height;
            cap.fps = {sdk.signal_fps > 0 ? sdk.signal_fps : 60};
            cap.supported = true;
            d.caps.push_back(std::move(cap));
        }
        m_devices.push_back(std::move(d));
    }

    {
        QSignalBlocker block(m_devCombo);
        m_devCombo->clear();
        for (const auto& d : m_devices)
            m_devCombo->addItem(QString::fromStdString(d.name),
                                QString::fromStdString(d.friendly_name));
    }

    if (m_devices.empty()) {
        showError(QStringLiteral(
            "没有检测到任何视频采集设备。请确认采集卡已经插好、驱动正常。"));
        m_devCombo->setCurrentIndex(-1);
        rebuildFormatCombo();
        return;
    }

    const int idx = m_devCombo->findData(want);
    if (want.isEmpty()) {
        m_devCombo->setCurrentIndex(-1);
        clearError();
    } else if (idx < 0) {
        m_devCombo->setCurrentIndex(-1);
        showError(QStringLiteral(
            "上次选择的采集卡「%1」当前不在线。请重新选择一张卡。")
            .arg(want));
    } else {
        m_devCombo->setCurrentIndex(idx);
        clearError();
    }

    rebuildFormatCombo();
    const MFDeviceInfo* selected = currentDevice();
    m_gpuDecode->setEnabled(!(selected && magewell::IsDeviceKey(selected->friendly_name)));
}

void CapturePage::onDeviceChanged(int) {
    if (m_sourceCombo->currentData().toString() != QStringLiteral("device")) return;
    const MFDeviceInfo* dev = currentDevice();
    const bool sdk = dev && magewell::IsDeviceKey(dev->friendly_name);
    m_gpuDecode->setEnabled(!sdk);
    if (sdk)
        ConfigManager::instance().setCaptureDevice(
            QString::fromStdString(dev->friendly_name));
    rebuildFormatCombo();
}

void CapturePage::rebuildFormatCombo() {
    const MFDeviceInfo* dev = currentDevice();

    {
        QSignalBlocker b1(m_fmtCombo), b2(m_resCombo), b3(m_fpsCombo);
        m_fmtCombo->clear();
        m_resCombo->clear();
        m_fpsCombo->clear();
    }

    if (!dev) {
        updateCapabilitySummary();
        return;
    }

    if (dev->caps.empty() && !magewell::IsDeviceKey(dev->friendly_name))
        showError(QStringLiteral("采集卡能力读取失败：%1。请点击右侧刷新重试。")
                  .arg(QString::fromStdString(mfcap::Describe(*dev))));

    for (const auto& f : mfcap::Formats(*dev))
        m_fmtCombo->addItem(QString::fromStdString(f));

    const QString want = ConfigManager::instance().captureFormat();
    int fi = m_fmtCombo->findText(want);
    if (fi < 0 && magewell::IsDeviceKey(dev->friendly_name)
        && m_fmtCombo->count() > 0)
        fi = 0;
    m_fmtCombo->setCurrentIndex(fi);

    if (fi < 0 && !want.isEmpty() && m_fmtCombo->count() > 0) {
        showError(QStringLiteral(
            "采集卡不支持配置里的像素格式「%1」, 请从下面的选项里重新选择。")
            .arg(want));
    } else if (fi >= 0) {
        clearError();
    }

    rebuildResolutionCombo();
}

void CapturePage::onFormatChanged(int) { rebuildResolutionCombo(); }

void CapturePage::rebuildResolutionCombo() {
    const MFDeviceInfo* dev = currentDevice();
    const QString fmt = m_fmtCombo->currentIndex() >= 0 ? m_fmtCombo->currentText()
                                                       : QString();

    {
        QSignalBlocker b2(m_resCombo), b3(m_fpsCombo);
        m_resCombo->clear();
        m_fpsCombo->clear();
    }

    if (!dev || fmt.isEmpty()) {
        updateCapabilitySummary();
        return;
    }

    const auto res = mfcap::Resolutions(*dev, fmt.toStdString());
    for (const auto& wh : res)
        m_resCombo->addItem(QStringLiteral("%1 × %2").arg(wh.first).arg(wh.second),
                            QPoint(wh.first, wh.second));

    const auto& cfg = ConfigManager::instance();
    int ri = -1;
    for (int i = 0; i < m_resCombo->count(); ++i) {
        const QPoint p = m_resCombo->itemData(i).toPoint();
        if (p.x() == cfg.captureWidth() && p.y() == cfg.captureHeight()) { ri = i; break; }
    }
    m_resCombo->setCurrentIndex(ri);
    if (ri < 0 && m_resCombo->count() > 0)
        m_resCombo->setCurrentIndex(0);

    rebuildFpsCombo();
}

void CapturePage::onResolutionChanged(int) { rebuildFpsCombo(); }

void CapturePage::rebuildFpsCombo() {
    const MFDeviceInfo* dev = currentDevice();
    const QString fmt = m_fmtCombo->currentIndex() >= 0 ? m_fmtCombo->currentText()
                                                       : QString();
    const QPoint res = m_resCombo->currentIndex() >= 0
        ? m_resCombo->currentData().toPoint() : QPoint(0, 0);

    {
        QSignalBlocker b3(m_fpsCombo);
        m_fpsCombo->clear();
    }

    if (!dev || fmt.isEmpty() || res.x() <= 0) {
        updateCapabilitySummary();
        return;
    }

    for (int f : mfcap::FpsList(*dev, fmt.toStdString(), res.x(), res.y()))
        m_fpsCombo->addItem(QStringLiteral("%1 fps").arg(f), f);

    const int want = ConfigManager::instance().captureFps();
    int fi = m_fpsCombo->findData(want);
    if (fi < 0 && m_fpsCombo->count() > 0)
        fi = m_fpsCombo->count() - 1;
    m_fpsCombo->setCurrentIndex(fi);

    if (fi >= 0)
        applySelectionToConfig();

    updateCapabilitySummary();
}

void CapturePage::onFpsChanged(int) {
    applySelectionToConfig();
}

void CapturePage::applySelectionToConfig() {
    if (m_restoring) return;
    if (m_sourceCombo->currentData().toString() != QStringLiteral("device")) return;

    const MFDeviceInfo* dev = currentDevice();
    if (!dev) return;
    if (m_fmtCombo->currentIndex() < 0) return;
    if (m_resCombo->currentIndex() < 0) return;
    if (m_fpsCombo->currentIndex() < 0) return;

    const QString fmt = m_fmtCombo->currentText();
    const QPoint res = m_resCombo->currentData().toPoint();
    const int fps = m_fpsCombo->currentData().toInt();

    if (!mfcap::Validate(*dev, fmt.toStdString(), res.x(), res.y(), fps)) {
        showError(QStringLiteral(
            "内部错误: 选中的组合 %1 %2×%3@%4fps 未通过设备能力校验。")
            .arg(fmt).arg(res.x()).arg(res.y()).arg(fps));
        return;
    }

    auto& cfg = ConfigManager::instance();
    cfg.setCaptureDevice(m_devCombo->currentData().toString());
    cfg.setCaptureFormat(fmt);
    cfg.setCaptureWidth(res.x());
    cfg.setCaptureHeight(res.y());
    cfg.setCaptureFps(fps);
    clearError();
}

void CapturePage::updateCapabilitySummary() {
    if (m_sourceCombo->currentData().toString() != QStringLiteral("device")) {
        m_capSummary->setText(QStringLiteral("等待网络视频流；使用 FFmpeg 解码后输出中心裁切画面。"));
        m_recommend->setText(QStringLiteral("发送端使用 MPEG-TS 视频流，接收端地址与端口按上方设置。"));
        return;
    }
    const MFDeviceInfo* dev = currentDevice();

    if (!dev) {
        m_capSummary->setText(QStringLiteral("—"));
        m_recommend->setText(QStringLiteral("—"));
        return;
    }

    if (magewell::IsDeviceKey(dev->friendly_name) && dev->caps.empty())
        m_capSummary->setText(QStringLiteral("美乐威 SDK 已识别设备；当前无锁定的视频信号。"));
    else if (dev->directshow_fallback)
        m_capSummary->setText(QStringLiteral("DirectShow 回退：%1")
                              .arg(QString::fromStdString(mfcap::Describe(*dev))));
    else
        m_capSummary->setText(QString::fromStdString(mfcap::Describe(*dev)));

    const auto& cfg = ConfigManager::instance();
    const int side = cfg.detectionResolution();

    std::string rf, why;
    int rw = 0, rh = 0, rffps = 0;
    if (magewell::IsDeviceKey(dev->friendly_name) && !dev->caps.empty())
        m_recommend->setText(QStringLiteral("SDK 读取当前输入信号，并直接输出中心 %1×%1 BGR 图像。")
                             .arg(side));
    else if (mfcap::PickBest(*dev, side, side, 240, rf, rw, rh, rffps, &why))
        m_recommend->setText(QString::fromStdString(why));
    else
        m_recommend->setText(QStringLiteral("该设备没有可用于采集的组合。"));
}

void CapturePage::onLoadConfig() {
    auto& cfg = ConfigManager::instance();

    m_gpuDecode->setChecked(cfg.captureGpuDecode());

    m_restoring = true;
    {
        QSignalBlocker blocker(m_sourceCombo);
        const int sourceIndex = m_sourceCombo->findData(cfg.captureSource());
        m_sourceCombo->setCurrentIndex(sourceIndex >= 0 ? sourceIndex : 0);
    }
    m_streamUrl->setText(cfg.captureStreamUrl());
    refreshDevices();
    m_restoring = false;
    updateSourceUi();
}
