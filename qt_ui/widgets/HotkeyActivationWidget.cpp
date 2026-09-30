#include "HotkeyActivationWidget.h"
#include "keyboard/hotkey_selection.h"
#include "macro/macro_config.h"
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

HotkeyActivationWidget::HotkeyActivationWidget(QWidget* parent) : QWidget(parent)
{
    setObjectName("hotkeyActivation");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 6, 0, 0);
    auto* hint = new QLabel(QStringLiteral("同组存在相同触发按键。激活一套后，按触发键会使用它的全部设置。"), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    status_ = new QLabel(this);
    status_->setObjectName("hotkeyActivationStatus");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    activate_ = new QPushButton(QStringLiteral("激活当前热键"), this);
    activate_->setObjectName("activateHotkeyButton");
    layout->addWidget(activate_);
    layout->addWidget(new QLabel(QStringLiteral("激活当前热键的快捷键"), this));
    key_ = new QComboBox(this);
    key_->setObjectName("hotkeyActivationKey");
    key_->addItem(QStringLiteral("未设置（使用上方按钮）"), QString());
    for (const auto& key : macros::keys())
        key_->addItem(QString::fromStdString(key.label), QString::fromStdString(key.id));
    const char* mouseKeys[] = {"LeftMouseButton", "RightMouseButton", "MiddleMouseButton", "X1MouseButton", "X2MouseButton"};
    const QString labels[] = {QStringLiteral("鼠标左键"), QStringLiteral("鼠标右键"), QStringLiteral("鼠标中键"), QStringLiteral("鼠标侧键4"), QStringLiteral("鼠标侧键5")};
    for (int i = 0; i < 5; ++i) key_->addItem(labels[i], QString::fromLatin1(mouseKeys[i]));
    layout->addWidget(key_);
    connect(activate_, &QPushButton::clicked, this, &HotkeyActivationWidget::activateRequested);
    connect(key_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        emit activationKeyChanged(key_->currentData().toString());
    });
    hide();
}

void HotkeyActivationWidget::load(const std::vector<HotkeyProfile>& profiles, int index)
{
    const bool conflict = hasHotkeyConflict(profiles, index);
    setVisible(conflict);
    if (!conflict) return;
    const auto& profile = profiles[index];
    const QSignalBlocker blocker(key_);
    const int keyIndex = key_->findData(QString::fromStdString(profile.activation_key));
    key_->setCurrentIndex(std::max(0, keyIndex));
    const int preferred = preferredHotkey(profiles, index);
    const bool selected = preferred == index;
    activate_->setText(selected ? QStringLiteral("当前热键已激活") : QStringLiteral("激活当前热键"));
    activate_->setEnabled(!selected);
    QString status = QStringLiteral("当前使用：%1").arg(QString::fromStdString(profiles[preferred].name));
    if (!profile.activation_key.empty() && (!validActivationKey(profiles, index) || keyIndex < 0))
        status += QStringLiteral("\n激活快捷键不可用：请选择不同于同组触发键、且不与同按键配置的激活键重复的按键。");
    status_->setText(status);
}
