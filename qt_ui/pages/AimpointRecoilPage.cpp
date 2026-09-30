#include "pages/AimpointRecoilPage.h"

#include "Apotheosis.h"
#include "config/config_bridge.h"
#include "config/ConfigManager.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"

#include <QDoubleSpinBox>
#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <mutex>

AimpointRecoilPage::AimpointRecoilPage(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(14);
    scroll->setWidget(content);

    auto* card = new CardWidget(QStringLiteral("瞄点压枪参数"), QStringLiteral("target"));

    speed_ = new QDoubleSpinBox;
    speed_->setObjectName("recoilSpeed");
    speed_->setRange(0.0, 2000.0);
    speed_->setDecimals(1);
    speed_->setSingleStep(5.0);
    speed_->setSuffix(QStringLiteral(" px/s"));
    card->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("每秒下移"), speed_));

    maximum_ = new QDoubleSpinBox;
    maximum_->setObjectName("recoilMaximum");
    maximum_->setRange(0.0, 2000.0);
    maximum_->setDecimals(1);
    maximum_->setSingleStep(5.0);
    maximum_->setSuffix(QStringLiteral(" px"));
    card->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("最多下移"), maximum_));

    fireKey_ = new QComboBox;
    fireKey_->setObjectName("recoilFireKey");
    fireKey_->addItem(QStringLiteral("鼠标左键"), QStringLiteral("LeftMouseButton"));
    fireKey_->addItem(QStringLiteral("鼠标右键"), QStringLiteral("RightMouseButton"));
    fireKey_->addItem(QStringLiteral("鼠标中键"), QStringLiteral("MiddleMouseButton"));
    fireKey_->addItem(QStringLiteral("鼠标侧键 4"), QStringLiteral("X1MouseButton"));
    fireKey_->addItem(QStringLiteral("鼠标侧键 5"), QStringLiteral("X2MouseButton"));
    card->contentLayout()->addWidget(FormKit::fieldRow(QStringLiteral("开火键"), fireKey_));

    const QString hint = QStringLiteral(
        "在「瞄准设置」中选中已有热键，并将「瞄准方式」设为「瞄点压枪」。"
        "同时按住瞄准热键和这里设置的开火键时，目标瞄点才开始向下移动；松开任一键即复位。"
        "自动扳机成功按住左键时也会触发，松开后复位。"
        "这里的速度和最大偏移对所有选择该模式的热键生效，单位为检测画面的像素。");
    card->setToolTip(hint);
    speed_->setToolTip(hint);
    maximum_->setToolTip(hint);
    fireKey_->setToolTip(hint);
    layout->addWidget(card);
    layout->addStretch();

    connect(speed_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this] { commit(); });
    connect(maximum_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this] { commit(); });
    connect(fireKey_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this] { commit(); });
    connect(&ConfigManager::instance(), &ConfigManager::configLoaded,
            this, [this] { refresh(); });
    refresh();
}

void AimpointRecoilPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh();
}

void AimpointRecoilPage::refresh() {
    loading_ = true;
    std::lock_guard<std::recursive_mutex> lock(configMutex);
    QSignalBlocker speedBlock(speed_);
    QSignalBlocker maximumBlock(maximum_);
    QSignalBlocker fireKeyBlock(fireKey_);
    speed_->setValue(config.aimpoint_recoil_speed_px_s);
    maximum_->setValue(config.aimpoint_recoil_max_px);
    const QString fireKey = QString::fromStdString(config.aimpoint_recoil_fire_key);
    const int index = fireKey_->findData(fireKey);
    fireKey_->setCurrentIndex(index >= 0 ? index : 0);
    loading_ = false;
}

void AimpointRecoilPage::commit() {
    if (loading_) return;
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        config.aimpoint_recoil_speed_px_s = speed_->value();
        config.aimpoint_recoil_max_px = maximum_->value();
        config.aimpoint_recoil_fire_key = fireKey_->currentData().toString().toStdString();
    }
    ConfigBridge::instance().markDirty();
}
