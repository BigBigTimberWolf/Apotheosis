#include "pages/AutoFlashPage.h"

#include "Apotheosis.h"
#include "config/ConfigManager.h"
#include "config/config_bridge.h"
#include "macro/macro_config.h"
#include "runtime/aim_telemetry.h"
#include "runtime/auto_flash_rules.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>

namespace {

// 行控件里存的是真实类别 id；这里给出“可用类别”列表（模型类别 + 已保存规则里
// 用到的类别 + 各热键瞄点/扳机引用过的类别），保证没加载模型时也能编辑。
std::vector<std::pair<int, QString>> availableFlashClasses()
{
    std::vector<std::pair<int, QString>> out;
    auto push = [&out](int id, const QString& name) {
        if (id < 0) return;
        for (const auto& entry : out) if (entry.first == id) return;
        out.emplace_back(id, name);
    };
    std::lock_guard<std::recursive_mutex> lock(configMutex);
    for (const auto& cf : config.class_filters)
        push(cf.class_id, QString::fromStdString(cf.class_name));
    for (const auto& rule : config.auto_flash_rules)
        if (rule.class_id >= 0) push(rule.class_id, QStringLiteral("已保存类别"));
    for (const auto& hk : config.hotkeys) {
        for (const auto& c : hk.aim_classes) push(c.class_id, QStringLiteral("瞄准类别"));
        for (const auto& c : hk.trigger_classes) push(c.class_id, QStringLiteral("扳机类别"));
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

} // namespace

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
        "下面可以给每个瞄准类别单独设阈值；没有单独设置的类别用「框面积占检测画面」这个全局值。"
        "输出可以选键盘键、鼠标键，也可以选“滚轮 · 上/下”——滚轮是滚一格，"
        "没有按下/松开两个阶段，不会卡在按住状态。"
        "目标持续大于阈值不会连续按；缩小到阈值以下或锁定新目标后可再次触发。"
        "滚轮读取只能靠本机钩子（被控端盒子不上报物理滚轮），所以滚轮热键只在鼠标接本机或输入方式为 WINDOWS 时有效。");
    card->setToolTip(hint);
    enabled_->setToolTip(hint);

    area_ = new QDoubleSpinBox;
    area_->setRange(0.1, 100.0);
    area_->setDecimals(1);
    area_->setSingleStep(0.5);
    area_->setSuffix(QStringLiteral(" %"));
    area_->setObjectName("autoFlashArea");
    area_->setToolTip(QStringLiteral("没有单独设置的类别都用这个阈值（旧配置里的那一项就是它）。"));
    card->contentLayout()->addWidget(FormKit::fieldRow(
        QStringLiteral("框面积占检测画面"), area_));

    {
        auto* ruleCard = new CardWidget(QStringLiteral("按类别单独设阈值"), QStringLiteral("target"));
        const QString ruleHint = QStringLiteral(
            "每行一个瞄准类别：该类别锁定后按这一行的面积阈值触发，其它类别继续用上面的全局值。"
            "类别就是自瞄/扳机里用的检测类别。");
        ruleCard->setToolTip(ruleHint);
        ruleContainer_ = new QWidget;
        ruleLayout_ = new QVBoxLayout(ruleContainer_);
        ruleLayout_->setContentsMargins(0, 0, 0, 0);
        ruleLayout_->setSpacing(6);
        ruleCard->contentLayout()->addWidget(ruleContainer_);
        auto* addRule = new QPushButton(QStringLiteral("+ 添加类别阈值"));
        addRule->setObjectName("autoFlashRuleAdd");
        addRule->setCursor(Qt::PointingHandCursor);
        ruleCard->contentLayout()->addWidget(addRule);
        ruleSummary_ = new QLabel;
        ruleSummary_->setWordWrap(true);
        ruleSummary_->setStyleSheet(QStringLiteral("color:#ABA697;font-size:12px;"));
        ruleCard->contentLayout()->addWidget(ruleSummary_);
        connect(addRule, &QPushButton::clicked, this, [this] {
            std::set<int> used;
            for (auto* row : ruleRows_)
                if (auto* combo = row->findChild<QComboBox*>("autoFlashRuleClass"))
                    used.insert(combo->currentData().toInt());
            int pick = -1;
            for (const auto& entry : availableFlashClasses())
                if (!used.count(entry.first)) { pick = entry.first; break; }
            if (pick < 0) {
                ruleSummary_->setText(QStringLiteral("没有其它可选类别了：请在 AI 模型页确认类别列表，"
                    "或先删除一行。"));
                return;
            }
            addRuleRow(pick, area_->value());
            commitRules();
        });
        layout->addWidget(ruleCard);
    }

    key_ = new QComboBox;
    key_->addItem(QStringLiteral("请选择输出按键"), QString());
    const struct { const char* id; const char* label; } mouseKeys[] = {
        {"LeftMouseButton", "鼠标左键"}, {"RightMouseButton", "鼠标右键"},
        {"MiddleMouseButton", "鼠标中键"}, {"X1MouseButton", "鼠标侧键 4"},
        {"X2MouseButton", "鼠标侧键 5"}};
    for (const auto& mouse : mouseKeys)
        key_->addItem(QString::fromUtf8(mouse.label), QString::fromUtf8(mouse.id));
    // 滚轮爆闪：输出一次滚动（上/下各一格），没有“按下/松开”两阶段，
    // 因此不会出现只按下没松开而卡住的情况。
    for (const auto& wheel : macros::wheelKeys())
        key_->addItem(QString::fromUtf8(wheel.label), QString::fromLatin1(wheel.id));
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

QWidget* AutoFlashPage::addRuleRow(int classId, double areaPercent)
{
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* combo = new QComboBox;
    combo->setObjectName("autoFlashRuleClass");
    for (const auto& entry : availableFlashClasses())
        combo->addItem(QStringLiteral("[%1] %2").arg(entry.first).arg(entry.second), entry.first);
    if (const int index = combo->findData(classId); index >= 0) combo->setCurrentIndex(index);
    layout->addWidget(combo, 1);

    auto* spin = new QDoubleSpinBox;
    spin->setObjectName("autoFlashRuleArea");
    spin->setRange(0.1, 100.0);
    spin->setDecimals(1);
    spin->setSingleStep(0.5);
    spin->setSuffix(QStringLiteral(" %"));
    spin->setValue(areaPercent);
    layout->addWidget(spin);

    auto* remove = new QPushButton(QStringLiteral("删除"));
    remove->setObjectName("autoFlashRuleRemove");
    remove->setCursor(Qt::PointingHandCursor);
    layout->addWidget(remove);

    ruleLayout_->addWidget(row);
    ruleRows_.push_back(row);

    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { commitRules(); });
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) { commitRules(); });
    connect(remove, &QPushButton::clicked, this, [this, row] {
        ruleRows_.erase(std::remove(ruleRows_.begin(), ruleRows_.end(), row), ruleRows_.end());
        ruleLayout_->removeWidget(row);
        row->deleteLater();
        commitRules();
    });
    return row;
}

void AutoFlashPage::rebuildRuleRows()
{
    for (auto* row : ruleRows_) {
        ruleLayout_->removeWidget(row);
        row->deleteLater();
    }
    ruleRows_.clear();
    std::vector<runtime::AutoFlashRule> rules;
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        rules = config.auto_flash_rules;
    }
    for (const auto& rule : rules) {
        if (rule.class_id < 0) continue; // 兜底行由上面的全局阈值承担
        addRuleRow(rule.class_id, rule.area_percent);
    }
}

void AutoFlashPage::commitRules()
{
    std::vector<runtime::AutoFlashRule> rules;
    for (auto* row : ruleRows_) {
        auto* combo = row->findChild<QComboBox*>("autoFlashRuleClass");
        auto* spin = row->findChild<QDoubleSpinBox*>("autoFlashRuleArea");
        if (!combo || !spin) continue;
        const int classId = combo->currentData().toInt();
        if (std::any_of(rules.begin(), rules.end(),
                        [&](const runtime::AutoFlashRule& r) { return r.class_id == classId; }))
            continue; // 同一类别只留第一行
        rules.push_back({classId, spin->value()});
    }
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        config.auto_flash_rules = rules;
    }
    ConfigBridge::instance().markDirty();
    if (rules.empty()) {
        ruleSummary_->setText(QStringLiteral("未按类别分档：所有类别都用上面的全局阈值。"));
        return;
    }
    QStringList parts;
    for (const auto& rule : rules)
        parts << QStringLiteral("类别 %1 → %2%").arg(rule.class_id).arg(rule.area_percent, 0, 'f', 1);
    ruleSummary_->setText(QStringLiteral("已生效：%1；其它类别用全局阈值 %2%。")
        .arg(parts.join(QStringLiteral("、")))
        .arg(area_->value(), 0, 'f', 1));
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
    rebuildRuleRows();
    commitRules();
}

void AutoFlashPage::commit() {
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        config.auto_flash_enabled = enabled_->isChecked();
        config.auto_flash_area_percent = area_->value();
        config.auto_flash_key = key_->currentData().toString().toStdString();
    }
    ConfigBridge::instance().markDirty();
    commitRules(); // 全局值变了，摘要里的“其它类别用 X%”要跟着更新
}

void AutoFlashPage::refreshArea() {
    const auto state = runtime::readAimOverlay();
    const auto now = std::chrono::steady_clock::now();
    int resolution;
    double fallback;
    std::vector<runtime::AutoFlashRule> rules;
    {
        std::lock_guard<std::recursive_mutex> lock(configMutex);
        resolution = config.detection_resolution;
        fallback = config.auto_flash_area_percent;
        rules = config.auto_flash_rules;
    }
    if (!state.valid || !state.engaged || state.ts.time_since_epoch().count() == 0 ||
        now - state.ts > std::chrono::milliseconds(250) || resolution <= 0) {
        currentArea_->setText(QStringLiteral("当前锁定目标面积：—"));
        return;
    }
    const double pct = 100.0 * state.box.area() /
        (static_cast<double>(resolution) * resolution);
    // 顺便显示这一拍按哪个阈值判定，省得猜“为什么没闪”。
    const auto* rule = runtime::pickAutoFlashRule(rules, state.target_class_id);
    const double threshold = rule ? rule->area_percent : fallback;
    currentArea_->setText(QStringLiteral("当前锁定目标面积：%1 %（类别 %2 的触发阈值 %3 %）")
        .arg(pct, 0, 'f', 1).arg(state.target_class_id).arg(threshold, 0, 'f', 1));
}
