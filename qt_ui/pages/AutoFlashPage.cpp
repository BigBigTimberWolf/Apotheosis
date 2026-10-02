#include "pages/AutoFlashPage.h"

#include "Apotheosis.h"
#include "config/ConfigManager.h"
#include "config/config_bridge.h"
#include "macro/macro_config.h"
#include "runtime/aim_telemetry.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>
#include <mutex>

AutoFlashPage::AutoFlashPage(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(14);
    scroll->setWidget(content);

    auto* card = new CardWidget(QStringLiteral("自动爆闪"), QStringLiteral("keyboard"));
    enabled_ = new QCheckBox(QStringLiteral("启用自动爆闪"));
    card->contentLayout()->addWidget(enabled_);
    const QString hint = QStringLiteral(
        "按住瞄准热键且已锁定目标时，目标框面积达到设定比例就点按一次。"
        "目标持续大于阈值不会连续按；缩小到阈值以下或锁定新目标后可再次触发。");
    card->setToolTip(hint);
    enabled_->setToolTip(hint);

    area_ = new QDoubleSpinBox;
    area_->setRange(0.1, 100.0);
    area_->setDecimals(1);
    area_->setSingleStep(0.5);
    area_->setSuffix(QStringLiteral(" %"));
    card->contentLayout()->addWidget(FormKit::fieldRow(
        QStringLiteral("框面积占检测画面"), area_));

    key_ = new QComboBox;
    key_->addItem(QStringLiteral("请选择输出按键"), QString());
    const struct { const char* id; const char* label; } mouseKeys[] = {
        {"LeftMouseButton", "鼠标左键"}, {"RightMouseButton", "鼠标右键"},
        {"MiddleMouseButton", "鼠标中键"}, {"X1MouseButton", "鼠标侧键 4"},
        {"X2MouseButton", "鼠标侧键 5"}};
    for (const auto& mouse : mouseKeys)
        key_->addItem(QString::fromUtf8(mouse.label), QString::fromUtf8(mouse.id));
    for (const auto& keyboard : macros::keys())
        key_->addItem(QString::fromStdString(keyboard.label),
                      QString::fromStdString(keyboard.id));
    card->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("点按按键"), key_));

    currentArea_ = new QLabel;
    currentArea_->setStyleSheet(QStringLiteral("color:#D5B56B;font-size:13px;font-weight:600;"));
    card->contentLayout()->addWidget(currentArea_);
    layout->addWidget(card);
    layout->addStretch();

    connect(enabled_, &QCheckBox::toggled, this, [this] { commit(); });
    connect(area_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this] { commit(); });
    connect(key_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this] { commit(); });
    connect(&ConfigManager::instance(), &ConfigManager::configLoaded,
            this, [this] { refresh(); });
    auto* timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, [this] { refreshArea(); });
    timer->start();
    refresh();
    refreshArea();
}

void AutoFlashPage::refresh() {
    bool enabled;
    double area;
    QString key;
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        enabled = config.auto_flash_enabled;
        area = config.auto_flash_area_percent;
        key = QString::fromStdString(config.auto_flash_key);
    }
    const QSignalBlocker b1(enabled_), b2(area_), b3(key_);
    enabled_->setChecked(enabled);
    area_->setValue(area);
    const int index = key_->findData(key);
    key_->setCurrentIndex(index >= 0 ? index : 0);
}

void AutoFlashPage::commit() {
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        config.auto_flash_enabled = enabled_->isChecked();
        config.auto_flash_area_percent = area_->value();
        config.auto_flash_key = key_->currentData().toString().toStdString();
    }
    ConfigBridge::instance().markDirty();
}

void AutoFlashPage::refreshArea() {
    const auto state = runtime::readAimOverlay();
    const auto now = std::chrono::steady_clock::now();
    int resolution;
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        resolution = config.detection_resolution;
    }
    if (!state.valid || !state.engaged || state.ts.time_since_epoch().count() == 0 ||
        now - state.ts > std::chrono::milliseconds(250) || resolution <= 0) {
        currentArea_->setText(QStringLiteral("当前锁定目标面积：—"));
        return;
    }
    const double pct = 100.0 * state.box.area() /
        (static_cast<double>(resolution) * resolution);
    currentArea_->setText(QStringLiteral("当前锁定目标面积：%1 %").arg(pct, 0, 'f', 1));
}
