#include "pages/CapturePage.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/IconFont.h"
#include "widgets/ToggleSwitch.h"

#include "capture/capture_card_probe.h"

#include <QComboBox>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
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

void CapturePage::refreshDevices() {
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
}

void CapturePage::onDeviceChanged(int) {
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

    for (const auto& f : mfcap::Formats(*dev))
        m_fmtCombo->addItem(QString::fromStdString(f));

    const QString want = ConfigManager::instance().captureFormat();
    const int fi = m_fmtCombo->findText(want);
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
    const MFDeviceInfo* dev = currentDevice();

    if (!dev) {
        m_capSummary->setText(QStringLiteral("—"));
        m_recommend->setText(QStringLiteral("—"));
        return;
    }

    m_capSummary->setText(QString::fromStdString(mfcap::Describe(*dev)));

    const auto& cfg = ConfigManager::instance();
    const int side = cfg.detectionResolution();

    std::string rf, why;
    int rw = 0, rh = 0, rffps = 0;
    if (mfcap::PickBest(*dev, side, side, 240, rf, rw, rh, rffps, &why))
        m_recommend->setText(QString::fromStdString(why));
    else
        m_recommend->setText(QStringLiteral("该设备没有可用于采集的组合。"));
}

void CapturePage::onLoadConfig() {
    auto& cfg = ConfigManager::instance();

    m_gpuDecode->setChecked(cfg.captureGpuDecode());

    m_restoring = true;
    refreshDevices();
    m_restoring = false;
    updateCapabilitySummary();
}
