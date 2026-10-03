#include "pages/AimSettingsPage.h"

#include <QCheckBox>
#include <QDialog>
#include <QDropEvent>
#include <QTimer>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>          // QFrame::NoFrame
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>       // QLineEdit::Normal (QInputDialog 参数)
#include <QListWidget>
#include <QListWidgetItem> // 显式包含, 不依赖 QListWidget 的传递包含
#include <QMessageBox>     // 删除热键组的确认框
#include <QMenu>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QShowEvent>      // showEvent 的参数类型
#include <QSpinBox>
#include <QSplitter>       // 左栏/右栏可拖动分隔（旧页的写法）
#include <QVBoxLayout>
#include <QWheelEvent>     // NoWheelSpinBox/NoWheelDoubleSpinBox 的 wheelEvent 参数类型

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>   // std::size (开镜档的行表)
#include <mutex>

#include "Apotheosis.h"          // config / configMutex
#include "config.h"
#include "config/ConfigManager.h"
#include "config/config_bridge.h"
#include "macro/macro_config.h"
#include "pages/TargetPage.h"    // setTargetPage(): 取 &TargetPage::classFiltersChanged 需要完整定义
#include "runtime/config_snapshot.h"
#include "keyboard/hotkey_blocking.h"
#include "keyboard/hotkey_selection.h"
#include "widgets/HotkeyActivationWidget.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include "widgets/TriggerWorkflowEditor.h"
#include "widgets/TriggerTargetEditor.h"
#include "widgets/NeuralCurveTrainer.h"
#include "widgets/NeuralCurveFile.h"
#include "widgets/ToggleSwitch.h"

namespace
{

// QSpinBox/QDoubleSpinBox 默认只要鼠标悬停在上面就会响应滚轮改值——这个页面
// 塞在 QScrollArea 里、输入框又多又密, 用户想滚动整页时鼠标几乎必然会经过
// 某个输入框, 于是滚一下页面顺手就改掉了一个参数的值(而且不容易察觉)。
// 这里改成"没有键盘焦点就不响应滚轮", 滚轮事件原样丢给上层去滚页面。
class NoWheelSpinBox : public QSpinBox
{
public:
    using QSpinBox::QSpinBox;
protected:
    void wheelEvent(QWheelEvent* e) override
    {
        if (!hasFocus()) { e->ignore(); return; }
        QSpinBox::wheelEvent(e);
    }
};

class NoWheelDoubleSpinBox : public QDoubleSpinBox
{
public:
    using QDoubleSpinBox::QDoubleSpinBox;
protected:
    void wheelEvent(QWheelEvent* e) override
    {
        if (!hasFocus()) { e->ignore(); return; }
        QDoubleSpinBox::wheelEvent(e);
    }
};

class ReorderableHotkeyList final : public QListWidget
{
public:
    using QListWidget::QListWidget;
    std::function<void(int, int)> moveRequested;

protected:
    void startDrag(Qt::DropActions supportedActions) override
    {
        draggedRow_ = currentRow();
        QListWidget::startDrag(supportedActions);
        draggedRow_ = -1;
    }

    void dropEvent(QDropEvent* event) override
    {
        if (event->source() != this || !moveRequested) {
            event->ignore();
            return;
        }
        const int from = draggedRow_;
        if (from < 0) {
            event->ignore();
            return;
        }
        const auto* target = itemAt(event->position().toPoint());
        int insertion = target ? row(target) : count();
        if (target && dropIndicatorPosition() == QAbstractItemView::BelowItem)
            ++insertion;
        if (!target) {
            for (int rowIndex = 0; rowIndex < count(); ++rowIndex)
                if (event->position().y() < visualItemRect(item(rowIndex)).center().y()) {
                    insertion = rowIndex;
                    break;
                }
        }
        const int to = insertion > from ? insertion - 1 : insertion;
        if (to == from || to < 0 || to >= count()) {
            event->ignore();
            return;
        }
        // Reorder the config ourselves. Reject Qt's model-level MoveAction so
        // the drag source cannot also remove a row from the QListWidget.
        event->ignore();
        QTimer::singleShot(0, this, [this, from, to] {
            if (moveRequested) moveRequested(from, to);
        });
    }

private:
    int draggedRow_ = -1;
};

struct KeyEntry { const char* id; const char* label; };
const KeyEntry kKeyEntries[] = {
    { "",                 "无 (始终活跃)" },
    { "RightMouseButton", "鼠标右键 (RightMouseButton)" },
    { "X1MouseButton",    "鼠标侧键4 (X1MouseButton)" },
    { "X2MouseButton",    "鼠标侧键5 (X2MouseButton)" },
    { "MiddleMouseButton","鼠标中键 (MiddleMouseButton)" },
    { "LeftMouseButton",  "鼠标左键 (LeftMouseButton)" },
    { nullptr, nullptr }
};

}

QLabel* AimSettingsPage::makeHint(const QString& text)
{
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setProperty("class", "hint");
    return l;
}

QLabel* AimSettingsPage::makeSectionTitle(const QString& text)
{
    auto* l = new QLabel(text);
    l->setProperty("class", "heading");
    return l;
}

void AimSettingsPage::attachTip(QWidget* row, const QString& tip)
{
    if (!row || tip.isEmpty()) return;
    row->setToolTip(tip);
    const auto kids = row->findChildren<QWidget*>();
    for (QWidget* w : kids)
        w->setToolTip(tip);
}

QWidget* AimSettingsPage::makePathDoubleRow(const char* obj, const char* label,
                                            double lo, double hi, double step,
                                            double def, const QString& tip)
{
    auto* sp = new NoWheelDoubleSpinBox;
    sp->setRange(lo, hi);
    sp->setSingleStep(step);
    sp->setDecimals(3);
    sp->setObjectName(QString::fromUtf8(obj));
    sp->setValue(def);
    m_pathDoubles.push_back(sp);
    auto* row = FormKit::fieldRow(QString::fromUtf8(label), sp);
    attachTip(row, tip);
    return row;
}

QWidget* AimSettingsPage::makeIntRow(const char* obj, const char* label, int lo, int hi,
                                     int step, int def, const QString& tip)
{
    const QString name = QString::fromUtf8(obj);

    auto* sp = new NoWheelSpinBox;
    sp->setRange(lo, hi);
    sp->setSingleStep(step);
    sp->setObjectName(name);
    sp->setValue(def);
    m_pathInts.push_back(sp);
    auto* row = FormKit::fieldRow(QString::fromUtf8(label), sp);
    attachTip(row, tip);
    return row;
}

AimSettingsPage::AimSettingsPage(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    auto* left = new QWidget;
    left->setFixedWidth(190);
    buildLeftPanel(left);

    auto* right = new QWidget;
    buildRightPanel(right);

    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setSizes({190, 700});
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    root->addWidget(splitter);

    connect(&ConfigManager::instance(), &ConfigManager::configLoaded,
            this, &AimSettingsPage::reloadFromRuntime);

    rebuildGroupCombo();
    reloadFromRuntime();
}

void AimSettingsPage::setTargetPage(TargetPage* tp)
{
    m_targetPage = tp;
    if (m_targetPage)
        connect(m_targetPage, &TargetPage::classFiltersChanged,
                this, &AimSettingsPage::onTargetClassesChanged);
}

void AimSettingsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    reloadFromRuntime();
}

void AimSettingsPage::buildLeftPanel(QWidget* parent)
{
    auto* lay = new QVBoxLayout(parent);
    lay->setContentsMargins(12, 14, 6, 12);
    lay->setSpacing(8);

    auto* groupLabel = new QLabel(QStringLiteral("热键组（当前生效）"));
    groupLabel->setStyleSheet("color:#A49E90; font-size:11px; font-weight:500;");
    lay->addWidget(groupLabel);

    auto* groupRow = new QHBoxLayout;
    groupRow->setSpacing(4);
    m_groupCombo = new QComboBox;
    m_groupCombo->setMinimumHeight(30);
    groupRow->addWidget(m_groupCombo, 1);

    const QString smallBtnSS =
        "QPushButton{font-size:16px; color:#ABA697; background:transparent;"
        " border:1px solid rgba(213,181,107,0.08); border-radius:4px; padding:0;}"
        "QPushButton:hover{color:#D5B56B; border-color:#D5B56B;}";

    auto* addGroupBtn = new QPushButton(QStringLiteral("+"));
    addGroupBtn->setFixedSize(28, 28);
    addGroupBtn->setCursor(Qt::PointingHandCursor);
    addGroupBtn->setStyleSheet(smallBtnSS);
    addGroupBtn->setToolTip(QStringLiteral("新建热键组"));
    groupRow->addWidget(addGroupBtn);

    auto* delGroupBtn = new QPushButton(QStringLiteral("−"));
    delGroupBtn->setFixedSize(28, 28);
    delGroupBtn->setCursor(Qt::PointingHandCursor);
    delGroupBtn->setStyleSheet(smallBtnSS);
    delGroupBtn->setToolTip(QStringLiteral("删除当前热键组"));
    groupRow->addWidget(delGroupBtn);

    lay->addLayout(groupRow);

    connect(m_groupCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AimSettingsPage::onGroupChanged);
    connect(addGroupBtn, &QPushButton::clicked, this, &AimSettingsPage::onAddGroup);
    connect(delGroupBtn, &QPushButton::clicked, this, &AimSettingsPage::onDeleteGroup);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(4, 6, 4, 0);
    m_leftTitle = new QLabel(QStringLiteral("热键 · 拖动排序"));
    m_leftTitle->setStyleSheet("color:#A49E90; font-size:11px; font-weight:500;");
    m_leftTitle->setToolTip(QStringLiteral("相同按键使用已激活配置；不同按键同时按住时，下方优先，组合键优先。"));
    header->addWidget(m_leftTitle);
    header->addStretch();
    lay->addLayout(header);

    auto* profileList = new ReorderableHotkeyList;
    m_profileList = profileList;
    profileList->setDragDropMode(QAbstractItemView::InternalMove);
    profileList->setDefaultDropAction(Qt::MoveAction);
    profileList->setDragEnabled(true);
    profileList->setAcceptDrops(true);
    profileList->setDropIndicatorShown(true);
    profileList->moveRequested = [this](int from, int to) {
        moveProfileInGroup(from, to);
    };
    m_profileList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_profileList->setFrameShape(QFrame::NoFrame);
    m_profileList->setStyleSheet(
        "QListWidget{background:transparent; border:none; outline:none; padding:0;}"
        "QListWidget::item{padding:0; margin:0 0 5px 0; border-radius:9px; background:#19191C;"
        " border:1px solid rgba(213,181,107,0.05);}"
        "QListWidget::item:selected{background:#302A1E; border:1px solid #302A1E;}");
    lay->addWidget(m_profileList, 1);

    m_profileList->setToolTip(m_leftTitle->toolTip());

    connect(m_profileList, &QListWidget::currentRowChanged,
            this, &AimSettingsPage::onProfileSelected);
    connect(m_profileList, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        QMenu menu(this);
        if (auto* item = m_profileList->itemAt(pos)) {
            const int row = m_profileList->row(item);
            auto* copy = menu.addAction(QStringLiteral("复制热键"));
            auto* remove = menu.addAction(QStringLiteral("删除热键"));
            const QAction* action = menu.exec(m_profileList->viewport()->mapToGlobal(pos));
            if (!action) return;
            m_profileList->setCurrentRow(row);
            if (action == copy) onCopyProfile();
            else if (action == remove) onDeleteProfile();
        } else {
            auto* paste = menu.addAction(QStringLiteral("粘贴热键"));
            paste->setEnabled(static_cast<bool>(m_copiedProfile));
            auto* add = menu.addAction(QStringLiteral("新增热键"));
            const QAction* action = menu.exec(m_profileList->viewport()->mapToGlobal(pos));
            if (action == paste) onPasteProfile();
            else if (action == add) onAddProfile();
        }
    });

    auto* row = new QHBoxLayout;
    auto* addBtn = new QPushButton(QStringLiteral("+"));
    auto* delBtn = new QPushButton(QStringLiteral("−"));
    auto* cpyBtn = new QPushButton(QStringLiteral("复制"));
    row->addWidget(addBtn);
    row->addWidget(delBtn);
    row->addWidget(cpyBtn);
    lay->addLayout(row);

    connect(addBtn, &QPushButton::clicked, this, &AimSettingsPage::onAddProfile);
    connect(delBtn, &QPushButton::clicked, this, &AimSettingsPage::onDeleteProfile);
    connect(cpyBtn, &QPushButton::clicked, this, &AimSettingsPage::onCopyProfile);
}

void AimSettingsPage::buildRightPanel(QWidget* parent)
{
    auto* outer = new QVBoxLayout(parent);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget;
    m_rightLayout = new QVBoxLayout(content);
    m_rightLayout->setContentsMargins(16, 16, 16, 16);
    m_rightLayout->setSpacing(12);

    buildKeyBindCard();
    buildFovCard();
    buildAimClassCard();
    buildCrosshairCard();
    buildDynamicFovCard();
    buildRecoveredControllerCard();
    buildTriggerCard();
    buildTrajectoryCard();

    m_rightLayout->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll);
}

void AimSettingsPage::buildKeyBindCard()
{
    auto* card = new CardWidget(QStringLiteral("触发按键"), QStringLiteral("keyboard"));
    auto* cl = card->contentLayout();

    auto* combo = new QComboBox;
    for (int i = 0; kKeyEntries[i].id; ++i)
        combo->addItem(QString::fromUtf8(kKeyEntries[i].label), QString::fromUtf8(kKeyEntries[i].id));
    combo->setObjectName("keyCombo");
    cl->addWidget(FormKit::fieldRow(QStringLiteral("主按键"), combo));
    auto* chord = new QCheckBox(QStringLiteral("启用双键组合（两键同时按住才瞄准）"));
    chord->setObjectName("keyChord");
    cl->addWidget(chord);
    auto* second = new QComboBox;
    second->setObjectName("keyComboSecond");
    second->addItem(QStringLiteral("请选择第二个按键"), QString());
    for (int i = 1; kKeyEntries[i].id; ++i)
        second->addItem(QString::fromUtf8(kKeyEntries[i].label), QString::fromUtf8(kKeyEntries[i].id));
    for (const auto& key : macros::keys()) {
        const QString id = QString::fromStdString(key.id);
        second->addItem(QString::fromStdString(key.label), id);
        combo->addItem(QString::fromStdString(key.label), id);
    }
    cl->addWidget(FormKit::fieldRow(QStringLiteral("组合第二键"), second));
    second->setEnabled(chord->isChecked());
    auto* blockHotkey = new QCheckBox(QStringLiteral("屏蔽热键（仍触发瞄准，不传递原始按键）"));
    blockHotkey->setObjectName("blockAimHotkey");
    cl->addWidget(blockHotkey);
    blockHotkey->setToolTip(QStringLiteral("按当前热键组生效，已接入所有输入方式。硬件屏蔽被控端的鼠标热键，Windows 原生屏蔽本机；MAKCU 自定义旧固件需更新。修改后请松开再按。"));
    auto* blockStatus = makeHint(QString::fromUtf8(hotkey_blocking::status().c_str()));
    cl->addWidget(blockStatus);
    auto* blockTimer = new QTimer(card);
    connect(blockTimer, &QTimer::timeout, card, [blockStatus] {
        if (blockStatus->isVisible()) blockStatus->setText(QString::fromUtf8(hotkey_blocking::status().c_str()));
    });
    blockTimer->start(500);
    connect(blockHotkey, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        {
            std::lock_guard<std::recursive_mutex> lock(configMutex);
            if (ri < 0 || ri >= static_cast<int>(config.hotkeys.size())) return;
            config.hotkeys[ri].block_hotkey = checked;
        }
        ConfigBridge::instance().markDirty();
    });

    m_activationWidget = new HotkeyActivationWidget(card);
    cl->addWidget(m_activationWidget);
    connect(m_activationWidget, &HotkeyActivationWidget::activateRequested, this, [this] {
        const int index = currentRuntimeIndex();
        {
            std::lock_guard<std::recursive_mutex> lock(configMutex);
            if (!activateHotkey(config.hotkeys, index)) return;
        }
        ConfigBridge::instance().markDirty();
        refreshActivation();
    });
    connect(m_activationWidget, &HotkeyActivationWidget::activationKeyChanged, this, [this](const QString& key) {
        if (m_loading) return;
        const int index = currentRuntimeIndex();
        {
            std::lock_guard<std::recursive_mutex> lock(configMutex);
            if (index < 0 || index >= static_cast<int>(config.hotkeys.size())) return;
            config.hotkeys[index].activation_key = key.toStdString();
        }
        ConfigBridge::instance().markDirty();
        refreshActivation();
    });
    auto* activationTimer = new QTimer(card);
    connect(activationTimer, &QTimer::timeout, this, [this] {
        if (isVisible()) refreshActivation();
    });
    activationTimer->start(200);

    auto commitKeys = [this, combo, second, chord] {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            const QString id = combo->currentData().toString();
            const QString secondId = second->currentData().toString();
            const bool validChord = chord->isChecked() && !id.isEmpty() &&
                !secondId.isEmpty() && id != secondId;
            config.hotkeys[ri].keys.clear();
            if (!id.isEmpty())
                config.hotkeys[ri].keys.push_back(id.toStdString());
            if (validChord) config.hotkeys[ri].keys.push_back(secondId.toStdString());
            config.hotkeys[ri].keys_chord = chord->isChecked();

        }
        ConfigBridge::instance().markDirty();
        rebuildProfileList();
    };
    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [commitKeys](int) { commitKeys(); });
    connect(second, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [commitKeys](int) { commitKeys(); });
    connect(chord, &QCheckBox::toggled, this, [second, commitKeys](bool enabled) {
        second->setEnabled(enabled);
        commitKeys();
    });

    m_rightLayout->addWidget(card);
}

void AimSettingsPage::buildFovCard()
{
    auto* card = new CardWidget(QStringLiteral("视野 FOV"), QStringLiteral("target"));
    auto* cl = card->contentLayout();

    auto* fx = new NoWheelSpinBox; fx->setRange(1, 4096); fx->setObjectName("fovX");
    auto* fy = new NoWheelSpinBox; fy->setRange(1, 4096); fy->setObjectName("fovY");
    cl->addWidget(FormKit::fieldRow(QStringLiteral("水平直径 (检测像素)"), fx));
    cl->addWidget(FormKit::fieldRow(QStringLiteral("垂直直径 (检测像素)"), fy));
    auto* maskX = new QCheckBox(QStringLiteral("屏蔽 X 轴（屏蔽真实横向输入）"));
    auto* maskY = new QCheckBox(QStringLiteral("屏蔽 Y 轴（屏蔽真实纵向输入）"));
    maskX->setObjectName("maskX");
    maskY->setObjectName("maskY");
    cl->addWidget(maskX);
    cl->addWidget(maskY);
    auto* aimDelay = new NoWheelSpinBox;
    aimDelay->setRange(0, 2000);
    aimDelay->setSuffix(QStringLiteral(" ms"));
    aimDelay->setObjectName("aimDelayMs");
    cl->addWidget(FormKit::fieldRow(QStringLiteral("延迟瞄准（识别到目标且按住热键后）"), aimDelay));
    auto* unlockX = new QCheckBox(QStringLiteral("解锁 X 轴（程序不横向瞄准）"));
    auto* unlockY = new QCheckBox(QStringLiteral("解锁 Y 轴（程序不纵向瞄准）"));
    unlockX->setObjectName("unlockX");
    unlockY->setObjectName("unlockY");
    cl->addWidget(unlockX);
    cl->addWidget(unlockY);
    auto* unlockYDelay = new NoWheelSpinBox;
    unlockYDelay->setRange(0, 5000);
    unlockYDelay->setSuffix(QStringLiteral(" ms"));
    unlockYDelay->setObjectName("unlockYDelayMs");
    cl->addWidget(FormKit::fieldRow(QStringLiteral("解锁 Y 延迟（目标与热键同时生效后）"), unlockYDelay));
    const QString axisHelp = QStringLiteral("屏蔽轴：有有效目标并开始瞄准时才拦截真实鼠标输入；丢失目标、松键或等待延迟期间解除。解锁轴：程序不控制该轴。延迟设为 0 即立即瞄准。");
    unlockX->setToolTip(axisHelp);
    unlockY->setToolTip(axisHelp);
    unlockYDelay->setToolTip(axisHelp);
    auto* axisStatus = makeHint(QString());
    cl->addWidget(axisStatus);
    auto* axisTimer = new QTimer(card);
    connect(axisTimer, &QTimer::timeout, axisStatus, [axisStatus] {
        axisStatus->setText(QString::fromUtf8(hotkey_blocking::axisStatus().c_str()));
    });
    axisTimer->start(300);

    auto commit = [this]() {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        config.hotkeys[ri].fovX = findChild<QSpinBox*>("fovX")->value();
        config.hotkeys[ri].fovY = findChild<QSpinBox*>("fovY")->value();
        config.hotkeys[ri].mask_x = findChild<QCheckBox*>("maskX")->isChecked();
        config.hotkeys[ri].mask_y = findChild<QCheckBox*>("maskY")->isChecked();
        config.hotkeys[ri].unlock_x = findChild<QCheckBox*>("unlockX")->isChecked();
        config.hotkeys[ri].unlock_y = findChild<QCheckBox*>("unlockY")->isChecked();
        config.hotkeys[ri].unlock_y_delay_ms = findChild<QSpinBox*>("unlockYDelayMs")->value();
        config.hotkeys[ri].aim_delay_ms = findChild<QSpinBox*>("aimDelayMs")->value();
        ConfigBridge::instance().markDirty();
    };
    connect(fx, QOverload<int>::of(&QSpinBox::valueChanged), this, [commit](int) { commit(); });
    connect(fy, QOverload<int>::of(&QSpinBox::valueChanged), this, [commit](int) { commit(); });
    connect(maskX, &QCheckBox::toggled, this, [commit](bool) { commit(); });
    connect(maskY, &QCheckBox::toggled, this, [commit](bool) { commit(); });
    connect(unlockX, &QCheckBox::toggled, this, [commit](bool) { commit(); });
    connect(unlockY, &QCheckBox::toggled, this, [commit](bool) { commit(); });
    connect(unlockYDelay, QOverload<int>::of(&QSpinBox::valueChanged), this, [commit](int) { commit(); });
    connect(aimDelay, QOverload<int>::of(&QSpinBox::valueChanged), this, [commit](int) { commit(); });

    m_rightLayout->addWidget(card);
}

void AimSettingsPage::buildAimClassCard()
{
    auto* card = new CardWidget(QStringLiteral("瞄准类别 (优先级排序)"), QStringLiteral("target"));
    auto* cl = card->contentLayout();
    card->setToolTip(QString::fromUtf8(
        u8"从「目标类别」页勾选「瞄准」的类别会出现在下方。"
        u8"用 ▲ ▼ 调整优先级（顶部 = 最高），✕ 移除。"));

    m_aimClassContainer = new QWidget;
    m_aimClassLayout = new QVBoxLayout(m_aimClassContainer);
    m_aimClassLayout->setContentsMargins(0, 0, 0, 0);
    m_aimClassLayout->setSpacing(8);
    cl->addWidget(m_aimClassContainer);

    auto* addRow = new QHBoxLayout;
    addRow->setSpacing(6);
    m_addClassCombo = new QComboBox;
    m_addClassCombo->setMinimumWidth(120);
    m_addClassCombo->setMinimumHeight(30);
    addRow->addWidget(m_addClassCombo, 1);

    m_addClassBtn = new QPushButton(QString::fromUtf8(u8"+ 添加"));
    m_addClassBtn->setFixedHeight(30);
    m_addClassBtn->setCursor(Qt::PointingHandCursor);
    addRow->addWidget(m_addClassBtn);
    cl->addLayout(addRow);

    connect(m_addClassBtn, &QPushButton::clicked, this, [this] {
        const int ri = currentRuntimeIndex();
        if (ri < 0 || m_addClassCombo->currentIndex() < 0) return;
        const int cid = m_addClassCombo->currentData().toInt();
        if (cid < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            auto& acs = config.hotkeys[ri].aim_classes;
            for (const auto& a : acs)
                if (a.class_id == cid) return;
            HotkeyAimClass a;
            a.class_id = cid;
            a.y_offset = 0.65f;
            a.y_offset_max = 0.65f;
            a.min_conf = static_cast<float>(config.confidence_threshold);
            acs.push_back(a);
        }
        ConfigBridge::instance().markDirty();
        rebuildAimClassRows();
    });

    m_rightLayout->addWidget(card);
    rebuildAimClassRows();
}

void AimSettingsPage::rebuildAddClassCombo()
{
    if (!m_addClassCombo) return;
    m_addClassCombo->clear();
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    for (const auto& cf : config.class_filters)
    {
        const QString nm = cf.class_name.empty()
            ? QStringLiteral("class_%1").arg(cf.class_id)
            : QString::fromUtf8(cf.class_name.c_str());
        m_addClassCombo->addItem(QStringLiteral("[%1] %2").arg(cf.class_id).arg(nm), cf.class_id);
    }
}

void AimSettingsPage::moveAimClass(int from, int to)
{
    const int ri = currentRuntimeIndex();
    if (ri < 0) return;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        auto& ac = config.hotkeys[ri].aim_classes;
        const int n = static_cast<int>(ac.size());
        if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
        std::swap(ac[from], ac[to]);
    }
    ConfigBridge::instance().markDirty();
    rebuildAimClassRows();
}

void AimSettingsPage::rebuildAimClassRows()
{
    if (!m_aimClassLayout) return;

    while (QLayoutItem* it = m_aimClassLayout->takeAt(0))
    {
        if (QWidget* w = it->widget()) w->deleteLater();
        delete it;
    }

    struct Row { int cid; float yMin; float yMax; float xMin; float xMax; float c; QString name; };
    std::vector<Row> rows;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        const int ri = currentRuntimeIndex();
        if (ri >= 0 && ri < static_cast<int>(config.hotkeys.size()))
        {
            for (const auto& ac : config.hotkeys[ri].aim_classes)
            {
                QString name = QStringLiteral("class_%1").arg(ac.class_id);
                for (const auto& cf : config.class_filters)
                    if (cf.class_id == ac.class_id && !cf.class_name.empty())
                    { name = QString::fromUtf8(cf.class_name.c_str()); break; }
                rows.push_back({ ac.class_id, ac.y_offset, ac.y_offset_max,
                                 ac.x_offset, ac.x_offset_max, ac.min_conf, name });
            }
        }
    }

    if (rows.empty())
    {
        auto* empty = new QLabel(QString::fromUtf8(
            u8"（无瞄准类别 — 先在「目标类别」页把类别切到「瞄准」）"));
        empty->setProperty("class", "hint");
        empty->setWordWrap(true);
        empty->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        empty->setMinimumHeight(36);
        m_aimClassLayout->addWidget(empty);
        rebuildAddClassCombo();
        return;
    }

    auto confText = [](int raw) {
        return raw <= 0 ? QString::fromUtf8(u8"全局")
                        : QString::number(raw / 100.0, 'f', 2);
    };

    const int total = static_cast<int>(rows.size());
    for (int idx = 0; idx < total; ++idx)
    {
        const Row r = rows[idx];
        const int classId = r.cid;

        auto* rowFrame = new QFrame;
        rowFrame->setObjectName("aimRow");
        rowFrame->setStyleSheet(
            "QFrame#aimRow{background:#202023; border:1px solid rgba(213,181,107,0.06);"
            " border-radius:8px;}");
        auto* rl = new QVBoxLayout(rowFrame);
        rl->setContentsMargins(12, 8, 10, 10);
        rl->setSpacing(8);

        auto* top = new QHBoxLayout;
        top->setSpacing(8);

        auto* priLabel = new QLabel(QStringLiteral("#%1").arg(idx + 1));
        priLabel->setFixedWidth(30);
        priLabel->setStyleSheet("color:#D5B56B; font-size:13px; font-weight:600; border:none;");
        top->addWidget(priLabel);

        auto* nameLabel = new QLabel(QStringLiteral("[%1] %2").arg(r.cid).arg(r.name));
        nameLabel->setStyleSheet("color:#DCD7CA; font-size:13px; font-weight:500; border:none;");
        top->addWidget(nameLabel, 1);

        auto makeIconBtn = [](const QString& glyph, const QString& color,
                              const QString& hover, const QString& tip) {
            auto* b = new QPushButton(glyph);
            b->setFixedSize(26, 26);
            b->setCursor(Qt::PointingHandCursor);
            b->setToolTip(tip);
            b->setStyleSheet(QStringLiteral(
                "QPushButton{color:%1; background:transparent;"
                " border:1px solid rgba(213,181,107,0.08); border-radius:6px;"
                " font-size:13px; padding:0;}"
                "QPushButton:hover{color:%2; border-color:%2;}"
                "QPushButton:disabled{color:#514C40; border-color:rgba(213,181,107,0.05);}")
                .arg(color, hover));
            return b;
        };

        auto* upBtn = makeIconBtn(QString::fromUtf8(u8"▲"), QStringLiteral("#ABA697"),
                                  QStringLiteral("#D5B56B"),
                                  QString::fromUtf8(u8"上移（提高优先级）"));
        auto* downBtn = makeIconBtn(QString::fromUtf8(u8"▼"), QStringLiteral("#ABA697"),
                                    QStringLiteral("#D5B56B"),
                                    QString::fromUtf8(u8"下移（降低优先级）"));
        auto* delBtn = makeIconBtn(QString::fromUtf8(u8"✕"), QStringLiteral("#D25A5A"),
                                   QStringLiteral("#B83232"), QString::fromUtf8(u8"移除"));
        upBtn->setEnabled(idx > 0);
        downBtn->setEnabled(idx < total - 1);
        top->addWidget(upBtn);
        top->addWidget(downBtn);
        top->addWidget(delBtn);
        rl->addLayout(top);

        auto makeOffsetSpin = [](float value) {
            auto* sp = new NoWheelDoubleSpinBox;
            sp->setRange(0.0, 1.0);
            sp->setSingleStep(0.01);
            sp->setDecimals(2);
            sp->setValue(value);
            sp->setMinimumHeight(28);
            sp->setMinimumWidth(76);
            return sp;
        };

        auto* rangeRow = new QHBoxLayout;
        rangeRow->setSpacing(8);
        auto* yLbl = new QLabel(QString::fromUtf8(u8"随机锁点 Y"));
        yLbl->setStyleSheet("color:#ABA697; font-size:12px; border:none;");
        auto* yMinSpin = makeOffsetSpin(r.yMin);
        auto* yMaxSpin = makeOffsetSpin(r.yMax);
        yMinSpin->setToolTip(QString::fromUtf8(
            u8"范围下限：1=框顶，0.5=中心，0=框底。\n"
            u8"★ 该类的值会覆盖「控制器」卡里的热键级瞄点 Y（只在设了该类时）。"));
        yMaxSpin->setToolTip(QString::fromUtf8(
            u8"范围上限：每次新锁定在上下限之间随机一次，同一目标持续锁定时保持该比例。\n"
            u8"★ 等于下限时不随机（固定打同一个点）。"));
        rangeRow->addWidget(yLbl);
        rangeRow->addWidget(yMinSpin);
        rangeRow->addWidget(new QLabel(QString::fromUtf8(u8"—")));
        rangeRow->addWidget(yMaxSpin);
        rangeRow->addStretch();
        rl->addLayout(rangeRow);

        auto* xRangeRow = new QHBoxLayout;
        xRangeRow->setSpacing(8);
        auto* xLbl = new QLabel(QString::fromUtf8(u8"随机锁点 X"));
        xLbl->setStyleSheet("color:#ABA697; font-size:12px; border:none;");
        auto* xMinSpin = makeOffsetSpin(r.xMin);
        auto* xMaxSpin = makeOffsetSpin(r.xMax);
        xMinSpin->setToolTip(QString::fromUtf8(
            u8"范围下限：0=框左，0.5=中心，1=框右。"));
        xMaxSpin->setToolTip(QString::fromUtf8(
            u8"范围上限：等于下限时固定；不同则在每次新锁定时随机一次，同一目标持续锁定时保持该比例。"));
        xRangeRow->addWidget(xLbl);
        xRangeRow->addWidget(xMinSpin);
        xRangeRow->addWidget(new QLabel(QString::fromUtf8(u8"—")));
        xRangeRow->addWidget(xMaxSpin);
        xRangeRow->addStretch();
        rl->addLayout(xRangeRow);

        auto* cSlider = new QSlider(Qt::Horizontal);
        cSlider->setRange(0, 100);
        cSlider->setSingleStep(1);
        cSlider->setPageStep(5);
        cSlider->setValue(std::clamp(static_cast<int>(std::lround(r.c * 100.0f)), 0, 100));
        cSlider->setMinimumWidth(80);
        cSlider->setToolTip(QString::fromUtf8(
            u8"最低置信度：低于此值的框不会夺锁。0 = 跟随 AI 页全局阈值。\n"
            u8"★ 与全局阈值是「都要过」的关系 —— 这里是额外收紧，不替代它。"));

        auto* bottom = new QHBoxLayout;
        bottom->setSpacing(10);
        auto* cLbl = new QLabel(QString::fromUtf8(u8"置信"));
        cLbl->setFixedWidth(32);
        cLbl->setStyleSheet("color:#ABA697; font-size:12px; border:none;");
        auto* cVal = new QLabel(confText(cSlider->value()));
        cVal->setFixedWidth(38);
        cVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        cVal->setStyleSheet("color:#DCD7CA; font-size:12px; border:none;");
        bottom->addWidget(cLbl);
        bottom->addWidget(cSlider, 1);
        bottom->addWidget(cVal);
        rl->addLayout(bottom);

        m_aimClassLayout->addWidget(rowFrame);

        auto persistRange = [this, classId, yMinSpin, yMaxSpin, xMinSpin, xMaxSpin]
                            (bool xAxis, bool minChanged) {
            if (m_loading) return;
            auto* lo = xAxis ? xMinSpin : yMinSpin;
            auto* hi = xAxis ? xMaxSpin : yMaxSpin;
            if (minChanged && lo->value() > hi->value())
                hi->setValue(lo->value());
            else if (!minChanged && hi->value() < lo->value())
                lo->setValue(hi->value());

            const int ri2 = currentRuntimeIndex();
            if (ri2 < 0) return;
            {
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                if (ri2 >= static_cast<int>(config.hotkeys.size())) return;
                for (auto& a : config.hotkeys[ri2].aim_classes)
                    if (a.class_id == classId)
                    {
                        a.y_offset = static_cast<float>(yMinSpin->value());
                        a.y_offset_max = static_cast<float>(yMaxSpin->value());
                        a.x_offset = static_cast<float>(xMinSpin->value());
                        a.x_offset_max = static_cast<float>(xMaxSpin->value());
                        break;
                    }
            }
            ConfigBridge::instance().markDirty();
        };
        connect(yMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [persistRange](double) { persistRange(false, true); });
        connect(yMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [persistRange](double) { persistRange(false, false); });
        connect(xMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [persistRange](double) { persistRange(true, true); });
        connect(xMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [persistRange](double) { persistRange(true, false); });

        connect(cSlider, &QSlider::valueChanged, this,
                [this, classId, cVal, confText](int raw) {
            cVal->setText(confText(raw));
            if (m_loading) return;
            const float v = static_cast<float>(raw) / 100.0f;
            const int ri2 = currentRuntimeIndex();
            if (ri2 < 0) return;
            {
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                if (ri2 >= static_cast<int>(config.hotkeys.size())) return;
                for (auto& a : config.hotkeys[ri2].aim_classes)
                    if (a.class_id == classId) { a.min_conf = v; break; }
            }
            ConfigBridge::instance().markDirty();
        });

        connect(upBtn, &QPushButton::clicked, this,
                [this, idx] { moveAimClass(idx, idx - 1); });
        connect(downBtn, &QPushButton::clicked, this,
                [this, idx] { moveAimClass(idx, idx + 1); });
        connect(delBtn, &QPushButton::clicked, this, [this, classId] {
            const int ri2 = currentRuntimeIndex();
            if (ri2 < 0) return;
            {
                std::lock_guard<std::recursive_mutex> lk(configMutex);
                if (ri2 >= static_cast<int>(config.hotkeys.size())) return;
                auto& ac2 = config.hotkeys[ri2].aim_classes;
                ac2.erase(std::remove_if(ac2.begin(), ac2.end(),
                    [classId](const HotkeyAimClass& a) { return a.class_id == classId; }),
                    ac2.end());
            }
            ConfigBridge::instance().markDirty();
            rebuildAimClassRows();
        });
    }

    rebuildAddClassCombo();
}

void AimSettingsPage::buildCrosshairCard()
{
    auto* card = new CardWidget(QStringLiteral("瞄准方式"), QStringLiteral("crosshair"));
    auto* cl = card->contentLayout();
    auto* mode = new QComboBox;
    mode->setObjectName("aimMode");
    mode->addItem(QStringLiteral("画面中心"));
    mode->addItem(QStringLiteral("瞄点压枪"));
    mode->addItem(QStringLiteral("镭射找色"));
    mode->addItem(QStringLiteral("准星找色"));
    cl->addWidget(FormKit::fieldRow(QStringLiteral("当前热键使用"), mode));
    mode->setToolTip(QStringLiteral(
        "为当前已有热键选择瞄准方式。三种功能互斥。瞄点压枪在热键与开火键同时按住时逐渐下移目标瞄点；"
        "找色模式使用检测到的准星或镭射位置，失效时退回画面中心。"
        "速度与最大偏移在左侧「瞄点压枪」页设置。"));

    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            auto& hk = config.hotkeys[ri];
            hk.aimpoint_recoil_enabled = index == 1;
            hk.laser_detect_enabled = index == 2;
            hk.crosshair_detect_enabled = index == 3;
        }
        ConfigBridge::instance().markDirty();
    });

    m_rightLayout->addWidget(card);
}

void AimSettingsPage::buildDynamicFovCard()
{
    auto* card = new CardWidget(QStringLiteral("动态 FOV"), QStringLiteral("target"));
    auto* cl = card->contentLayout();

    auto* chk = new QCheckBox(QStringLiteral("启用（锁定后收紧瞄准区域，防止别的目标抢锁）"));
    chk->setObjectName("dynFovChk");
    cl->addWidget(chk);

    auto* spin = new NoWheelSpinBox;
    spin->setRange(1, 4096);
    spin->setSingleStep(5);
    spin->setSuffix(QStringLiteral(" px"));
    spin->setObjectName("dynFovSize");
    spin->setToolTip(QStringLiteral("追近后的 FOV 直径，单位为检测像素。每轴不超过原 FOV；尚未追近目标时会临时保留额外空间，避免卡圈边。"));
    cl->addWidget(FormKit::fieldRow(QStringLiteral("缩小后的 FOV 大小（直径）"), spin));
    auto* shrink = new NoWheelSpinBox;
    shrink->setObjectName("dynFovShrinkMs");
    shrink->setRange(0, 2000);
    shrink->setSingleStep(25);
    shrink->setSuffix(QStringLiteral(" ms"));
    shrink->setToolTip(QStringLiteral("数值越小缩小越快，0 为立即缩小。目标距离固定时，约在此时间内完成 95% 的缩小；尚未追近时会保留空间。"));
    cl->addWidget(FormKit::fieldRow(QStringLiteral("缩小时间"), shrink));
    auto* expand = new NoWheelSpinBox;
    expand->setObjectName("dynFovExpandMs");
    expand->setRange(0, 2000);
    expand->setSingleStep(25);
    expand->setSuffix(QStringLiteral(" ms"));
    expand->setToolTip(QStringLiteral("目标拉远或丢失后放大 FOV 的时间，约完成 95% 的放大；0 为立即放大。松开瞄准键或切换配置时直接恢复原 FOV。"));
    cl->addWidget(FormKit::fieldRow(QStringLiteral("放大时间"), expand));
    spin->setEnabled(chk->isChecked());
    shrink->setEnabled(chk->isChecked());
    expand->setEnabled(chk->isChecked());
    connect(chk, &QCheckBox::toggled, spin, &QWidget::setEnabled);
    connect(chk, &QCheckBox::toggled, shrink, &QWidget::setEnabled);
    connect(chk, &QCheckBox::toggled, expand, &QWidget::setEnabled);
    connect(expand, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lock(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            config.hotkeys[ri].dynamic_fov_expand_ms = value;
        }
        ConfigBridge::instance().markDirty();
    });
    connect(shrink, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lock(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            config.hotkeys[ri].dynamic_fov_shrink_ms = value;
        }
        ConfigBridge::instance().markDirty();
    });

    connect(chk, &QCheckBox::toggled, this, [this](bool v) {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            config.hotkeys[ri].dynamic_fov_enabled = v;
        }
        ConfigBridge::instance().markDirty();
    });
    connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            config.hotkeys[ri].dynamic_fov_size = v;
        }
        ConfigBridge::instance().markDirty();
    });

    m_rightLayout->addWidget(card);
}

void AimSettingsPage::buildRecoveredControllerCard()
{
    auto* card = new CardWidget(QString::fromUtf8(u8"瞄准控制器"),
                                QStringLiteral("adjustments"));
    auto* layout = card->contentLayout();
    auto* enabled = new QCheckBox(QString::fromUtf8(u8"启用瞄准移动"));
    enabled->setObjectName("ctlEnabled");
    layout->addWidget(enabled);
    auto* advancedButton = new QPushButton(QString::fromUtf8(u8"开镜独立参数…"));
    advancedButton->setCursor(Qt::PointingHandCursor);
    layout->addWidget(advancedButton);
    auto* dialog = new QDialog(this);
    dialog->setWindowTitle(QString::fromUtf8(u8"瞄准控制器 · 开镜独立参数"));
    dialog->resize(760, 650);
    auto* dialogRoot = new QVBoxLayout(dialog);
    auto* scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    auto* dialogContent = new QWidget(scroll);
    auto* extraLayout = new QVBoxLayout(dialogContent);
    extraLayout->setSpacing(12);
    scroll->setWidget(dialogContent);
    dialogRoot->addWidget(scroll);
    auto* closeButton = new QPushButton(QString::fromUtf8(u8"完成"), dialog);
    dialogRoot->addWidget(closeButton, 0, Qt::AlignRight);
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::accept);
    connect(advancedButton, &QPushButton::clicked, dialog, [dialog] { dialog->open(); });

    auto* scopeCtlCombo = new QComboBox(dialogContent);
    scopeCtlCombo->setObjectName("scopeCtlMode");
    scopeCtlCombo->addItem(QString::fromUtf8(u8"关闭：跟随当前参数"), 0);
    scopeCtlCombo->addItem(QString::fromUtf8(u8"自动开镜时使用独立参数"), 1);
    extraLayout->addWidget(FormKit::fieldRow(QString::fromUtf8(u8"开镜独立参数"), scopeCtlCombo));

    struct Row { const char* suffix; const char* label; double minimum; double maximum; double step; double value; };
    const Row xRows[] = {
        { "KpX", "比例 Kp", 0.0, 10.0, 0.01, 0.4 },
        { "KiX", "积分 Ki", 0.0, 10.0, 0.01, 0.02 },
        { "KdX", "微分 Kd", 0.0, 10.0, 0.01, 0.12 },
        { "FfX", "速度前馈 FF", 0.0, 10.0, 0.0005, 0.0 },
        { "DeadzoneX", "死区半径", 0.0, 200.0, 0.5, 5.0 },
        { "HardDeadzoneX", "XY 轴死区", 0.0, 200.0, 0.5, 0.0 },
        { "FollowX", "跟随补偿", 0.0, 50.0, 0.1, 0.0 },
    };
    const Row yRows[] = {
        { "KpY", "比例 Kp", 0.0, 10.0, 0.01, 0.4 },
        { "KiY", "积分 Ki", 0.0, 10.0, 0.01, 0.02 },
        { "KdY", "微分 Kd", 0.0, 10.0, 0.01, 0.12 },
        { "FfY", "速度前馈 FF", 0.0, 10.0, 0.0005, 0.0 },
        { "DeadzoneY", "死区半径", 0.0, 200.0, 0.5, 5.0 },
        { "HardDeadzoneY", "XY 轴死区", 0.0, 200.0, 0.5, 0.0 },
        { "FollowY", "跟随补偿", 0.0, 50.0, 0.1, 0.0 },
    };

    auto addSet = [&](QVBoxLayout* targetLayout, const QString& prefix, const QString& title) {
        targetLayout->addWidget(makeSectionTitle(title));
        auto* grid = new QGridLayout;
        grid->setHorizontalSpacing(16);
        grid->setVerticalSpacing(8);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        grid->addWidget(makeSectionTitle(QString::fromUtf8(u8"横向 X")), 0, 0);
        grid->addWidget(makeSectionTitle(QString::fromUtf8(u8"纵向 Y")), 0, 1);

        auto addNumeric = [&](const Row& row, int line, int column) {
            auto* spin = new NoWheelDoubleSpinBox;
            spin->setObjectName(prefix + QString::fromLatin1(row.suffix));
            spin->setRange(row.minimum, row.maximum);
            spin->setSingleStep(row.step);
            const QString suffix = QString::fromLatin1(row.suffix);
            spin->setDecimals(suffix == QStringLiteral("FfX") ||
                              suffix == QStringLiteral("FfY") ? 4 : 3);
            spin->setValue(row.value);
            if (suffix == QStringLiteral("KiX") || suffix == QStringLiteral("KiY"))
                spin->setToolTip(QString::fromUtf8(u8"积累持续瞄准误差，积分上限等于 Ki。开启准星找色时保留反向旧积分。二次移植版在该轴 Ki 数值变化时清空该轴旧积分。"));
            if (suffix == QStringLiteral("HardDeadzoneX") || suffix == QStringLiteral("HardDeadzoneY"))
                spin->setToolTip(QString::fromUtf8(u8"额外的自由死区：误差落在这个像素范围内时，该轴完全不输出，准星可以在里面随意晃动；X、Y 各自独立判断，0 关闭。与上面的“死区半径”互不影响（死区半径只是把输出按比例减弱）。自动扳机的死区绕过规则不会绕过它。"));
            if (suffix == QStringLiteral("FollowX") || suffix == QStringLiteral("FollowY"))
                spin->setToolTip(QString::fromUtf8(u8"仅根据原瞄点与准星的持续误差积累补偿，不使用鼠标换算比例。越过原瞄点保留补偿并逐步调整；利用背景移动判断目标变向，确认后立即清空该轴旧补偿。背景不可靠时不触发变向清空。0 关闭，数值越大建立越快，过大仍可能过冲。"));
            if (suffix == QStringLiteral("FfX") || suffix == QStringLiteral("FfY"))
                spin->setToolTip(QString::fromUtf8(u8"二次移植版速度前馈：检测框画面速度与成功发送的鼠标运动事件合成，再经逐轴速度门控；0 关闭。"));
            grid->addWidget(FormKit::fieldRow(QString::fromUtf8(row.label), spin), line, column);
        };
        for (int i = 0; i < static_cast<int>(std::size(xRows)); ++i)
        {
            addNumeric(xRows[i], i + 1, 0);
            addNumeric(yRows[i], i + 1, 1);
        }

        grid->addWidget(makeSectionTitle(QString::fromUtf8(u8"功能与输出")), 8, 0, 1, 2);
        addNumeric({ "MaxPixel", "单帧限幅", 0.0, 1000.0, 1.0, 50.0 }, 9, 0);
        addNumeric({ "Segment", "分段数", 1.0, 10.0, 0.1, 3.0 }, 9, 1);
        auto* check = new QCheckBox(QString::fromUtf8(u8"启用自定义分段数（关闭时除以 3）"));
        check->setObjectName(prefix + "SegmentEnabled");
        grid->addWidget(check, 10, 0, 1, 2);
        targetLayout->addLayout(grid);
    };
    addSet(layout, "recovered", QString::fromUtf8(u8"默认参数"));
    addSet(extraLayout, "recoveredScope", QString::fromUtf8(u8"开镜独立参数"));

    auto commit = [this, enabled, scopeCtlCombo]() {
        if (m_loading) return;
        const int index = currentRuntimeIndex();
        if (index < 0) return;
        auto read = [this](const QString& name) -> float {
            auto* spin = findChild<QDoubleSpinBox*>(name);
            return spin ? static_cast<float>(spin->value()) : 0.0f;
        };
        auto collect = [&](const QString& prefix) {
            control::RecoveredPidConfig pid;
            pid.kpX = read(prefix + "KpX"); pid.kiX = read(prefix + "KiX");
            pid.kdX = read(prefix + "KdX"); pid.feedforwardX = read(prefix + "FfX");
            pid.deadzoneX = read(prefix + "DeadzoneX");
            pid.hardDeadzoneX = read(prefix + "HardDeadzoneX");
            pid.kpY = read(prefix + "KpY"); pid.kiY = read(prefix + "KiY");
            pid.kdY = read(prefix + "KdY"); pid.feedforwardY = read(prefix + "FfY");
            pid.deadzoneY = read(prefix + "DeadzoneY");
            pid.hardDeadzoneY = read(prefix + "HardDeadzoneY");
            pid.smoothMaxPixel = read(prefix + "MaxPixel");
            pid.followX = read(prefix + "FollowX");
            pid.followY = read(prefix + "FollowY");
            pid.segment = read(prefix + "Segment");
            if (auto* check = findChild<QCheckBox*>(prefix + "SegmentEnabled"))
                pid.segmentEnabled = check->isChecked();
            return pid;
        };
        {
            std::lock_guard<std::recursive_mutex> lock(configMutex);
            if (index >= static_cast<int>(config.hotkeys.size())) return;
            auto& hotkey = config.hotkeys[index];
            hotkey.ctl_enabled = enabled->isChecked();
            hotkey.recovered_pid = collect("recovered");
            hotkey.recovered_scope_pid = collect("recoveredScope");
            hotkey.scope_ctl_enabled = scopeCtlCombo->currentData().toInt();
        }
        ConfigBridge::instance().markDirty();
    };
    connect(enabled, &QCheckBox::toggled, this, [commit](bool) { commit(); });
    for (auto* spin : card->findChildren<QDoubleSpinBox*>())
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [commit](double) { commit(); });
    for (auto* check : card->findChildren<QCheckBox*>())
        if (check != enabled)
            connect(check, &QCheckBox::toggled, this, [commit](bool) { commit(); });
    for (auto* spin : dialog->findChildren<QDoubleSpinBox*>())
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [commit](double) { commit(); });
    for (auto* check : dialog->findChildren<QCheckBox*>())
    {
        connect(check, &QCheckBox::toggled, this, [commit](bool) { commit(); });
    }
    for (auto* combo : card->findChildren<QComboBox*>())
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [commit](int) { commit(); });
    for (auto* combo : dialog->findChildren<QComboBox*>())
        if (combo != scopeCtlCombo)
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [commit](int) { commit(); });
    connect(scopeCtlCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [commit](int) { commit(); });
    m_rightLayout->addWidget(card);
}

void AimSettingsPage::buildTriggerCard()
{
    auto* card = new CardWidget(QString::fromUtf8(u8"自动扳机流程"),
                                QStringLiteral("crosshair"));
    m_triggerWorkflow = new TriggerWorkflowEditor(card);
    card->contentLayout()->addWidget(m_triggerWorkflow);
    m_triggerWorkflow->setChanged([this]() {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        m_triggerWorkflow->save(config.hotkeys[ri]);
        ConfigBridge::instance().markDirty();
    });
    m_rightLayout->addWidget(card);

    auto* targetsCard = new CardWidget(QString::fromUtf8(u8"扳机类别、瞄点与范围"),
                                       QStringLiteral("target"));
    m_triggerTargetEditor = new TriggerTargetEditor(targetsCard);
    targetsCard->contentLayout()->addWidget(m_triggerTargetEditor);
    m_triggerTargetEditor->setChanged([this]() {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            m_triggerTargetEditor->save(config.hotkeys[ri]);
            if (m_triggerWorkflow) m_triggerWorkflow->load(config.hotkeys[ri]);
        }
        ConfigBridge::instance().markDirty();
    });
    m_rightLayout->addWidget(targetsCard);
}


void AimSettingsPage::buildTrajectoryCard()
{
    auto* card = new CardWidget(QString::fromUtf8(u8"轨迹曲线"),
                                QStringLiteral("vector-spline"));
    auto* cl = card->contentLayout();
    card->setToolTip(QString::fromUtf8(
        u8"轨迹只有在【瞄准控制器开启】时才有意义 —— 它整形的是控制器算出来的位移。\n"
        u8"曲线模式只旋转不缩放：每拍走多远仍由 PID 决定，曲线只决定往哪个方向走。"));

    auto* modeCombo = new QComboBox;
    modeCombo->setObjectName("aimPathMode");
    modeCombo->addItem(QStringLiteral("直线（透传，不整形）"), 0);
    modeCombo->addItem(QStringLiteral("贝塞尔曲线"), 1);
    modeCombo->addItem(QStringLiteral("自定义手绘"), 2);
    modeCombo->addItem(QStringLiteral("WindMouse（风力曲线）"), 3);
    modeCombo->addItem(QString::fromUtf8(u8"神经网络训练曲线"), 4);
    auto* modeRow = FormKit::fieldRow(QStringLiteral("轨迹模式"), modeCombo);
    attachTip(modeRow, QString::fromUtf8(
        u8"移动轨迹的整形方式。\n"
        u8"★ 直线 = 完全透传控制器输出，与不开这个功能逐位一致。\n"
        u8"★ 曲线模式都【只旋转不缩放】：曲线只决定「往哪个方向走」，\n"
        u8"  每拍走多远仍然由 PID 决定。所以它不会把控制器拖成振荡。\n"
        u8"★ 风力曲线额外带一个门控（见下面的门控阈值）：误差很小时整段\n"
        u8"  旁路走直线 —— 小修正保精度，只有大甩枪才走拟人路径。"));
    cl->addWidget(modeRow);

    auto* infl = new NoWheelSpinBox;
    infl->setRange(0, 100);
    infl->setObjectName("aimPathInfluence");
    infl->setValue(25);
    infl->setSuffix(QStringLiteral(" %"));
    m_pathInts.push_back(infl);
    auto* inflRow = FormKit::fieldRow(QStringLiteral("曲线影响量"), infl);
    attachTip(inflRow, QString::fromUtf8(
        u8"曲线对控制器原始方向的影响程度。\n"
        u8"★ 0% = 完全透传 PID（等于直线，但模式仍算开启）。\n"
        u8"★ 100% = 完整采用曲线切线方向。\n"
        u8"★ 默认 25%，避免曲线完全接管移动。"));
    cl->addWidget(inflRow);

    m_pathSectionBezier = makeSectionTitle(QString::fromUtf8(u8"贝塞尔控制点"));
    cl->addWidget(m_pathSectionBezier);
    attachTip(m_pathSectionBezier, QString::fromUtf8(
        u8"只用「贝塞尔曲线」模式时生效。\n"
        u8"曲线从起点 (0,0) 到终点 (1,0)，两个控制点决定它弯成什么样。\n"
        u8"X 是「走到全程的百分之几」，Y 是「横向偏出弦长的多少倍」。"));

    struct PD { const char* obj; const char* label; double lo, hi, step, def; const char* tip; };
    const PD bez[] = {
        { "pathCx1", "控制点 1 · X", 0.0, 1.0, 0.05, 0.30,
          "第一个控制点的 X（0~1，沿起点→终点方向的位置）。" },
        { "pathCy1", "控制点 1 · Y", -1.0, 1.0, 0.05, 0.0,
          "第一个控制点的 Y（-1~1，垂直方向的偏移比例）。\n正负决定往哪一侧弯。" },
        { "pathCx2", "控制点 2 · X", 0.0, 1.0, 0.05, 0.70,
          "第二个控制点的 X。" },
        { "pathCy2", "控制点 2 · Y", -1.0, 1.0, 0.05, 0.0,
          "第二个控制点的 Y。\n两个 Y 同号 = 往一侧弯；异号 = S 形。" },
    };
    m_pathSectionBezierRows.clear();
    for (const PD& d : bez)
    {
        QWidget* row = makePathDoubleRow(d.obj, d.label, d.lo, d.hi, d.step, d.def,
                                         QString::fromUtf8(d.tip));
        cl->addWidget(row);
        m_pathSectionBezierRows.push_back(row);
    }

    // ── 风力（WindMouse）参数 ──
    m_pathSectionWind = makeSectionTitle(QString::fromUtf8(u8"风力参数"));
    cl->addWidget(m_pathSectionWind);
    attachTip(m_pathSectionWind, QString::fromUtf8(
        u8"只用「WindMouse（风力曲线）」模式时生效。\n"
        u8"重力把准星往目标拉，风力把它往旁边推，两者拉扯出拟人轨迹。"));

    const PD winds[] = {
        { "windGravity", "重力 G", 0.1, 100.0, 0.5, 5.0,
          "向目标的吸引强度（WindMouse 的 G0，单位像素）。\n越大越「坚决」，路径越直、越快贴上目标。" },
        { "windWind", "风力 W", 0.0, 100.0, 0.5, 2.0,
          "横向随机游走的幅度（WindMouse 的 W0，单位像素）。\n越大路径越「飘」。调太大会明显绕着走。" },
        { "windStep", "步长 M", 1.0, 200.0, 1.0, 10.0,
          "单步最大长度（WindMouse 的 M0，单位像素）。\n越小路径越碎、越慢；越大越接近直线。" },
        { "windDistance", "风力衰减距离 D", 1.0, 200.0, 1.0, 8.0,
          "离目标多近时风力开始衰减（WindMouse 的 D0，单位像素）。\n保证最后一小段能稳住，不会在锚点附近乱飘。" },
    };
    m_pathSectionWindRows.clear();
    for (const PD& d : winds)
    {
        QWidget* row = makePathDoubleRow(d.obj, d.label, d.lo, d.hi, d.step, d.def,
                                         QString::fromUtf8(d.tip));
        cl->addWidget(row);
        m_pathSectionWindRows.push_back(row);
    }

    // 门控阈值 —— 只有 WindMouse 会读它（aim_path.h 里三处都判 mode == WindMouse）。
    m_windThresholdRow = makeIntRow("windThreshold", "门控阈值 (像素)", 0, 500, 1, 10,
        QString::fromUtf8(u8"两个轴的误差都【不超过】它时，整段曲线旁路，直接走直线。\n"
        u8"10px 意味着：微修正（贴住目标之后的抖动）走直线保精度，只有大甩枪才走拟人路径。\n"
        u8"设成 0 = 任何误差都走曲线（不是「关闭曲线」）。"));
    cl->addWidget(m_windThresholdRow);

    // ── 自定义手绘曲线 ──
    m_pathSectionCustom = makeSectionTitle(QString::fromUtf8(u8"手绘曲线"));
    cl->addWidget(m_pathSectionCustom);
    attachTip(m_pathSectionCustom, QString::fromUtf8(
        u8"只用「自定义手绘」模式时生效。\n"
        u8"在下面的画布上按住左键拖动即可画出轨迹：\n"
        u8"  横轴 = 行程进度（左端起点、右端终点）\n"
        u8"  纵轴 = 横向偏移比例（中间虚线是 0，往上往一侧弯、往下往另一侧）\n"
        u8"★ 两端请落在中轴线上 —— 起点要接上当前准星位置，终点要落在锚点。"));

    m_curveCanvas = new CurveCanvas;
    m_curveCanvas->setObjectName("aimCustomCurve");
    cl->addWidget(m_curveCanvas);
    m_pathSectionCustomRows.push_back(m_curveCanvas);

    {
        auto* btnRow = new QWidget;
        auto* bl = new QHBoxLayout(btnRow);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->addStretch(1);
        auto* clearBtn = new QPushButton(QString::fromUtf8(u8"清空曲线"));
        clearBtn->setObjectName("aimCurveClear");
        clearBtn->setCursor(Qt::PointingHandCursor);
        bl->addWidget(clearBtn);
        cl->addWidget(btnRow);
        m_pathSectionCustomRows.push_back(btnRow);

        connect(clearBtn, &QPushButton::clicked, this, [this]() {
            if (m_curveCanvas) m_curveCanvas->clearCurve();
        });
    }

    m_pathSectionNeural = makeSectionTitle(QString::fromUtf8(u8"神经网络训练曲线"));
    cl->addWidget(m_pathSectionNeural);
    m_neuralQualityLabel = makeHint(QString::fromUtf8(
        u8"尚未训练。点击下方按钮录制至少 5 条真实鼠标轨迹。"));
    cl->addWidget(m_neuralQualityLabel);
    m_pathSectionNeuralRows.push_back(m_neuralQualityLabel);
    m_neuralPreviewCanvas = new CurveCanvas;
    m_neuralPreviewCanvas->setObjectName("aimNeuralCurvePreview");
    m_neuralPreviewCanvas->setEnabled(false);
    cl->addWidget(m_neuralPreviewCanvas);
    m_pathSectionNeuralRows.push_back(m_neuralPreviewCanvas);
    auto* trainButton = new QPushButton(QString::fromUtf8(u8"录制轨迹并训练神经网络"));
    trainButton->setObjectName("aimNeuralCurveTrain");
    cl->addWidget(trainButton);
    m_pathSectionNeuralRows.push_back(trainButton);
    attachTip(trainButton, QString::fromUtf8(
        u8"在弹窗里用 Windows 桌面鼠标从金色起点拖到绿色目标，录制多轮真实路径。\n"
        u8"训练结束会用留出的轨迹评估拟合误差；点击应用后才写入当前热键。\n"
        u8"★ 评估的是曲线拟合质量，不是游戏命中率。"));
    const auto applyNeural = [this](int profileIndex, const boss::NeuralCurveTrainResult& result) {
        if (!result.success || profileIndex < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lock(configMutex);
            if (profileIndex >= static_cast<int>(config.hotkeys.size())) return;
            HotkeyProfile& hp = config.hotkeys[profileIndex];
            hp.aim_path_mode = 4;
            hp.aim_path_neural_trained = true;
            hp.aim_path_neural_weights = result.weights;
            hp.aim_path_neural_examples = result.quality.trainingTrajectories
                                        + result.quality.validationTrajectories;
            hp.aim_path_neural_validation_rmse =
                static_cast<float>(result.quality.validationRmse);
            hp.aim_path_neural_validation_p95 =
                static_cast<float>(result.quality.validationP95);
            hp.aim_path_neural_slope_variation =
                static_cast<float>(result.quality.slopeVariation);
            ConfigBridge::instance().markDirty();
        }
        reloadProfileToUi();
    };
    connect(trainButton, &QPushButton::clicked, this, [this, applyNeural] {
        const int profileIndex = currentRuntimeIndex();
        if (profileIndex < 0) return;
        auto* trainer = new NeuralCurveTrainerDialog(this);
        trainer->setAttribute(Qt::WA_DeleteOnClose);
        trainer->setWindowModality(Qt::WindowModal);
        trainer->onApply = [applyNeural, profileIndex](const boss::NeuralCurveTrainResult& result) {
            applyNeural(profileIndex, result);
        };
        trainer->show();
    });
    auto* randomButton = new QPushButton(QString::fromUtf8(u8"一键随机曲线"));
    randomButton->setObjectName("aimNeuralCurveRandom");
    cl->addWidget(randomButton);
    m_pathSectionNeuralRows.push_back(randomButton);
    attachTip(randomButton, QString::fromUtf8(
        u8"生成平滑的随机曲线并保存到当前热键，预览立即更新；再次点击可换一条。"
        u8"随机曲线不代表个人训练结果，弯曲程度仍受曲线影响参数控制。"));
    connect(randomButton, &QPushButton::clicked, this, [this, applyNeural] {
        applyNeural(currentRuntimeIndex(), boss::randomNeuralCurve(QRandomGenerator::global()->generate()));
    });

    auto* neuralFileRow = new QWidget;
    auto* neuralFileLayout = new QHBoxLayout(neuralFileRow);
    neuralFileLayout->setContentsMargins(0, 0, 0, 0);
    auto* importNeural = new QPushButton(QString::fromUtf8(u8"导入神经曲线"));
    importNeural->setObjectName("aimNeuralCurveImport");
    auto* exportNeural = new QPushButton(QString::fromUtf8(u8"导出神经曲线"));
    exportNeural->setObjectName("aimNeuralCurveExport");
    neuralFileLayout->addWidget(importNeural);
    neuralFileLayout->addWidget(exportNeural);
    cl->addWidget(neuralFileRow);
    m_pathSectionNeuralRows.push_back(neuralFileRow);
    connect(importNeural, &QPushButton::clicked, this, [this, applyNeural] {
        const int profileIndex = currentRuntimeIndex();
        if (profileIndex < 0) return;
        const QString path = QFileDialog::getOpenFileName(this,
            QString::fromUtf8(u8"导入神经曲线"), QString(),
            QString::fromUtf8(u8"神经曲线 (*.ancurve *.json)"));
        if (path.isEmpty()) return;
        boss::NeuralCurveTrainResult model;
        QString error;
        if (!neural_curve_file::load(path, model, error)) {
            QMessageBox::warning(this, QString::fromUtf8(u8"导入失败"), error);
            return;
        }
        applyNeural(profileIndex, model);
    });
    connect(exportNeural, &QPushButton::clicked, this, [this] {
        const int profileIndex = currentRuntimeIndex();
        if (profileIndex < 0) return;
        boss::NeuralCurveTrainResult model;
        {
            std::lock_guard<std::recursive_mutex> lock(configMutex);
            if (profileIndex >= static_cast<int>(config.hotkeys.size())) return;
            const HotkeyProfile& hp = config.hotkeys[profileIndex];
            model.success = hp.aim_path_neural_trained;
            model.weights = hp.aim_path_neural_weights;
            model.quality.trainingTrajectories = hp.aim_path_neural_examples;
            model.quality.validationRmse = hp.aim_path_neural_validation_rmse;
            model.quality.validationP95 = hp.aim_path_neural_validation_p95;
            model.quality.slopeVariation = hp.aim_path_neural_slope_variation;
        }
        if (!model.success) {
            QMessageBox::information(this, QString::fromUtf8(u8"没有曲线"),
                QString::fromUtf8(u8"请先训练、随机生成或导入神经曲线。"));
            return;
        }
        const QString path = QFileDialog::getSaveFileName(this,
            QString::fromUtf8(u8"导出神经曲线"), QStringLiteral("neural_curve.ancurve"),
            QString::fromUtf8(u8"神经曲线 (*.ancurve)"));
        if (path.isEmpty()) return;
        QString error;
        if (!neural_curve_file::save(path, model, error))
            QMessageBox::warning(this, QString::fromUtf8(u8"导出失败"), error);
    });

    auto commit = [this, modeCombo]() {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        HotkeyProfile& hp = config.hotkeys[ri];

        auto d = [this](const char* n) { return findChild<QDoubleSpinBox*>(n)->value(); };
        auto i = [this](const char* n) { return findChild<QSpinBox*>(n)->value(); };

        hp.aim_path_mode          = modeCombo->currentData().toInt();
        hp.aim_path_influence     = i("aimPathInfluence");
        hp.aim_path_bezier_cx1    = static_cast<float>(d("pathCx1"));
        hp.aim_path_bezier_cy1    = static_cast<float>(d("pathCy1"));
        hp.aim_path_bezier_cx2    = static_cast<float>(d("pathCx2"));
        hp.aim_path_bezier_cy2    = static_cast<float>(d("pathCy2"));
        hp.aim_path_wind_gravity  = static_cast<float>(d("windGravity"));
        hp.aim_path_wind_wind     = static_cast<float>(d("windWind"));
        hp.aim_path_wind_step     = static_cast<float>(d("windStep"));
        hp.aim_path_wind_distance = static_cast<float>(d("windDistance"));
        hp.aim_path_wind_threshold = i("windThreshold");

        // 手绘曲线：只在真画过东西时才存。全 0 的曲线等于直线，存了只是白占空间。
        if (m_curveCanvas)
        {
            const auto s = m_curveCanvas->samples();
            const bool allZero = std::all_of(s.begin(), s.end(),
                [](float v) { return std::abs(v) < 1e-4f; });
            if (allZero)
                hp.aim_path_custom_samples.reset();
            else
                hp.aim_path_custom_samples =
                    std::make_shared<const std::vector<float>>(s);
        }

        ConfigBridge::instance().markDirty();
    };

    // 模式切换时重排卡片：只显示当前模式真正会读的参数。
    auto applyMode = [this](int mode) {
        const bool isBezier = (mode == 1);
        const bool isCustom = (mode == 2);
        const bool isWind   = (mode == 3);
        const bool isNeural = (mode == 4);

        if (m_pathSectionBezier)
            m_pathSectionBezier->setVisible(isBezier);
        for (auto* w : m_pathSectionBezierRows) w->setVisible(isBezier);

        if (m_pathSectionWind)
            m_pathSectionWind->setVisible(isWind);
        for (auto* w : m_pathSectionWindRows) w->setVisible(isWind);
        if (m_windThresholdRow) m_windThresholdRow->setVisible(isWind);

        if (m_pathSectionCustom)
            m_pathSectionCustom->setVisible(isCustom);
        for (auto* w : m_pathSectionCustomRows) w->setVisible(isCustom);
        if (m_pathSectionNeural)
            m_pathSectionNeural->setVisible(isNeural);
        for (auto* w : m_pathSectionNeuralRows) w->setVisible(isNeural);
    };

    connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [commit, applyMode, modeCombo](int) {
                commit();
                applyMode(modeCombo->currentData().toInt());
            });
    for (auto* sp : m_pathInts)
        connect(sp, QOverload<int>::of(&QSpinBox::valueChanged), this, [commit](int) { commit(); });
    for (auto* sp : m_pathDoubles)
        connect(sp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [commit](double) { commit(); });
    if (m_curveCanvas)
        connect(m_curveCanvas, &CurveCanvas::curveChanged, this, [commit]() { commit(); });

    applyMode(modeCombo->currentData().toInt());

    m_rightLayout->addWidget(card);
}

int AimSettingsPage::currentRuntimeIndex() const
{
    auto* item = m_profileList ? m_profileList->currentItem() : nullptr;
    return item ? item->data(Qt::UserRole).toInt() : -1;
}

void AimSettingsPage::rebuildGroupCombo()
{
    if (!m_groupCombo) return;
    QString cur;
    m_groupCombo->blockSignals(true);
    m_groupCombo->clear();
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        cur = QString::fromStdString(config.active_hotkey_group);
        for (const auto& hp : config.hotkeys)
        {
            const QString g = QString::fromUtf8(hp.group.c_str());
            if (m_groupCombo->findText(g) < 0)
                m_groupCombo->addItem(g);
        }
    }
    const int idx = m_groupCombo->findText(cur);
    if (idx >= 0) m_groupCombo->setCurrentIndex(idx);
    m_groupCombo->blockSignals(false);
    if (idx < 0 && m_groupCombo->count() > 0) onGroupChanged(m_groupCombo->currentIndex());
}

void AimSettingsPage::rebuildProfileList()
{
    if (!m_profileList) return;
    const int previouslySelected = currentRuntimeIndex();
    m_profileList->blockSignals(true);
    m_profileList->clear();

    const QString group = m_groupCombo ? m_groupCombo->currentText() : QString();
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        for (int i = 0; i < static_cast<int>(config.hotkeys.size()); ++i)
        {
            const auto& hp = config.hotkeys[i];
            if (QString::fromUtf8(hp.group.c_str()) != group) continue;

            QString keys;
            for (const auto& k : hp.keys)
            {
                if (!keys.isEmpty()) keys += hp.keys_chord ? QStringLiteral(" + ") : QStringLiteral(" / ");
                keys += QString::fromUtf8(k.c_str());
            }
            if (keys.isEmpty()) keys = QStringLiteral("None");

            auto* item = new QListWidgetItem(m_profileList);
            item->setData(Qt::UserRole, i);
            item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);

            auto* w = new QWidget;
            w->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            auto* v = new QVBoxLayout(w);
            v->setContentsMargins(11, 8, 11, 8);
            v->setSpacing(3);

            auto* nameLbl = new QLabel(QString::fromUtf8(hp.name.c_str()));
            nameLbl->setObjectName("pname");
            auto* keyLbl = new QLabel(keys);
            keyLbl->setObjectName("pkey");

            v->addWidget(nameLbl);
            v->addWidget(keyLbl);

            item->setSizeHint(w->sizeHint());
            m_profileList->setItemWidget(item, w);
        }
    }
    m_profileList->blockSignals(false);
    if (m_profileList->count() > 0)
    {
        int rowToSelect = 0;
        for (int row = 0; row < m_profileList->count(); ++row)
            if (m_profileList->item(row)->data(Qt::UserRole).toInt() == previouslySelected) {
                rowToSelect = row;
                break;
            }
        m_profileList->setCurrentRow(rowToSelect);
        onProfileSelected(rowToSelect);
    }
    restyleProfileItems();
}

void AimSettingsPage::moveProfileInGroup(int fromRow, int toRow)
{
    if (!m_profileList || !m_groupCombo || fromRow == toRow ||
        fromRow < 0 || toRow < 0 ||
        fromRow >= m_profileList->count() || toRow >= m_profileList->count())
        return;

    std::vector<int> positions;
    positions.reserve(m_profileList->count());
    for (int row = 0; row < m_profileList->count(); ++row)
        positions.push_back(m_profileList->item(row)->data(Qt::UserRole).toInt());
    auto uniquePositions = positions;
    std::sort(uniquePositions.begin(), uniquePositions.end());
    if (std::adjacent_find(uniquePositions.begin(), uniquePositions.end()) !=
        uniquePositions.end()) return;

    const std::string group = m_groupCombo->currentText().toStdString();
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        for (const int index : positions)
            if (index < 0 || index >= static_cast<int>(config.hotkeys.size()) ||
                config.hotkeys[index].group != group)
                return;

        HotkeyProfile moved = std::move(config.hotkeys[positions[fromRow]]);
        if (fromRow < toRow)
            for (int row = fromRow; row < toRow; ++row)
                config.hotkeys[positions[row]] = std::move(config.hotkeys[positions[row + 1]]);
        else
            for (int row = fromRow; row > toRow; --row)
                config.hotkeys[positions[row]] = std::move(config.hotkeys[positions[row - 1]]);
        config.hotkeys[positions[toRow]] = std::move(moved);
    }
    ConfigBridge::instance().markDirty();
    rebuildProfileList();
    m_profileList->setCurrentRow(toRow);
}

void AimSettingsPage::restyleProfileItems()
{
    if (!m_profileList) return;
    for (int i = 0; i < m_profileList->count(); ++i)
    {
        auto* w = m_profileList->itemWidget(m_profileList->item(i));
        if (!w) continue;
        const bool sel = (i == m_profileList->currentRow());
        if (auto* n = w->findChild<QLabel*>("pname"))
            n->setStyleSheet(sel ? "color:#E9CD8A; font-size:13px; font-weight:500;"
                                 : "color:#DCD7CA; font-size:13px;");
        if (auto* k = w->findChild<QLabel*>("pkey"))
            k->setStyleSheet(sel ? "color:#C2AD7C; font-size:11px;"
                                 : "color:#A49E90; font-size:11px;");
    }
}

void AimSettingsPage::refreshActivation()
{
    if (m_loading || !m_activationWidget) return;
    std::lock_guard<std::recursive_mutex> lock(configMutex);
    m_activationWidget->load(config.hotkeys, currentRuntimeIndex());
    for (int row = 0; row < m_profileList->count(); ++row) {
        const int index = m_profileList->item(row)->data(Qt::UserRole).toInt();
        auto* widget = m_profileList->itemWidget(m_profileList->item(row));
        if (!widget || index < 0 || index >= static_cast<int>(config.hotkeys.size())) continue;
        if (auto* label = widget->findChild<QLabel*>("pname")) {
            QString name = QString::fromStdString(config.hotkeys[index].name);
            if (hasHotkeyConflict(config.hotkeys, index) && preferredHotkey(config.hotkeys, index) == index)
                name += QStringLiteral(" · 已激活");
            label->setText(name);
        }
    }
}

void AimSettingsPage::onGroupChanged(int)
{
    if (!m_groupCombo || m_groupCombo->currentIndex() < 0) return;
    bool changed = false;
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        const auto group = m_groupCombo->currentText().toStdString();
        changed = config.active_hotkey_group != group;
        config.active_hotkey_group = group;
    }
    if (changed) ConfigBridge::instance().markDirty();
    rebuildProfileList();
}

void AimSettingsPage::onProfileSelected(int)
{
    restyleProfileItems();
    reloadProfileToUi();
}

void AimSettingsPage::onTargetClassesChanged()
{
    reloadProfileToUi();
}

void AimSettingsPage::reloadProfileToUi()
{
    m_loading = true;
    const int ri = currentRuntimeIndex();

    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= 0 && ri < static_cast<int>(config.hotkeys.size()))
        {
            const HotkeyProfile& hp = config.hotkeys[ri];

            if (auto* c = findChild<QComboBox*>("keyCombo"))
            {
                const QString want = hp.keys.empty() ? QString()
                                                     : QString::fromUtf8(hp.keys.front().c_str());
                const int k = c->findData(want);
                c->setCurrentIndex(k >= 0 ? k : 0);
            }
            if (auto* c = findChild<QCheckBox*>("keyChord")) c->setChecked(hp.keys_chord);
            if (auto* c = findChild<QComboBox*>("keyComboSecond")) {
                const QString want = hp.keys.size() > 1 ? QString::fromStdString(hp.keys[1]) : QString();
                const int k = c->findData(want);
                c->setCurrentIndex(k >= 0 ? k : 0);
                c->setEnabled(hp.keys_chord);
            }
            if (auto* s = findChild<QSpinBox*>("fovX")) s->setValue(hp.fovX);
            if (auto* s = findChild<QSpinBox*>("fovY")) s->setValue(hp.fovY);
            if (auto* c = findChild<QCheckBox*>("maskX")) c->setChecked(hp.mask_x);
            if (auto* c = findChild<QCheckBox*>("blockAimHotkey")) c->setChecked(hp.block_hotkey);
            if (auto* c = findChild<QCheckBox*>("maskY")) c->setChecked(hp.mask_y);
            if (auto* c = findChild<QCheckBox*>("unlockX")) c->setChecked(hp.unlock_x);
            if (auto* c = findChild<QCheckBox*>("unlockY")) c->setChecked(hp.unlock_y);
            if (auto* s = findChild<QSpinBox*>("unlockYDelayMs")) s->setValue(hp.unlock_y_delay_ms);
            if (auto* s = findChild<QSpinBox*>("aimDelayMs")) s->setValue(hp.aim_delay_ms);
            if (auto* s = findChild<QSpinBox*>("dynFovShrinkMs")) s->setValue(hp.dynamic_fov_shrink_ms);
            if (auto* c = findChild<QComboBox*>("aimMode"))
                c->setCurrentIndex(hp.crosshair_detect_enabled ? 3
                    : hp.laser_detect_enabled ? 2 : hp.aimpoint_recoil_enabled ? 1 : 0);
            if (auto* c = findChild<QCheckBox*>("dynFovChk"))
                c->setChecked(hp.dynamic_fov_enabled);
            if (auto* s = findChild<QSpinBox*>("dynFovSize")) s->setValue(hp.dynamic_fov_size);
            if (auto* s = findChild<QSpinBox*>("dynFovExpandMs")) s->setValue(hp.dynamic_fov_expand_ms);

            if (auto* c = findChild<QCheckBox*>("ctlEnabled")) c->setChecked(hp.ctl_enabled);

            auto sd = [this](const char* n, double v) {
                if (auto* s = findChild<QDoubleSpinBox*>(n)) s->setValue(v);
            };
            auto si = [this](const char* n, int v) {
                if (auto* s = findChild<QSpinBox*>(n)) s->setValue(v);
            };
            auto loadRecovered = [this, &sd](const QString& prefix,
                                              const control::RecoveredPidConfig& pid) {
                auto set = [&](const char* suffix, float value) {
                    const QByteArray objectName = (prefix + QString::fromLatin1(suffix)).toUtf8();
                    sd(objectName.constData(), value);
                };
                set("KpX", pid.kpX); set("KiX", pid.kiX); set("KdX", pid.kdX);
                set("FfX", pid.feedforwardX); set("DeadzoneX", pid.deadzoneX);
                set("HardDeadzoneX", pid.hardDeadzoneX);
                set("KpY", pid.kpY); set("KiY", pid.kiY); set("KdY", pid.kdY);
                set("FfY", pid.feedforwardY); set("DeadzoneY", pid.deadzoneY);
                set("HardDeadzoneY", pid.hardDeadzoneY);
                set("MaxPixel", pid.smoothMaxPixel); set("Segment", pid.segment);
                set("FollowX", pid.followX);
                set("FollowY", pid.followY);
                if (auto* check = findChild<QCheckBox*>(prefix + "SegmentEnabled"))
                    check->setChecked(pid.segmentEnabled);
            };
            loadRecovered("recovered", hp.recovered_pid);
            loadRecovered("recoveredScope", hp.recovered_scope_pid);
            if (m_triggerWorkflow) m_triggerWorkflow->load(hp);
            if (m_triggerTargetEditor) m_triggerTargetEditor->load(hp, config.class_filters);

            // 开镜期间: 跟随热键 / 用独立那一套。
            if (auto* c = findChild<QComboBox*>("scopeCtlMode"))
            {
                const int k = c->findData(hp.scope_ctl_enabled != 0 ? 1 : 0);
                c->setCurrentIndex(k >= 0 ? k : 0);
            }
            if (auto* c = findChild<QComboBox*>("aimPathMode"))
            {
                const int k = c->findData(hp.aim_path_mode);
                c->setCurrentIndex(k >= 0 ? k : 0);
            }
            si("aimPathInfluence",    hp.aim_path_influence);
            si("windThreshold",       hp.aim_path_wind_threshold);
            sd("pathCx1",             hp.aim_path_bezier_cx1);
            sd("pathCy1",             hp.aim_path_bezier_cy1);
            sd("pathCx2",             hp.aim_path_bezier_cx2);
            sd("pathCy2",             hp.aim_path_bezier_cy2);
            sd("windGravity",         hp.aim_path_wind_gravity);
            sd("windWind",            hp.aim_path_wind_wind);
            sd("windStep",            hp.aim_path_wind_step);
            sd("windDistance",        hp.aim_path_wind_distance);

            if (m_curveCanvas)
                m_curveCanvas->setSamples(
                    hp.aim_path_custom_samples ? *hp.aim_path_custom_samples
                                               : std::vector<float>{});
            if (m_neuralPreviewCanvas && m_neuralQualityLabel)
            {
                if (hp.aim_path_neural_trained)
                {
                    std::vector<float> preview(512);
                    for (size_t pi = 0; pi < preview.size(); ++pi)
                        preview[pi] = static_cast<float>(boss::evaluateNeuralCurve(
                            hp.aim_path_neural_weights,
                            static_cast<double>(pi) / (preview.size() - 1)));
                    m_neuralPreviewCanvas->setSamples(preview);
                    if (hp.aim_path_neural_examples > 0)
                        m_neuralQualityLabel->setText(QString::fromUtf8(
                            u8"已训练 %1 条轨迹；留出验证均方根偏差 %2%，95% 点偏差 %3%；"
                            u8"斜率变化量 %4。指标衡量拟合质量，不是命中率。")
                            .arg(hp.aim_path_neural_examples)
                            .arg(hp.aim_path_neural_validation_rmse * 100.0, 0, 'f', 1)
                            .arg(hp.aim_path_neural_validation_p95 * 100.0, 0, 'f', 1)
                            .arg(hp.aim_path_neural_slope_variation, 0, 'f', 3));
                    else
                        m_neuralQualityLabel->setText(QString::fromUtf8(
                            u8"已载入随机生成或旧版曲线，无个人训练评分；可一键随机更换或录制训练。"));
                }
                else
                {
                    m_neuralPreviewCanvas->setSamples({});
                    m_neuralQualityLabel->setText(QString::fromUtf8(
                        u8"尚未训练。点击下方按钮录制至少 5 条真实鼠标轨迹；"
                        u8"未训练时该模式安全退回直线。"));
                }
            }

            rebuildAimClassRows();
        }
    }

    m_loading = false;
    refreshActivation();

}

void AimSettingsPage::reloadFromRuntime()
{
    rebuildGroupCombo();
    rebuildProfileList();
    reloadProfileToUi();
}

void AimSettingsPage::onAddProfile()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("新建热键"),
        QStringLiteral("名称"), QLineEdit::Normal, QStringLiteral("Aim"), &ok);
    if (!ok || name.isEmpty()) return;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        HotkeyProfile hp;
        hp.name = name.toStdString();
        hp.group = m_groupCombo->currentText().toStdString();
        hp.keys = { "RightMouseButton" };
        config.hotkeys.push_back(std::move(hp));
    }
    ConfigBridge::instance().markDirty();
    reloadFromRuntime();
}

void AimSettingsPage::onDeleteProfile()
{
    const int ri = currentRuntimeIndex();
    if (ri < 0) return;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (static_cast<int>(config.hotkeys.size()) <= 1) return;
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        config.hotkeys.erase(config.hotkeys.begin() + ri);
    }
    ConfigBridge::instance().markDirty();
    reloadFromRuntime();
}

void AimSettingsPage::onCopyProfile()
{
    const int ri = currentRuntimeIndex();
    if (ri < 0) return;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        m_copiedProfile = std::make_shared<HotkeyProfile>(config.hotkeys[ri]);
    }
}

void AimSettingsPage::onPasteProfile()
{
    if (!m_copiedProfile || !m_groupCombo || m_groupCombo->currentText().isEmpty()) return;
    int insertedIndex = -1;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        HotkeyProfile copy = *m_copiedProfile;
        copy.group = m_groupCombo->currentText().toStdString();
        copy.name += " 副本";
        copy.activation_key.clear();
        copy.activation_selected = false;
        insertedIndex = static_cast<int>(config.hotkeys.size());
        config.hotkeys.push_back(std::move(copy));
    }
    ConfigBridge::instance().markDirty();
    reloadFromRuntime();
    for (int row = 0; row < m_profileList->count(); ++row) {
        if (m_profileList->item(row)->data(Qt::UserRole).toInt() == insertedIndex) {
            m_profileList->setCurrentRow(row);
            break;
        }
    }
}

void AimSettingsPage::onAddGroup()
{
    bool ok = false;
    QString name = QInputDialog::getText(this, QStringLiteral("新建热键组"),
        QStringLiteral("组名:"), QLineEdit::Normal, QString(), &ok);
    if (!ok) return;
    name = name.trimmed();
    if (name.isEmpty()) return;

    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        HotkeyProfile hp;
        hp.name  = QStringLiteral("新热键").toStdString();
        hp.group = name.toStdString();
        config.hotkeys.push_back(std::move(hp));
    }
    ConfigBridge::instance().markDirty();

    rebuildGroupCombo();
    const int idx = m_groupCombo->findText(name);
    if (idx >= 0) m_groupCombo->setCurrentIndex(idx);
}

void AimSettingsPage::onDeleteGroup()
{
    const QString group = m_groupCombo->currentText();
    if (group.isEmpty()) return;

    const auto answer = QMessageBox::question(this, QStringLiteral("删除热键组"),
        QStringLiteral("删除组「%1」及其下所有热键？").arg(group));
    if (answer != QMessageBox::Yes) return;

    const std::string groupStd = group.toStdString();
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        config.hotkeys.erase(
            std::remove_if(config.hotkeys.begin(), config.hotkeys.end(),
                [&](const HotkeyProfile& h) { return h.group == groupStd; }),
            config.hotkeys.end());

        if (config.hotkeys.empty())
        {
            HotkeyProfile hp;
            hp.name  = "Aim";
            hp.group = QStringLiteral("默认").toStdString();
            config.hotkeys.push_back(std::move(hp));
        }
    }
    ConfigBridge::instance().markDirty();

    rebuildGroupCombo();
}
