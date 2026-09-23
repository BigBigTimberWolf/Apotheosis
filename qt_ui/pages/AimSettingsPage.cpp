#include "pages/AimSettingsPage.h"

#include "control/sensitivity_calibrator.h"

#include <QCheckBox>
#include <QDialog>
#include <QProgressBar>
#include <QTimer>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>          // QFrame::NoFrame
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>       // QLineEdit::Normal (QInputDialog 参数)
#include <QListWidget>
#include <QListWidgetItem> // 显式包含, 不依赖 QListWidget 的传递包含
#include <QMessageBox>     // 删除热键组的确认框
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>      // showEvent 的参数类型
#include <QSpinBox>
#include <QSplitter>       // 左栏/右栏可拖动分隔（旧页的写法）
#include <QVBoxLayout>
#include <QWheelEvent>     // NoWheelSpinBox/NoWheelDoubleSpinBox 的 wheelEvent 参数类型

#include <algorithm>
#include <iterator>   // std::size (开镜档的行表)
#include <mutex>

#include "Apotheosis.h"          // config / configMutex
#include "config.h"
#include "config/ConfigManager.h"
#include "config/config_bridge.h"
#include "pages/TargetPage.h"    // setTargetPage(): 取 &TargetPage::classFiltersChanged 需要完整定义
#include "runtime/config_snapshot.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
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

QWidget* AimSettingsPage::makeDoubleRow(const char* obj, const char* label,
                                        double lo, double hi, double step, double def)
{
    auto* sp = new NoWheelDoubleSpinBox;
    sp->setRange(lo, hi);
    sp->setSingleStep(step);
    sp->setDecimals(3);
    sp->setObjectName(QString::fromUtf8(obj));
    sp->setValue(def);
    m_ctlDoubles.push_back(sp);
    return FormKit::fieldRow(QString::fromUtf8(label), sp);
}

void AimSettingsPage::attachTip(QWidget* row, const QString& tip)
{
    if (!row || tip.isEmpty()) return;
    row->setToolTip(tip);
    const auto kids = row->findChildren<QWidget*>();
    for (QWidget* w : kids)
        w->setToolTip(tip);
}

QWidget* AimSettingsPage::makeDoubleRowTip(const char* obj, const char* label,
                                           double lo, double hi, double step,
                                           double def, const QString& tip)
{
    auto* row = makeDoubleRow(obj, label, lo, hi, step, def);
    attachTip(row, tip);
    return row;
}

// 轨迹卡片专用：makeDoubleRow 一律塞进 m_ctlDoubles（那是 PID 的列表），
// 但轨迹参数要进 m_pathDoubles —— 否则会被当成 PID 增益写进配置。
QWidget* AimSettingsPage::makePathDoubleRow(const char* obj, const char* label,
                                            double lo, double hi, double step,
                                            double def, const QString& tip)
{
    auto* row = makeDoubleRow(obj, label, lo, hi, step, def);
    // 从 m_ctlDoubles 末尾摘下来，改登记到 m_pathDoubles
    if (!m_ctlDoubles.empty() && m_ctlDoubles.back()->objectName() == QString::fromUtf8(obj))
    {
        m_pathDoubles.push_back(m_ctlDoubles.back());
        m_ctlDoubles.pop_back();
    }
    attachTip(row, tip);
    return row;
}

QWidget* AimSettingsPage::makeIntRow(const char* obj, const char* label, int lo, int hi,
                                     int step, int def, const QString& tip)
{
    std::vector<QSpinBox*>* sink = &m_ctlInts;
    const QString name = QString::fromUtf8(obj);
    if (name.startsWith(QLatin1String("trigger")))      sink = &m_triggerInts;
    else if (name.startsWith(QLatin1String("wind")) ||
             name.startsWith(QLatin1String("aimPath"))) sink = &m_pathInts;

    auto* sp = new NoWheelSpinBox;
    sp->setRange(lo, hi);
    sp->setSingleStep(step);
    sp->setObjectName(name);
    sp->setValue(def);
    sink->push_back(sp);
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

    auto* groupLabel = new QLabel(QStringLiteral("热键组"));
    groupLabel->setStyleSheet("color:#A1A1AA; font-size:11px; font-weight:500;");
    lay->addWidget(groupLabel);

    auto* groupRow = new QHBoxLayout;
    groupRow->setSpacing(4);
    m_groupCombo = new QComboBox;
    m_groupCombo->setMinimumHeight(30);
    groupRow->addWidget(m_groupCombo, 1);

    const QString smallBtnSS =
        "QPushButton{font-size:16px; color:#71717A; background:transparent;"
        " border:1px solid rgba(0,0,0,0.08); border-radius:4px; padding:0;}"
        "QPushButton:hover{color:#5E6AD2; border-color:#5E6AD2;}";

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
    m_leftTitle = new QLabel(QStringLiteral("热键"));
    m_leftTitle->setStyleSheet("color:#A1A1AA; font-size:11px; font-weight:500;");
    header->addWidget(m_leftTitle);
    header->addStretch();
    lay->addLayout(header);

    m_profileList = new QListWidget;
    m_profileList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_profileList->setFrameShape(QFrame::NoFrame);
    m_profileList->setStyleSheet(
        "QListWidget{background:transparent; border:none; outline:none; padding:0;}"
        "QListWidget::item{padding:0; margin:0 0 5px 0; border-radius:9px; background:#FFFFFF;"
        " border:1px solid rgba(0,0,0,0.05);}"
        "QListWidget::item:selected{background:#EEF0FC; border:1px solid #EEF0FC;}");
    lay->addWidget(m_profileList, 1);

    connect(m_profileList, &QListWidget::currentRowChanged,
            this, &AimSettingsPage::onProfileSelected);

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
    buildControllerCard();
    buildTriggerCard();
    buildScopeCtlCard();
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
    cl->addWidget(FormKit::fieldRow(QStringLiteral("按住此键时瞄准生效"), combo));

    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, combo](int) {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            const QString id = combo->currentData().toString();
            config.hotkeys[ri].keys.clear();
            if (!id.isEmpty())
                config.hotkeys[ri].keys.push_back(id.toStdString());
        }
        ConfigBridge::instance().markDirty();
        rebuildProfileList();
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

    auto commit = [this]() {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        config.hotkeys[ri].fovX = findChild<QSpinBox*>("fovX")->value();
        config.hotkeys[ri].fovY = findChild<QSpinBox*>("fovY")->value();
        ConfigBridge::instance().markDirty();
    };
    connect(fx, QOverload<int>::of(&QSpinBox::valueChanged), this, [commit](int) { commit(); });
    connect(fy, QOverload<int>::of(&QSpinBox::valueChanged), this, [commit](int) { commit(); });

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

    struct Row { int cid; float yMin; float yMax; float c; QString name; };
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
                rows.push_back({ ac.class_id, ac.y_offset, ac.y_offset_max, ac.min_conf, name });
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
            "QFrame#aimRow{background:#FAFAFB; border:1px solid rgba(0,0,0,0.06);"
            " border-radius:8px;}");
        auto* rl = new QVBoxLayout(rowFrame);
        rl->setContentsMargins(12, 8, 10, 10);
        rl->setSpacing(8);

        auto* top = new QHBoxLayout;
        top->setSpacing(8);

        auto* priLabel = new QLabel(QStringLiteral("#%1").arg(idx + 1));
        priLabel->setFixedWidth(30);
        priLabel->setStyleSheet("color:#5E6AD2; font-size:13px; font-weight:600; border:none;");
        top->addWidget(priLabel);

        auto* nameLabel = new QLabel(QStringLiteral("[%1] %2").arg(r.cid).arg(r.name));
        nameLabel->setStyleSheet("color:#3C3C44; font-size:13px; font-weight:500; border:none;");
        top->addWidget(nameLabel, 1);

        auto makeIconBtn = [](const QString& glyph, const QString& color,
                              const QString& hover, const QString& tip) {
            auto* b = new QPushButton(glyph);
            b->setFixedSize(26, 26);
            b->setCursor(Qt::PointingHandCursor);
            b->setToolTip(tip);
            b->setStyleSheet(QStringLiteral(
                "QPushButton{color:%1; background:transparent;"
                " border:1px solid rgba(0,0,0,0.08); border-radius:6px;"
                " font-size:13px; padding:0;}"
                "QPushButton:hover{color:%2; border-color:%2;}"
                "QPushButton:disabled{color:#C8C8CE; border-color:rgba(0,0,0,0.05);}")
                .arg(color, hover));
            return b;
        };

        auto* upBtn = makeIconBtn(QString::fromUtf8(u8"▲"), QStringLiteral("#71717A"),
                                  QStringLiteral("#5E6AD2"),
                                  QString::fromUtf8(u8"上移（提高优先级）"));
        auto* downBtn = makeIconBtn(QString::fromUtf8(u8"▼"), QStringLiteral("#71717A"),
                                    QStringLiteral("#5E6AD2"),
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
        yLbl->setStyleSheet("color:#71717A; font-size:12px; border:none;");
        auto* yMinSpin = makeOffsetSpin(r.yMin);
        auto* yMaxSpin = makeOffsetSpin(r.yMax);
        yMinSpin->setToolTip(QString::fromUtf8(
            u8"范围下限：1=框顶，0.5=中心，0=框底。\n"
            u8"★ 该类的值会覆盖「控制器」卡里的热键级瞄点 Y（只在设了该类时）。"));
        yMaxSpin->setToolTip(QString::fromUtf8(
            u8"范围上限：每次新锁定在上下限之间随机一次。\n"
            u8"★ 等于下限时不随机（固定打同一个点）。"));
        rangeRow->addWidget(yLbl);
        rangeRow->addWidget(yMinSpin);
        rangeRow->addWidget(new QLabel(QString::fromUtf8(u8"—")));
        rangeRow->addWidget(yMaxSpin);
        rangeRow->addStretch();
        rl->addLayout(rangeRow);

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
        cLbl->setStyleSheet("color:#71717A; font-size:12px; border:none;");
        auto* cVal = new QLabel(confText(cSlider->value()));
        cVal->setFixedWidth(38);
        cVal->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        cVal->setStyleSheet("color:#3C3C44; font-size:12px; border:none;");
        bottom->addWidget(cLbl);
        bottom->addWidget(cSlider, 1);
        bottom->addWidget(cVal);
        rl->addLayout(bottom);

        m_aimClassLayout->addWidget(rowFrame);

        auto persistRange = [this, classId, yMinSpin, yMaxSpin](bool minChanged) {
            if (m_loading) return;
            if (minChanged && yMinSpin->value() > yMaxSpin->value())
                yMaxSpin->setValue(yMinSpin->value());
            else if (!minChanged && yMaxSpin->value() < yMinSpin->value())
                yMinSpin->setValue(yMaxSpin->value());

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
                        break;
                    }
            }
            ConfigBridge::instance().markDirty();
        };
        connect(yMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [persistRange](double) { persistRange(true); });
        connect(yMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [persistRange](double) { persistRange(false); });

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
    auto* card = new CardWidget(QStringLiteral("准星找色"), QStringLiteral("crosshair"));
    auto* cl = card->contentLayout();

    auto* chk = new QCheckBox(QStringLiteral("启用找色（用检测到的准星位置代替画面中心）"));
    chk->setObjectName("crosshairChk");
    chk->setToolTip(QString::fromUtf8(
        u8"★ 关：准星 = 画面中心（静态常量）。\n"
        u8"★ 开：用找色结果；找色失效时【退回画面中心】并跳过本拍控制。"));
    cl->addWidget(chk);

    connect(chk, &QCheckBox::toggled, this, [this](bool v) {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            config.hotkeys[ri].crosshair_detect_enabled = v;
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

    auto* spin = new NoWheelDoubleSpinBox;
    spin->setRange(0.0, 1.0);
    spin->setSingleStep(0.05);
    spin->setDecimals(2);
    spin->setObjectName("dynFovStrength");
    cl->addWidget(FormKit::fieldRow(QStringLiteral("收敛强度 (0=不收缩, 1=紧贴目标框)"), spin));

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
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            if (ri >= static_cast<int>(config.hotkeys.size())) return;
            config.hotkeys[ri].dynamic_fov_strength = static_cast<float>(v);
        }
        ConfigBridge::instance().markDirty();
    });

    m_rightLayout->addWidget(card);
}

void AimSettingsPage::buildControllerCard()
{
    auto* card = new CardWidget(QStringLiteral("瞄准控制器（通用控制器层）"),
                                QStringLiteral("adjustments"));
    auto* cl = card->contentLayout();

    auto* enable = new QCheckBox(QStringLiteral("★ 启用控制器（会真的往游戏机发鼠标位移）"));
    enable->setObjectName("ctlEnabled");
    enable->setToolTip(QString::fromUtf8(
        u8"⚠️ 默认关闭。打开后本程序会真的动鼠标 —— 参数未在真机标定过，"
        u8"第一次打开请先把最大位移调小、并准备好随时关掉。"));
    cl->addWidget(enable);

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"增益（水平 x = 跟枪 / 垂直 y = 压枪）")));

    struct D { const char* obj; const char* label; double lo, hi, step, def; const char* tip; };
    const D gains[] = {
        { "ctlKpX", "Kp · 水平", 0.0, 500.0, 0.5, 35.0,
          "比例增益（跟枪 / 水平轴）。误差乘以它 = 本拍要走的位移。\n"
          "调大：贴上去更快，但太大（配合 s_max 过高）会开始左右摆动。\n"
          "★ 这是唯一非零的默认增益，调参先只动它。" },
        { "ctlKpY", "Kp · 垂直", 0.0, 500.0, 0.5, 35.0,
          "比例增益（压枪 / 垂直轴）。\n"
          "和 Kp·水平分开，是因为压枪和跟枪的手感需求不同。\n"
          "默认与水平相同。" },
        { "ctlKiX", "Ki · 水平", 0.0, 100.0, 0.01, 0.0,
          "积分增益（水平轴）。累积残差，用来消掉匀速目标留下的稳态滞后。\n"
          "★ 默认 0（关闭）。开太大遇到目标急停会过冲、来回甩。\n"
          "只在确认有稳态滞后时才加。" },
        { "ctlKiY", "Ki · 垂直", 0.0, 100.0, 0.01, 0.0,
          "积分增益（垂直轴）。默认 0（关闭），理由同水平轴。" },
        { "ctlKdX", "Kd · 水平", 0.0, 100.0, 0.01, 0.0,
          "微分增益（水平轴）。按误差变化速度提前刹车，抑制过冲。\n"
          "★ 默认 0。它对检测噪声很敏感 —— 调大之前先确认框是稳的。" },
        { "ctlKdY", "Kd · 垂直", 0.0, 100.0, 0.01, 0.0,
          "微分增益（垂直轴）。默认 0，理由同水平轴。" },
        { "ctlPFullScalePx", "P 项饱和 (像素, 0=不限)", 0.0, 2000.0, 1.0, 0.0,
          "P 项连续饱和阈值（像素）。误差超过它之后 P 项不再增大。\n"
          "★ 它负责『末段不冲过头』，取代了早期的死区。\n"
          "★ 0 = 不限。设成 0 以外的值会让大甩枪的力度被削平。" },
        { "ctlTauUnwindSec", "积分回吐时间常数 (秒)", 0.001, 5.0, 0.005, 0.030,
          "误差【反向】时积分按 exp(-dt/τ) 回吐的时间常数（秒）。\n"
          "越小 = 回吐越快，越不容易在目标变向时被旧积分顶着走。\n"
          "★ 30ms 是历史调整后的起点，无实测依据。" },
        { "ctlTauDerivSec", "D 项低通时间常数 (秒)", 0.0, 5.0, 0.005, 0.020,
          "D 项的低通时间常数（秒）。目标急停时误差导数会出现尖峰，\n"
          "低通用来削掉它，免得准星被朝『目标原来运动的方向』猛推一下。\n"
          "★ 0 = 不低通（噪声会直接进 D 项）。" },
        { "ctlIMax", "积分上限 (0=用输出限幅)", 0.0, 5000.0, 1.0, 0.0,
          "积分项的上限。0 = 直接用输出限幅当上限。\n"
          "限制积分是为了防止长时间同向误差把积分喂得过大，\n"
          "一旦反向就变成一大坨甩不掉的输出。" },
    };
    for (const D& d : gains)
        cl->addWidget(makeDoubleRowTip(d.obj, d.label, d.lo, d.hi, d.step, d.def,
                                       QString::fromUtf8(d.tip)));

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"输出限幅与随机化")));
    cl->addWidget(makeIntRow("ctlMaxOutputCounts", "单拍最大位移 (计数)", 1, 1000, 1, 200,
        QString::fromUtf8(u8"一拍最多发多少个鼠标计数（1 计数 = 链路的最小位移）。\n"
        "它是最后一道安全闸：不管 PID 算出多大的值，单拍都不会超过它。\n"
        "★ 调小 = 更安全但更慢；调大 = 甩枪更猛，但错的时候也更猛。\n"
        "★ 第一次打开控制器建议先设小一点。")));
    cl->addWidget(makeIntRow("ctlRandomSeed", "瞄点随机种子 (0=固定)", 0, 999999, 1, 0,
        QString::fromUtf8(u8"瞄点 Y 随机抖动的种子。0 = 用内部固定常数（同一帧可复现）。\n"
        "★ 非 0 时每次启动都会得到不同的抖动序列。\n"
        "★ 只在「瞄准类别」里某一类的『随机锁点 Y』上下限【不相等】时才有意义。")));

    // ── 灵敏度折算系数 k（修正预测吃到的速度）──────────────────────────────
    {
        auto* title = makeSectionTitle(QString::fromUtf8(u8"灵敏度折算系数 (修正预测速度)"));
        title->setToolTip(QString::fromUtf8(
            u8"★ 下面「在途补偿」的速度取自画面观测，但这个观测值有系统性偏差：\n"
            u8"准星每追近目标一截，画面里目标的相对位移就被抵消一截——追得越准，\n"
            u8"观测到的速度越比真实速度小，预测因此总是显得「不够用」。\n"
            u8"这里把自身下发的鼠标计数按 k(像素/计数) 折算回像素、加回观测速度，\n"
            u8"就能拿到目标接近真实的速度。0 = 关闭这项修正（预测仍能用，只是偏保守）。"));
        cl->addWidget(title);
    }

    {
        auto* row = new QWidget;
        auto* hl = new QHBoxLayout(row);
        hl->setContentsMargins(0, 0, 0, 0);

        auto* spinK = new NoWheelDoubleSpinBox;
        spinK->setObjectName("ctlKPxPerCount");
        spinK->setRange(0.0, 10.0);
        spinK->setSingleStep(0.005);
        spinK->setDecimals(4);
        spinK->setValue(0.0);
        m_ctlDoubles.push_back(spinK);

        auto* lbl = new QLabel(QString::fromUtf8(u8"灵敏度折算系数 k (像素/计数, 0=关):"));
        lbl->setToolTip(QString::fromUtf8(
            u8"发 1 个鼠标计数，准星在画面上移动多少像素。\n"
            u8"★ 填入你本机的实测值，即开启目标速度的自身运动修正。\n"
            u8"★ 不知道填多少？点击右侧「测算灵敏度」一键在线拟合。"));

        auto* btnCalib = new QPushButton(QString::fromUtf8(u8"测算灵敏度"));
        btnCalib->setStyleSheet("background-color: #238636; color: white; font-weight: bold; padding: 4px 12px; border-radius: 4px;");

        hl->addWidget(lbl);
        hl->addWidget(spinK, 1);
        hl->addWidget(btnCalib);
        cl->addWidget(row);

        connect(btnCalib, &QPushButton::clicked, this, [this, spinK]() {
            showSensitivityCalibrateDialog(spinK);
        });
    }

    // ── 在途补偿（预测提前量）────────────────────────────────────────────
    {
        auto* title = makeSectionTitle(QString::fromUtf8(u8"在途补偿 (预测提前量)"));
        title->setToolTip(QString::fromUtf8(
            u8"链路（采集 → 推理 → 瞄准 → 下发 → 游戏渲染）有几十毫秒延迟，"
            u8"等这一拍算完，目标已经跑掉了。\n"
            u8"在途补偿按目标速度把瞄准点往前推一段，抵消这段延迟。\n"
            u8"★ 「预测提前时间」是总开关：填 0 则整个功能关闭，下面两项不生效。"));
        cl->addWidget(title);
    }

    cl->addWidget(makeDoubleRowTip("ctlPredictLeadMs",
        "预测提前时间 (毫秒, 0=关闭)", 0.0, 1000.0, 1.0, 0.0,
        QString::fromUtf8(
        u8"【总开关】预测提前时间（毫秒）= 整条链路的【全部延迟】。\n"
        u8"由你自己测量后填入，把采集 / 推理 / 瞄准 / 下发 / 游戏渲染\n"
        u8"全部算在这一个值里。\n"
        u8"★ 只用这一个来源，程序不会再自动往里加任何东西 ——\n"
        u8"  填多少就是多少，不会出现重复计算。\n"
        u8"★ 0 = 关闭（默认）。关闭时行为与没有这个功能时完全一致。\n"
        u8"★ 调大 = 更早打提前量，但太大在目标急停时会冲过头。\n"
        u8"★ 日志里的 ref auto 是程序自己测到的链路延迟，仅供对照参考。")));

    cl->addWidget(makeDoubleRowTip("ctlPredictMaxVelocityPxPerSec",
        "速度上限 (像素/秒, 0=不限)", 0.0, 100000.0, 10.0, 0.0,
        QString::fromUtf8(
        u8"目标速度估计超过它时【钳住速度】—— 保留方向、只压大小。\n"
        u8"钳住而不是放弃，是为了让提前量连续，不会时有时无。\n"
        u8"★ 0 = 不限制（默认）。\n"
        u8"★ 怎么定：先看日志里打出的 v= 实际速度值，再往上留点余量。\n"
        u8"★ 定太低会让正常移动的目标被当成异常，提前量被白白压掉。")));

    cl->addWidget(makeDoubleRowTip("ctlPredictMaxLeadRatio",
        "预测距离上限 (目标框对角线倍数, 0=不限)", 0.0, 100.0, 0.05, 0.0,
        QString::fromUtf8(
        u8"预测推进量的硬上限，单位是【目标框对角线倍数】。\n"
        u8"1.0 = 最多提前一个对角线；0.5 = 半个。\n"
        u8"★ 0 = 不限制（默认）。\n"
        u8"★ 用相对量而不是绝对像素，是为了让远近目标的保护尺度一致 ——\n"
        u8"  固定的像素数在近处（框大）会显得太小、远处（框小）会显得太大。\n"
        u8"★ 它是最后一道保险：异常速度估计不会把准星甩出去。")));

    // ── 在途自身位移补偿 (Smith) ──────────────────────────────────────────
    // ★ 与上面「在途补偿(预测提前量)」是两回事：那个补的是【目标】在延迟期间
    //   走了多远；这个补的是【自己】已经发出去、画面还没显现的位移。两者互不
    //   干扰，命名容易混，所以分成独立的小节。
    {
        auto* title = makeSectionTitle(QString::fromUtf8(u8"在途自身位移补偿 (Smith)"));
        title->setToolTip(QString::fromUtf8(
            u8"链路死区内（约 46ms）已经发出去、游戏里已生效、但画面还没显现的自身"
            u8"位移，会被控制器当成「目标还没动」重复下令，导致锁定目标后来回抖动。\n"
            u8"这里把这部分位移从下一拍的输出里扣掉，纯计数域运算，不需要任何"
            u8"灵敏度标定。"));
        cl->addWidget(title);
    }

    cl->addWidget(makeDoubleRowTip("ctlInflightBeta",
        "在途补偿强度 (无量纲, 0=关闭)", 0.0, 3.0, 0.05, 1.6,
        QString::fromUtf8(
        u8"每拍从输出里扣掉「窗口内平均每拍已发出 counts」的这个倍数。\n"
        u8"★ 1.0 = 理论上的精确抵消点；实测适度调高（1.6）比精确点收敛更快、"
        u8"过冲更小，这是因为它顺带压掉了 PID 自身残留的超调。\n"
        u8"★ 超过 2.0 在低帧率下开始发散（实测 60fps 尾段从 0.29px 恶化到"
        u8"56px），上限 3.0 只是挡住填错量级的配置，不是可用值。\n"
        u8"★ 0 = 关闭，与没有这个功能逐位相同。\n"
        u8"★ 锁定目标后如果左右抖、必须靠降 Kp 才能压住，先把这个调到 1.6"
        u8"再重新试拉枪速度。")));

    card->setToolTip(QString::fromUtf8(
        u8"★ 「稳定器」那 5 项与滞回倍数目前都是【占位值】，没有实测依据，"
        u8"默认值只保证「程序能跑」。\n"
        u8"★ 六个增益默认 Kp=35 / 其余 0，等价于历史单套行为 —— 是安全起点。\n"
        u8"★ 改完立即生效：控制器每拍重读配置，不用重启会话。"));

    auto commit = [this, enable]() {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        HotkeyProfile& hp = config.hotkeys[ri];

        auto d = [this](const char* n) { return findChild<QDoubleSpinBox*>(n)->value(); };
        auto i = [this](const char* n) { return findChild<QSpinBox*>(n)->value(); };

        hp.ctl_enabled          = enable->isChecked();
        hp.ctl_kp_x             = d("ctlKpX");
        hp.ctl_kp_y             = d("ctlKpY");
        hp.ctl_ki_x             = d("ctlKiX");
        hp.ctl_ki_y             = d("ctlKiY");
        hp.ctl_kd_x             = d("ctlKdX");
        hp.ctl_kd_y             = d("ctlKdY");
        hp.ctl_tau_unwind_sec   = d("ctlTauUnwindSec");
        hp.ctl_tau_deriv_sec    = d("ctlTauDerivSec");
        hp.ctl_i_max            = d("ctlIMax");
        hp.ctl_max_output_counts= i("ctlMaxOutputCounts");
        hp.ctl_p_full_scale_px  = d("ctlPFullScalePx");
        hp.ctl_predict_lead_ms  = d("ctlPredictLeadMs");
        hp.ctl_predict_max_velocity_px_s = d("ctlPredictMaxVelocityPxPerSec");
        hp.ctl_predict_max_lead_ratio    = d("ctlPredictMaxLeadRatio");
        hp.ctl_k_px_per_count   = d("ctlKPxPerCount");
        hp.ctl_inflight_beta    = d("ctlInflightBeta");
        hp.ctl_random_seed      = i("ctlRandomSeed");

        ConfigBridge::instance().markDirty();
    };

    connect(enable, &QCheckBox::toggled, this, [commit](bool) { commit(); });
    for (auto* sp : m_ctlDoubles)
        connect(sp, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [commit](double) { commit(); });
    for (auto* sp : m_ctlInts)
        connect(sp, QOverload<int>::of(&QSpinBox::valueChanged), this, [commit](int) { commit(); });

    m_rightLayout->addWidget(card);
}

void AimSettingsPage::showSensitivityCalibrateDialog(QDoubleSpinBox* spinK)
{
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle(QString::fromUtf8(u8"灵敏度折算系数 (k) 在线测算"));
    dlg->resize(440, 260);

    auto* layout = new QVBoxLayout(dlg);

    auto* guide = new QLabel(QString::fromUtf8(
        u8"<b>测算指引：</b><br>"
        u8"1. 在游戏训练场中，将准星对准一个<b>静止的假人 / 靶子</b>。<br>"
        u8"2. 点击下方的「开始采集」。<br>"
        u8"3. 按住热键，<b>左右甩动鼠标 2 ~ 3 次</b>（产生画面目标相对位移）。<br>"
        u8"4. 进度条跑满并出现计算结果后，点击「应用回填」即可！"));
    guide->setWordWrap(true);
    layout->addWidget(guide);

    auto* statusLbl = new QLabel(QString::fromUtf8(u8"状态：等待开始..."));
    statusLbl->setStyleSheet("font-weight: bold; color: #4da3ff; margin-top: 8px;");
    layout->addWidget(statusLbl);

    auto* pbar = new QProgressBar;
    pbar->setRange(0, 100);
    pbar->setValue(0);
    layout->addWidget(pbar);

    auto* resultLbl = new QLabel(QString::fromUtf8(u8"当前估算 k: -- px/count"));
    resultLbl->setStyleSheet("font-size: 15px; font-weight: bold; color: #3fb950; margin: 6px 0;");
    layout->addWidget(resultLbl);

    auto* btnRow = new QWidget;
    auto* hl = new QHBoxLayout(btnRow);
    hl->setContentsMargins(0, 0, 0, 0);

    auto* btnToggle = new QPushButton(QString::fromUtf8(u8"开始采集"));
    btnToggle->setStyleSheet("background-color: #238636; color: white; font-weight: bold; padding: 6px 16px;");

    auto* btnApply = new QPushButton(QString::fromUtf8(u8"应用回填"));
    btnApply->setEnabled(false);
    btnApply->setStyleSheet("padding: 6px 16px;");

    auto* btnCancel = new QPushButton(QString::fromUtf8(u8"关闭"));
    btnCancel->setStyleSheet("padding: 6px 16px;");

    hl->addWidget(btnToggle);
    hl->addWidget(btnApply);
    hl->addWidget(btnCancel);
    layout->addWidget(btnRow);

    auto* timer = new QTimer(dlg);

    connect(btnToggle, &QPushButton::clicked, dlg, [btnToggle, timer]() {
        auto& calib = control::globalSensitivityCalibrator();
        if (!calib.isRunning())
        {
            calib.start();
            btnToggle->setText(QString::fromUtf8(u8"停止采集"));
            btnToggle->setStyleSheet("background-color: #da3633; color: white; font-weight: bold; padding: 6px 16px;");
            timer->start(50);
        }
        else
        {
            calib.stop();
            btnToggle->setText(QString::fromUtf8(u8"开始采集"));
            btnToggle->setStyleSheet("background-color: #238636; color: white; font-weight: bold; padding: 6px 16px;");
            timer->stop();
        }
    });

    connect(timer, &QTimer::timeout, dlg, [statusLbl, pbar, resultLbl, btnApply]() {
        auto& calib = control::globalSensitivityCalibrator();
        auto st = calib.status();
        statusLbl->setText(QString::fromUtf8(u8"状态：%1").arg(QString::fromUtf8(st.hint)));
        pbar->setValue(static_cast<int>(st.progress * 100.0));
        if (st.estimatedK > 0.0)
        {
            resultLbl->setText(QString::fromUtf8(u8"当前估算 k: %1 px/count").arg(st.estimatedK, 0, 'f', 4));
        }
        if (st.ready)
        {
            btnApply->setEnabled(true);
            btnApply->setStyleSheet("background-color: #1f6feb; color: white; font-weight: bold; padding: 6px 16px;");
        }
    });

    connect(btnApply, &QPushButton::clicked, dlg, [dlg, spinK]() {
        auto& calib = control::globalSensitivityCalibrator();
        if (calib.estimatedK() > 0.0)
        {
            spinK->setValue(calib.estimatedK());
        }
        calib.stop();
        dlg->accept();
    });

    connect(btnCancel, &QPushButton::clicked, dlg, [dlg]() {
        control::globalSensitivityCalibrator().stop();
        dlg->reject();
    });

    connect(dlg, &QDialog::finished, dlg, []() {
        control::globalSensitivityCalibrator().stop();
    });

    dlg->exec();
}

void AimSettingsPage::buildTriggerCard()
{
    auto* card = new CardWidget(QString::fromUtf8(u8"自动扳机"),
                                QStringLiteral("crosshair"));
    auto* cl = card->contentLayout();

    auto* enable = new QCheckBox(QStringLiteral("启用自动扳机（准星进入命中区就开火）"));
    enable->setObjectName("triggerEnabled");
    cl->addWidget(enable);
    attachTip(enable, QString::fromUtf8(
        u8"总开关。关着的时候不会碰左键，也不会碰右键。\n"
        u8"★ 命中区 = 以【检测框】为基准的一个区间，与瞄点无关。\n"
        u8"★ 打开后本程序会真的开火 —— 请先确认瞄准控制器已经调好。"));

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"开火时机")));
    cl->addWidget(makeIntRow("triggerYPercent", "命中区占框的百分比 (%)", 10, 300, 5, 100,
        QString::fromUtf8(u8"命中区的高度 = 框高 × 该百分比，宽度同理（等比）。\n"
        u8"★ 100 = 整框；>100 = 框上方也算（预开火，会打得更早）；\n"
        u8"  <100 = 只有框中间一条算（更严格的「打到才开火」）。\n"
        u8"★ 这个区间同时管横向和纵向，所以它是一个正方形比例。")));
    cl->addWidget(makeIntRow("triggerFireDelay", "进区后延迟开火 (ms)", 0, 2000, 5, 0,
        QString::fromUtf8(u8"进入命中区之后等这么多毫秒才按下左键。\n"
        u8"★ 0 = 进区那一拍立刻开火（机械级瞬发）。\n"
        u8"★ 想「停稳了再开枪」就调大它 —— 配合自动急停一起用。")));
    cl->addWidget(makeIntRow("triggerFireDuration", "单次按住时长 (ms, 0=长按)", 0, 2000, 5, 0,
        QString::fromUtf8(u8"连点模式下，每发按住左键多久。\n"
        u8"★ 0 = 长按模式：只要还在命中区就一直按着，离开才松手。\n"
        u8"★ 非 0 = 连点模式：按住这么久就松开，然后走一次冷却间隔。")));
    cl->addWidget(makeIntRow("triggerFireInterval", "连点冷却间隔 (ms)", 1, 2000, 5, 200,
        QString::fromUtf8(u8"两发之间的冷却。\n"
        u8"★ 连点模式：松开左键后等这么久才能再开火。\n"
        u8"★ 长按模式：离开命中区松手后等这么久。\n"
        u8"★ 不要设成 0 —— 那会在命中区里退化成每拍 press/release 的抖动。")));

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"抖动（破除机械感）")));
    cl->addWidget(makeIntRow("triggerDelayJitter", "开火延迟 ±抖动 (ms)", 0, 500, 1, 0,
        QString::fromUtf8(u8"给「进区后延迟开火」加一个 ±N ms 的随机抖动，\n"
        u8"让每枪的节奏不完全一致。0 = 不抖动。"))) ;
    cl->addWidget(makeIntRow("triggerDurationJitter", "按住时长 ±抖动 (ms)", 0, 500, 1, 0,
        QString::fromUtf8(u8"给「单次按住时长」加 ±N ms 随机抖动。0 = 不抖动。"))) ;
    cl->addWidget(makeIntRow("triggerIntervalJitter", "冷却间隔 ±抖动 (ms)", 0, 500, 1, 0,
        QString::fromUtf8(u8"给「连点冷却间隔」加 ±N ms 随机抖动。0 = 不抖动。"))) ;
    cl->addWidget(makeIntRow("triggerSwitchCooldown", "换目标冷却 (ms)", 0, 2000, 5, 0,
        QString::fromUtf8(u8"目标身份变化后，等这么久才允许再次开火。\n"
        u8"★ 0 = 不冷却，换目标立刻可以打。\n"
        u8"★ 只在「换目标且当前不在命中区」时生效 —— 转火后新目标就在准星上时\n"
        u8"  会立刻接力开火，不会为了冷却卡一下。")));

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"自动开镜")));
    auto* scopeCombo = new QComboBox;
    scopeCombo->setObjectName("triggerAutoScope");
    scopeCombo->addItem(QStringLiteral("关闭"), 0);
    scopeCombo->addItem(QStringLiteral("点按右键一下（不收镜）"), 1);
    scopeCombo->addItem(QStringLiteral("长按右键（按住开镜）"), 2);
    auto* scopeRow = FormKit::fieldRow(QStringLiteral("开火方式"), scopeCombo);
    attachTip(scopeRow, QString::fromUtf8(
        u8"仿 AimMagic 的「开火方式」。\n"
        u8"★ 点按：每次接敌开始时点一下右键，之后就不再碰它 —— 不自动收镜，\n"
        u8"  开镜状态由你自己负责（实机反馈：自动收镜会和你的操作打架）。\n"
        u8"★ 长按：命中区里一直按住，离开时松开。\n"
        u8"★ 如果你的热键本身绑了右键，这一项一律不生效（否则会把镜切回去）。"));
    cl->addWidget(scopeRow);

    cl->addWidget(makeIntRow("triggerScopeDelay", "开镜后等多久才开火 (ms)", 0, 2000, 5, 0,
        QString::fromUtf8(u8"按下右键之后等这么多毫秒才允许开左键 ——\n"
        u8"保证第一颗子弹是【开着镜】打出去的。\n"
        u8"★ 0 = 不等（同一拍就开火，可能第一枪还没进镜）。\n"
        u8"★ 本项目有意与 AimMagic 不同：AM 是同一拍先左键再右键，\n"
        u8"  那第一枪其实没开镜。")));

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"开镜后的瞄准参数")));
    auto* scopeCtlCombo = new QComboBox;
    scopeCtlCombo->setObjectName("scopeCtlMode");
    scopeCtlCombo->addItem(QStringLiteral("跟随热键（开镜前后同一套参数）"), 0);
    scopeCtlCombo->addItem(QStringLiteral("用「开镜独立瞄准参数」那一套"), 1);
    auto* scopeCtlRow = FormKit::fieldRow(QStringLiteral("开镜期间"), scopeCtlCombo);
    attachTip(scopeCtlRow, QString::fromUtf8(
        u8"自动开镜真的按下右键之后, 只要你还按着热键, 瞄准控制器就改用"
        u8"「开镜独立瞄准参数」卡里的【整组参数】; 松开热键后切回热键自己的那套。\n"
        u8"★ 长按开镜: 离开命中区会自动收镜 ⇒ 那一刻就切回(镜都收了, 参数也该回去)。\n"
        u8"★ 点按开镜(不收镜): 镜头一直开着 ⇒ 一直粘到松开热键为止。\n"
        u8"★ 为什么需要它: 开镜后游戏内灵敏度被【倍率】放大 —— 镜前调好的增益\n"
        u8"  直接用在镜内必然过冲。瞬狙时「镜前一甩、进镜就飞」就是这个原因。\n"
        u8"★ 跟随热键(默认) = 逐拍与没有这个功能时完全一致。\n"
        u8"★ 热键本身绑了右键时本项不生效(自动开镜那时根本不会按下右键)。"));
    cl->addWidget(scopeCtlRow);
    m_scopeModeCombo = scopeCtlCombo;

    cl->addWidget(makeSectionTitle(QString::fromUtf8(u8"自动急停")));
    auto* stopCombo = new QComboBox;
    stopCombo->setObjectName("triggerAutoStop");
    stopCombo->addItem(QStringLiteral("关闭"), 0);
    stopCombo->addItem(QStringLiteral("开启（开火时屏蔽真实键盘）"), 1);
    auto* stopRow = FormKit::fieldRow(QStringLiteral("开关"), stopCombo);
    attachTip(stopRow, QString::fromUtf8(
        u8"开火那一拍，把【真实键盘输入】整段屏蔽掉一段时间。\n"
        u8"屏蔽期间你按的 W/A/S/D 不会进入被控机，角色凭游戏自身的停止行为停住，\n"
        u8"不注入任何按键 —— 没有任何残余反向位移，也不干扰你的真实操作。\n"
        u8"★ 只从【接键盘那台硬件】下发屏蔽命令，绝不落到鼠标硬件上，\n"
        u8"  否则会连带把真实鼠标输入一起屏蔽。\n"
        u8"★ 需要接键盘硬件（MAKCU + 键盘板 / MAKCUNEW / KMBOXNET）；\n"
        u8"  没接的输入方式自动跳过，不影响鼠标的任何行为。"));
    cl->addWidget(stopRow);

    cl->addWidget(makeIntRow("triggerStopMs", "急停屏蔽键盘时长 (ms)", 20, 300, 5, 60,
        QString::fromUtf8(u8"屏蔽真实键盘多久。\n"
        u8"★ 固件侧带硬超时自解除 —— 就算上位机崩了，时间一到输入也会自己回来。\n"
        u8"★ 太短：停不下来；太长：屏蔽期间你会觉得键盘没反应。范围 20~300。")));

    card->setToolTip(QString::fromUtf8(
        u8"★ 扳机用的是【以框为基准】的命中区，和瞄点解耦 —— 换瞄点（胸口/头部）"
        u8"不会改变触发几何。\n"
        u8"★ 判定输入是【原始准星】，不是平滑过的值。"));

    auto commit = [this, enable, scopeCombo, stopCombo, scopeCtlCombo]() {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        HotkeyProfile& hp = config.hotkeys[ri];

        auto i = [this](const char* n) { return findChild<QSpinBox*>(n)->value(); };

        hp.trigger_enabled            = enable->isChecked();
        hp.trigger_y_percent          = i("triggerYPercent");
        hp.trigger_fire_delay         = i("triggerFireDelay");
        hp.trigger_fire_duration      = i("triggerFireDuration");
        hp.trigger_fire_interval      = i("triggerFireInterval");
        hp.trigger_delay_jitter_ms    = i("triggerDelayJitter");
        hp.trigger_duration_jitter_ms = i("triggerDurationJitter");
        hp.trigger_interval_jitter_ms = i("triggerIntervalJitter");
        hp.trigger_switch_cooldown_ms = i("triggerSwitchCooldown");
        hp.trigger_auto_scope         = scopeCombo->currentData().toInt();
        hp.trigger_scope_delay_ms     = i("triggerScopeDelay");
        hp.trigger_auto_stop          = stopCombo->currentData().toInt();
        hp.trigger_stop_ms            = i("triggerStopMs");
        // 开镜期间是否用独立那一套 (与「开镜独立瞄准参数」卡联动显隐)。
        hp.scope_ctl_enabled          = scopeCtlCombo->currentData().toInt();

        ConfigBridge::instance().markDirty();
        applyScopeCtlVisibility();
    };

    connect(enable, &QCheckBox::toggled, this, [commit](bool) { commit(); });
    connect(scopeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [commit](int) { commit(); });
    connect(stopCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [commit](int) { commit(); });
    connect(scopeCtlCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [commit](int) { commit(); });
    for (auto* sp : m_triggerInts)
        connect(sp, QOverload<int>::of(&QSpinBox::valueChanged), this, [commit](int) { commit(); });

    m_rightLayout->addWidget(card);
}

namespace
{

// ── 开镜档的行表 ────────────────────────────────────────────────────────────
//
// ★ 说明文字不在这里重写一遍: 建行时按 mainObj 去「瞄准控制器」卡里取那个控件
//   已有的 tooltip(见 buildScopeCtlCard 的 tipFromMainCard)。文案只有一份,
//   两张卡的说明不会各说各话。
// ★ 量程/步长/默认值必须与「瞄准控制器」卡【逐条一致】—— 这里列的只是数值。
// ★ decimals == 0 ⇒ 整数行(QSpinBox); > 0 ⇒ 浮点行(QDoubleSpinBox)。
struct ScopeRowDesc
{
    const char* mainObj;    // 「瞄准控制器」卡里的控件名 (取说明用)
    const char* scopeObj;   // 本卡控件名
    const char* label;      // 行标签 (FormKit 标签宽 88, 太长会被截)
    double lo, hi, step, def;
    int    decimals;
};

const ScopeRowDesc kScopeGainRows[] = {
    { "ctlKpX", "scopeKpX", "Kp · 水平", 0.0,   500.0, 0.5,   35.0, 3 },
    { "ctlKpY", "scopeKpY", "Kp · 垂直", 0.0,   500.0, 0.5,   35.0, 3 },
    { "ctlKiX", "scopeKiX", "Ki · 水平", 0.0,   100.0, 0.01,   0.0, 3 },
    { "ctlKiY", "scopeKiY", "Ki · 垂直", 0.0,   100.0, 0.01,   0.0, 3 },
    { "ctlKdX", "scopeKdX", "Kd · 水平", 0.0,   100.0, 0.01,   0.0, 3 },
    { "ctlKdY", "scopeKdY", "Kd · 垂直", 0.0,   100.0, 0.01,   0.0, 3 },
    { "ctlPFullScalePx", "scopePFullScalePx", "P 项饱和 (像素, 0=不限)",
      0.0, 2000.0, 1.0, 0.0, 3 },
    { "ctlTauUnwindSec", "scopeTauUnwindSec", "积分回吐时间常数 (秒)",
      0.001, 5.0, 0.005, 0.030, 3 },
    { "ctlTauDerivSec", "scopeTauDerivSec", "D 项低通时间常数 (秒)",
      0.0, 5.0, 0.005, 0.020, 3 },
    { "ctlIMax", "scopeIMax", "积分上限 (0=用输出限幅)", 0.0, 5000.0, 1.0, 0.0, 3 },
};

const ScopeRowDesc kScopeLimitRows[] = {
    { "ctlMaxOutputCounts", "scopeMaxOutputCounts", "单拍最大位移 (计数)",
      1.0, 1000.0, 1.0, 200.0, 0 },
    { "ctlRandomSeed", "scopeRandomSeed", "瞄点随机种子 (0=固定)",
      0.0, 999999.0, 1.0, 0.0, 0 },
};

const ScopeRowDesc kScopeSensitivityRows[] = {
    { "ctlKPxPerCount", "scopeKPxPerCount", "灵敏度折算系数 k (像素/计数, 0=关)",
      0.0, 10.0, 0.005, 0.0, 4 },
};

const ScopeRowDesc kScopePredictRows[] = {
    { "ctlPredictLeadMs", "scopePredictLeadMs", "预测提前时间 (毫秒, 0=关闭)",
      0.0, 1000.0, 1.0, 0.0, 3 },
    { "ctlPredictMaxVelocityPxPerSec", "scopePredictMaxVelocityPxPerSec",
      "速度上限 (像素/秒, 0=不限)", 0.0, 100000.0, 10.0, 0.0, 3 },
    { "ctlPredictMaxLeadRatio", "scopePredictMaxLeadRatio",
      "预测距离上限 (框对角线倍数, 0=不限)", 0.0, 100.0, 0.05, 0.0, 3 },
};

const ScopeRowDesc kScopeInflightRows[] = {
    { "ctlInflightBeta", "scopeInflightBeta", "在途补偿强度 (无量纲, 0=关闭)",
      0.0, 3.0, 0.05, 1.6, 3 },
};


}

// ── 开镜档: 自动开镜生效期间取代「瞄准控制器」的整组参数 ────────────────────
//
// ★ 为什么需要它: 开镜后游戏内灵敏度被倍率放大, 镜前调好的一套增益与灵敏度
//   折算填进镜内必然过冲(瞬狙最典型: 镜前一甩、进镜就飞)。这里给镜内一套独立
//   参数, 且只在【自动开镜真的按下了右键、并且你还按着热键】的期间生效 ——
//   判定在 runtime/aim_loop.cpp。
// ★ 开关是「自动扳机 → 自动开镜 → 开镜期间」那个下拉框。选「跟随热键」时这
//   一整套不生效, 逐拍与没有这个功能时完全一致。
void AimSettingsPage::buildScopeCtlCard()
{
    auto* card = new CardWidget(QString::fromUtf8(u8"开镜独立瞄准参数"),
                                QStringLiteral("adjustments"));
    auto* cl = card->contentLayout();

    card->setToolTip(QString::fromUtf8(
        u8"这里的参数【整组取代】「瞄准控制器」卡里的同名参数, 只在「自动扳机 → "
        u8"自动开镜」真的按下右键、并且你还按着热键的那几拍生效。\n"
        u8"★ 开关在「自动扳机 → 自动开镜 → 开镜期间」。\n"
        u8"★ 量程与默认值跟「瞄准控制器」卡逐条一致; 说明文字直接取那张卡的, 只有一份。\n"
        u8"★ 选靶 / 稳定器 / 滞回倍数 / 瞄点 Y 这些【不属于控制器增益】的参数仍然"
        u8"只有热键一份, 不随开镜切档 —— 它们管的是「瞄谁」, 不是「用多大力」。"));

    m_scopeOffHint = makeHint(QString::fromUtf8(
        u8"当前是「跟随热键」：开镜前后用同一套参数, 下面这些【不生效】(已置灰)。\n"
        u8"把「自动扳机 → 自动开镜 → 开镜期间」切到「用「开镜独立瞄准参数」那一套」"
        u8"就会启用。"));
    cl->addWidget(m_scopeOffHint);

    // ── 一键复制 ─────────────────────────────────────────────────────────
    auto* copyTitle = makeSectionTitle(QString::fromUtf8(u8"一键复制"));
    cl->addWidget(copyTitle);
    m_scopeParamRows.push_back(copyTitle);

    m_scopeCopyCombo = new QComboBox;
    m_scopeCopyCombo->setObjectName("scopeCopySource");
    auto* copyBtn = new QPushButton(QString::fromUtf8(u8"复制它的默认参数"));
    copyBtn->setObjectName("scopeCopyBtn");
    copyBtn->setCursor(Qt::PointingHandCursor);

    auto* copyRow = new QWidget;
    {
        auto* hl = new QHBoxLayout(copyRow);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(10);
        hl->addWidget(m_scopeCopyCombo, 1);
        hl->addWidget(copyBtn);
    }
    attachTip(copyRow, QString::fromUtf8(
        u8"把【指定热键的默认(未开镜)瞄准控制器参数】整套搬进本热键的开镜档。\n"
        u8"★ 搬的是对方的【默认档】, 不是对方的开镜档 —— 所以可以先把某个热键在"
        u8"镜内调好当模板, 再从别的热键一键搬过来。\n"
        u8"★ 源热键选自己 = 把本热键的开镜档重置回自己的默认参数。"));
    cl->addWidget(copyRow);
    m_scopeParamRows.push_back(copyRow);

    m_scopeCopyHint = makeHint(QString());
    cl->addWidget(m_scopeCopyHint);

    // 说明文字一律从主卡取: 主卡先建(buildRightPanel 的顺序), 控件已存在。
    auto tipFromMainCard = [this](const char* obj) -> QString {
        auto* w = findChild<QWidget*>(QString::fromUtf8(obj));
        if (!w) return QString();
        if (!w->toolTip().isEmpty()) return w->toolTip();
        // 少数行(灵敏度折算系数 k —— 它那行是手搓的, 说明挂在标签上而不是控件上)
        // 的说明在行内第一个标签里, 取它, 免得这里悄悄少一段说明。
        if (QWidget* row = w->parentWidget())
            if (auto* l = row->findChild<QLabel*>())
                return l->toolTip();
        return QString();
    };

    auto addGroup = [&](const char* title, const ScopeRowDesc* rows, size_t count) {
        auto* t = makeSectionTitle(QString::fromUtf8(title));
        cl->addWidget(t);
        m_scopeParamRows.push_back(t);

        for (size_t i = 0; i < count; ++i)
        {
            const ScopeRowDesc& d = rows[i];
            QWidget* row = nullptr;

            if (d.decimals <= 0)
            {
                auto* sp = new NoWheelSpinBox;
                sp->setRange(static_cast<int>(d.lo), static_cast<int>(d.hi));
                sp->setSingleStep(static_cast<int>(d.step));
                sp->setObjectName(QString::fromUtf8(d.scopeObj));
                sp->setValue(static_cast<int>(d.def));
                m_scopeInts.push_back(sp);
                row = FormKit::fieldRow(QString::fromUtf8(d.label), sp);
            }
            else
            {
                auto* sp = new NoWheelDoubleSpinBox;
                sp->setRange(d.lo, d.hi);
                sp->setSingleStep(d.step);
                sp->setDecimals(d.decimals);
                sp->setObjectName(QString::fromUtf8(d.scopeObj));
                sp->setValue(d.def);
                m_scopeDoubles.push_back(sp);
                row = FormKit::fieldRow(QString::fromUtf8(d.label), sp);
            }

            attachTip(row, tipFromMainCard(d.mainObj));
            cl->addWidget(row);
            m_scopeParamRows.push_back(row);
        }
    };

    addGroup("增益（水平 x = 跟枪 / 垂直 y = 压枪）",
             kScopeGainRows, std::size(kScopeGainRows));
    addGroup("输出限幅与随机化", kScopeLimitRows, std::size(kScopeLimitRows));
    addGroup("灵敏度折算系数 (修正预测速度)",
             kScopeSensitivityRows, std::size(kScopeSensitivityRows));
    addGroup("在途补偿 (预测提前量)", kScopePredictRows, std::size(kScopePredictRows));
    addGroup("在途自身位移补偿 (Smith)", kScopeInflightRows, std::size(kScopeInflightRows));

    auto commitScope = [this]() {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        if (ri < 0) return;
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (ri >= static_cast<int>(config.hotkeys.size())) return;
        AimCtlParams& p = config.hotkeys[ri].ctl_scope;

        auto d = [this](const char* n) { return findChild<QDoubleSpinBox*>(n)->value(); };
        auto i = [this](const char* n) { return findChild<QSpinBox*>(n)->value(); };

        p.kp_x             = d("scopeKpX");
        p.kp_y             = d("scopeKpY");
        p.ki_x             = d("scopeKiX");
        p.ki_y             = d("scopeKiY");
        p.kd_x             = d("scopeKdX");
        p.kd_y             = d("scopeKdY");
        p.tau_unwind_sec   = d("scopeTauUnwindSec");
        p.tau_deriv_sec    = d("scopeTauDerivSec");
        p.i_max            = d("scopeIMax");
        p.max_output_counts= i("scopeMaxOutputCounts");
        p.p_full_scale_px  = d("scopePFullScalePx");
        p.predict_lead_ms  = d("scopePredictLeadMs");
        p.predict_max_velocity_px_s = d("scopePredictMaxVelocityPxPerSec");
        p.predict_max_lead_ratio    = d("scopePredictMaxLeadRatio");
        p.k_px_per_count   = d("scopeKPxPerCount");
        p.inflight_beta    = d("scopeInflightBeta");
        p.random_seed      = i("scopeRandomSeed");

        ConfigBridge::instance().markDirty();
    };

    for (auto* sp : m_scopeDoubles)
        connect(sp, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [commitScope](double) { commitScope(); });
    for (auto* sp : m_scopeInts)
        connect(sp, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [commitScope](int) { commitScope(); });

    connect(copyBtn, &QPushButton::clicked, this, [this]() {
        if (m_loading) return;
        const int ri = currentRuntimeIndex();
        const int src = m_scopeCopyCombo ? m_scopeCopyCombo->currentData().toInt() : -1;
        if (ri < 0 || src < 0) return;

        QString srcLabel;
        QString selfLabel;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            const int n = static_cast<int>(config.hotkeys.size());
            if (ri >= n || src >= n) return;
            // ★ 复制的是【源热键的默认档】, 不是它的开镜档。
            config.hotkeys[ri].ctl_scope = ctlParamsOf(config.hotkeys[src]);
            srcLabel  = QString::fromUtf8(config.hotkeys[src].name.c_str());
            selfLabel = QString::fromUtf8(config.hotkeys[ri].name.c_str());
        }
        ConfigBridge::instance().markDirty();
        reloadProfileToUi();   // 把新值回填到控件

        if (m_scopeCopyHint)
            m_scopeCopyHint->setText(QString::fromUtf8(
                u8"✅ 已把「%1」的默认瞄准控制器参数整套复制到「%2」的开镜档。")
                .arg(srcLabel, selfLabel));
    });

    m_rightLayout->addWidget(card);

    rebuildScopeCopyCombo();
    applyScopeCtlVisibility();
}

// 开镜档参数是否生效: 「开镜期间」选了「用独立那一套」。
// ★ 用置灰而不是隐藏 —— 用户还能看见自己填进去的值, 只是明确"现在不生效"。
void AimSettingsPage::applyScopeCtlVisibility()
{
    const bool on = m_scopeModeCombo && m_scopeModeCombo->currentData().toInt() != 0;
    for (auto* w : m_scopeParamRows)
        if (w) w->setEnabled(on);
    if (m_scopeOffHint) m_scopeOffHint->setVisible(!on);
}

// 一键复制的来源下拉框: 列出【所有】热键(带组名与按键), 不限于当前组 ——
// 镜内参数往往是在另一个组的某个热键上调出来的。
void AimSettingsPage::rebuildScopeCopyCombo()
{
    if (!m_scopeCopyCombo) return;

    const int prev = m_scopeCopyCombo->currentData().isValid()
                         ? m_scopeCopyCombo->currentData().toInt()
                         : -1;

    m_scopeCopyCombo->blockSignals(true);
    m_scopeCopyCombo->clear();
    bool prevKept = false;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        for (int i = 0; i < static_cast<int>(config.hotkeys.size()); ++i)
        {
            const auto& hp = config.hotkeys[i];
            QString text = QString::fromUtf8(hp.group.c_str()) + QStringLiteral(" / ") +
                           QString::fromUtf8(hp.name.c_str());
            if (!hp.keys.empty() && hp.keys.front() != "None")
                text += QStringLiteral("  [") +
                        QString::fromUtf8(hp.keys.front().c_str()) + QStringLiteral("]");
            m_scopeCopyCombo->addItem(text, i);
            if (i == prev) prevKept = true;
        }
    }

    if (prevKept)
    {
        const int k = m_scopeCopyCombo->findData(prev);
        if (k >= 0) m_scopeCopyCombo->setCurrentIndex(k);
    }
    else
    {
        // 默认挑【不是本热键】的第一个 —— 一键复制的常见用法是"从别的热键搬"。
        const int self = currentRuntimeIndex();
        int pick = -1;
        for (int i = 0; i < m_scopeCopyCombo->count(); ++i)
        {
            if (m_scopeCopyCombo->itemData(i).toInt() != self) { pick = i; break; }
        }
        if (pick < 0 && m_scopeCopyCombo->count() > 0) pick = 0;
        if (pick >= 0) m_scopeCopyCombo->setCurrentIndex(pick);
    }
    m_scopeCopyCombo->blockSignals(false);
}

void AimSettingsPage::buildTrajectoryCard()
{
    auto* card = new CardWidget(QString::fromUtf8(u8"轨迹曲线"),
                                QStringLiteral("vector-spline"));
    auto* cl = card->contentLayout();
    card->setToolTip(QString::fromUtf8(
        u8"轨迹只有在【瞄准控制器开启】时才有意义 —— 它整形的是控制器算出来的位移。\n"
        u8"四种模式都只旋转不缩放：每拍走多远仍由 PID 决定，曲线只决定往哪个方向走。"));

    auto* modeCombo = new QComboBox;
    modeCombo->setObjectName("aimPathMode");
    modeCombo->addItem(QStringLiteral("直线（透传，不整形）"), 0);
    modeCombo->addItem(QStringLiteral("贝塞尔曲线"), 1);
    modeCombo->addItem(QStringLiteral("自定义手绘"), 2);
    modeCombo->addItem(QStringLiteral("WindMouse（风力曲线）"), 3);
    auto* modeRow = FormKit::fieldRow(QStringLiteral("轨迹模式"), modeCombo);
    attachTip(modeRow, QString::fromUtf8(
        u8"移动轨迹的整形方式。\n"
        u8"★ 直线 = 完全透传控制器输出，与不开这个功能逐位一致。\n"
        u8"★ 其余三种都【只旋转不缩放】：曲线只决定「往哪个方向走」，\n"
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
    const QString cur = m_groupCombo->currentText();
    m_groupCombo->blockSignals(true);
    m_groupCombo->clear();
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
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
}

void AimSettingsPage::rebuildProfileList()
{
    if (!m_profileList) return;
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
                if (!keys.isEmpty()) keys += QStringLiteral(" / ");
                keys += QString::fromUtf8(k.c_str());
            }
            if (keys.isEmpty()) keys = QStringLiteral("None");

            auto* item = new QListWidgetItem(m_profileList);
            item->setData(Qt::UserRole, i);

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
        m_profileList->setCurrentRow(0);
        onProfileSelected(0);
    }
    restyleProfileItems();
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
            n->setStyleSheet(sel ? "color:#4A55C8; font-size:13px; font-weight:500;"
                                 : "color:#3C3C44; font-size:13px;");
        if (auto* k = w->findChild<QLabel*>("pkey"))
            k->setStyleSheet(sel ? "color:#7E88D8; font-size:11px;"
                                 : "color:#A1A1AA; font-size:11px;");
    }
}

void AimSettingsPage::onGroupChanged(int)
{
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
            if (auto* s = findChild<QSpinBox*>("fovX")) s->setValue(hp.fovX);
            if (auto* s = findChild<QSpinBox*>("fovY")) s->setValue(hp.fovY);
            if (auto* c = findChild<QCheckBox*>("crosshairChk"))
                c->setChecked(hp.crosshair_detect_enabled);
            if (auto* c = findChild<QCheckBox*>("dynFovChk"))
                c->setChecked(hp.dynamic_fov_enabled);
            if (auto* s = findChild<QDoubleSpinBox*>("dynFovStrength"))
                s->setValue(hp.dynamic_fov_strength);

            if (auto* c = findChild<QCheckBox*>("ctlEnabled")) c->setChecked(hp.ctl_enabled);

            auto sd = [this](const char* n, double v) {
                if (auto* s = findChild<QDoubleSpinBox*>(n)) s->setValue(v);
            };
            auto si = [this](const char* n, int v) {
                if (auto* s = findChild<QSpinBox*>(n)) s->setValue(v);
            };
            sd("ctlKpX", hp.ctl_kp_x);
            sd("ctlKpY", hp.ctl_kp_y);
            sd("ctlKiX", hp.ctl_ki_x);
            sd("ctlKiY", hp.ctl_ki_y);
            sd("ctlKdX", hp.ctl_kd_x);
            sd("ctlKdY", hp.ctl_kd_y);
            sd("ctlTauUnwindSec", hp.ctl_tau_unwind_sec);
            sd("ctlTauDerivSec", hp.ctl_tau_deriv_sec);
            sd("ctlIMax", hp.ctl_i_max);
            sd("ctlPFullScalePx", hp.ctl_p_full_scale_px);
            sd("ctlPredictLeadMs", hp.ctl_predict_lead_ms);
            sd("ctlPredictMaxVelocityPxPerSec", hp.ctl_predict_max_velocity_px_s);
            sd("ctlPredictMaxLeadRatio", hp.ctl_predict_max_lead_ratio);
            sd("ctlKPxPerCount", hp.ctl_k_px_per_count);
            sd("ctlInflightBeta", hp.ctl_inflight_beta);
            si("ctlMaxOutputCounts", hp.ctl_max_output_counts);
            si("ctlRandomSeed", hp.ctl_random_seed);

            if (auto* c = findChild<QCheckBox*>("triggerEnabled"))
                c->setChecked(hp.trigger_enabled);
            if (auto* c = findChild<QComboBox*>("triggerAutoScope"))
            {
                const int k = c->findData(hp.trigger_auto_scope);
                c->setCurrentIndex(k >= 0 ? k : 0);
            }
            if (auto* c = findChild<QComboBox*>("triggerAutoStop"))
            {
                const int k = c->findData(hp.trigger_auto_stop > 0 ? 1 : 0);
                c->setCurrentIndex(k >= 0 ? k : 0);
            }
            si("triggerYPercent",         hp.trigger_y_percent);
            si("triggerFireDelay",        hp.trigger_fire_delay);
            si("triggerFireDuration",     hp.trigger_fire_duration);
            si("triggerFireInterval",     hp.trigger_fire_interval);
            si("triggerDelayJitter",      hp.trigger_delay_jitter_ms);
            si("triggerDurationJitter",   hp.trigger_duration_jitter_ms);
            si("triggerIntervalJitter",   hp.trigger_interval_jitter_ms);
            si("triggerSwitchCooldown",   hp.trigger_switch_cooldown_ms);
            si("triggerScopeDelay",       hp.trigger_scope_delay_ms);
            si("triggerStopMs",           hp.trigger_stop_ms);

            // 开镜期间: 跟随热键 / 用独立那一套。
            if (auto* c = findChild<QComboBox*>("scopeCtlMode"))
            {
                const int k = c->findData(hp.scope_ctl_enabled != 0 ? 1 : 0);
                c->setCurrentIndex(k >= 0 ? k : 0);
            }
            // 开镜档的 18 项 (与「瞄准控制器」卡同一组参数)。
            sd("scopeKpX",             hp.ctl_scope.kp_x);
            sd("scopeKpY",             hp.ctl_scope.kp_y);
            sd("scopeKiX",             hp.ctl_scope.ki_x);
            sd("scopeKiY",             hp.ctl_scope.ki_y);
            sd("scopeKdX",             hp.ctl_scope.kd_x);
            sd("scopeKdY",             hp.ctl_scope.kd_y);
            sd("scopeTauUnwindSec",    hp.ctl_scope.tau_unwind_sec);
            sd("scopeTauDerivSec",     hp.ctl_scope.tau_deriv_sec);
            sd("scopeIMax",            hp.ctl_scope.i_max);
            sd("scopePFullScalePx",    hp.ctl_scope.p_full_scale_px);
            sd("scopePredictLeadMs",   hp.ctl_scope.predict_lead_ms);
            sd("scopePredictMaxVelocityPxPerSec", hp.ctl_scope.predict_max_velocity_px_s);
            sd("scopePredictMaxLeadRatio",        hp.ctl_scope.predict_max_lead_ratio);
            sd("scopeKPxPerCount",     hp.ctl_scope.k_px_per_count);
            sd("scopeInflightBeta",    hp.ctl_scope.inflight_beta);
            si("scopeMaxOutputCounts", hp.ctl_scope.max_output_counts);
            si("scopeRandomSeed",      hp.ctl_scope.random_seed);

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

            rebuildAimClassRows();
        }
    }

    m_loading = false;

    // 开镜档: 来源下拉框(所有热键) + 生效与否的置灰/提示。
    // ★ 放在锁外: 两者都会自己取锁 (configMutex 是递归锁, 但没必要套着)。
    rebuildScopeCopyCombo();
    if (m_scopeCopyHint) m_scopeCopyHint->clear();
    applyScopeCtlVisibility();
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
        HotkeyProfile copy = config.hotkeys[ri];
        copy.name += " 副本";
        config.hotkeys.push_back(std::move(copy));
    }
    ConfigBridge::instance().markDirty();
    reloadFromRuntime();
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
