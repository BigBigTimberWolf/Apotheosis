#include "widgets/MacroConditionList.h"

#include "widgets/MacroSummary.h"
#include "widgets/MacroUiCommon.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <iterator>

namespace {

using macro_ui::q;
using macro_ui::utf8;

enum class Kind { Flag, Number, Text };

// The conditions a row can edit exactly. `textLabel` names an extra text field.
struct Compact
{
    const char* id;
    Kind kind;
    const char* textLabel;
    int decimals;
    int group; // add-menu group: 0 targets, 1 state, 2 other
};

const Compact kCompact[] = {
    {"target.exists", Kind::Flag, nullptr, 0, 0},
    {"target.count", Kind::Number, nullptr, 0, 0},
    {"target.class", Kind::Number, nullptr, 0, 0},
    {"target.confidence", Kind::Number, nullptr, 2, 0},
    {"target.max_size", Kind::Number, nullptr, 0, 0},
    {"target.max_height", Kind::Number, nullptr, 0, 0},
    {"target.max_width", Kind::Number, nullptr, 0, 0},
    {"target.min_height", Kind::Number, nullptr, 0, 0},
    {"target.min_width", Kind::Number, nullptr, 0, 0},
    {"target.mean_height", Kind::Number, nullptr, 0, 0},
    {"target.mean_width", Kind::Number, nullptr, 0, 0},
    {"target.area", Kind::Number, nullptr, 0, 0},
    {"target.ratio", Kind::Number, nullptr, 2, 0},
    {"target.distance", Kind::Number, nullptr, 0, 0},
    {"target.speed", Kind::Number, nullptr, 0, 0},
    {"target.fov", Kind::Flag, nullptr, 0, 0},
    {"target.visible", Kind::Flag, nullptr, 0, 0},
    {"target.locked", Kind::Flag, nullptr, 0, 0},
    {"aim.active", Kind::Flag, nullptr, 0, 1},
    {"trigger.active", Kind::Flag, nullptr, 0, 1},
    {"aim.key", Kind::Flag, nullptr, 0, 1},
    {"color.hit", Kind::Flag, nullptr, 0, 1},
    {"key", Kind::Flag, "按键 / 组合键", 0, 1},
    {"cooldown.ready", Kind::Flag, nullptr, 0, 1},
    {"variable", Kind::Number, "变量名", 2, 2},
    {"window.title", Kind::Text, "窗口标题", 0, 2},
    {"process.name", Kind::Text, "进程名", 0, 2},
};

const std::pair<const char*, const char*> kNumberOps[] = {
    {"==", "等于"}, {"!=", "不等于"}, {">", "大于"}, {">=", "大于等于"}, {"<", "小于"},
    {"<=", "小于等于"}, {"range", "区间"}};
const std::pair<const char*, const char*> kTextOps[] = {
    {"contains", "包含"}, {"text_eq", "相同"}, {"text_ne", "不同"}};

const Compact* findCompact(const std::string& id)
{
    for (const auto& entry : kCompact)
        if (id == entry.id) return &entry;
    return nullptr;
}

bool wantsClass(const std::string& id)
{
    return id.rfind("target.", 0) == 0 && id != "target.class";
}

bool noRegion(const macros::Condition& c)
{
    return std::all_of(c.region.begin(), c.region.end(), [](int v) { return v == 0; });
}

template <size_t N>
bool hasOp(const std::pair<const char*, const char*> (&ops)[N], const std::string& op)
{
    return std::any_of(std::begin(ops), std::end(ops), [&](const auto& o) { return op == o.first; });
}

// Can the row show and edit this condition without changing what it means?
bool representable(const macros::Condition& c, const Compact& info)
{
    if (c.parent != -1 || !noRegion(c)) return false;
    switch (info.kind) {
    case Kind::Flag: return c.comparison == "==" && (c.value == 0.0 || c.value == 1.0);
    case Kind::Number: return hasOp(kNumberOps, c.comparison);
    case Kind::Text: return hasOp(kTextOps, c.comparison);
    }
    return false;
}

macros::Condition makeCondition(const std::string& id)
{
    macros::Condition c;
    c.metric = id;
    c.parent = -1;
    c.classId = -1;
    const Compact* info = findCompact(id);
    if (!info) return c;
    switch (info->kind) {
    case Kind::Flag: c.comparison = "=="; c.value = 1; break;
    case Kind::Text: c.comparison = "contains"; c.value = 0; break;
    case Kind::Number:
        c.comparison = id == "target.class" || id == "variable" ? "==" : ">=";
        c.value = id == "target.count" ? 1 : id == "target.confidence" ? 0.5 : 0;
        c.upper = 0;
        break;
    }
    return c;
}

QToolButton* flatButton(const QString& text, const QString& tip)
{
    auto* button = new QToolButton;
    button->setText(text);
    button->setToolTip(tip);
    button->setAutoRaise(true);
    return button;
}

} // namespace

MacroConditionList::MacroConditionList(QWidget* parent)
    : CardWidget(QStringLiteral("② 执行条件 · 全部满足才会启动"), QStringLiteral("crosshair"), parent)
{
    auto* body = contentLayout();
    body->setSpacing(8);
    body->addWidget(macro_ui::hint(QStringLiteral(
        "触发后会逐条检查下面的条件，全部满足才执行动作。没有条件就直接执行。")));

    rows_ = new QVBoxLayout;
    rows_->setSpacing(6);
    body->addLayout(rows_);
    empty_ = macro_ui::hint(QStringLiteral("还没有额外条件。"));
    body->addWidget(empty_);
    warning_ = macro_ui::hint(QString());
    warning_->setStyleSheet(QStringLiteral("color:#E0A458;font-size:12px;"));
    warning_->hide();
    body->addWidget(warning_);

    add_ = new QPushButton(QStringLiteral("＋ 添加条件 ▾"));
    add_->setObjectName("macroAddCondition");
    add_->setStyleSheet(QStringLiteral("QPushButton::menu-indicator{image:none;width:0;}"));
    auto* menu = new QMenu(add_);
    const char* titles[] = {"目标", "状态与按键", "其他"};
    for (int group = 0; group < 3; ++group) {
        auto* submenu = menu->addMenu(QString::fromUtf8(titles[group]));
        for (const auto& entry : kCompact) {
            if (entry.group != group) continue;
            const std::string id = entry.id;
            auto* action = submenu->addAction(q(macros::label(macros::metrics(), id)));
            connect(action, &QAction::triggered, this, [this, id] { addCondition(id); });
        }
    }
    add_->setMenu(menu);
    body->addWidget(add_, 0, Qt::AlignLeft);

    // The older target limits, still honoured by the engine next to the list.
    target_ = new QCheckBox(QStringLiteral("只在检测到目标时运行（可限定类别、框高）"));
    target_->setObjectName("macroTargetOnly");
    body->addWidget(target_);
    classes_ = new QLineEdit;
    classes_->setObjectName("macroClasses");
    classes_->setMaxLength(256);
    classes_->setPlaceholderText(QStringLiteral("留空表示任意类别；多个 ID 用逗号分隔"));
    classes_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9,，\\s]*")), classes_));
    auto labelled = [](const QString& label, QWidget* control) {
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(24, 0, 0, 0);
        layout->addWidget(new QLabel(label));
        layout->addWidget(control, 1);
        return row;
    };
    classesRow_ = labelled(QStringLiteral("目标类别 ID"), classes_);
    body->addWidget(classesRow_);
    height_ = new QCheckBox(QStringLiteral("再限制目标框高度（占画面百分比）"));
    height_->setObjectName("macroHeightFilter");
    heightRow_ = height_;
    body->addWidget(heightRow_);
    auto* range = new QWidget;
    auto* rangeLayout = new QHBoxLayout(range);
    rangeLayout->setContentsMargins(0, 0, 0, 0);
    minHeight_ = macro_ui::spin(0, 100, QStringLiteral(" %"));
    maxHeight_ = macro_ui::spin(0, 100, QStringLiteral(" %"));
    rangeLayout->addWidget(minHeight_);
    rangeLayout->addWidget(new QLabel(QStringLiteral("至")));
    rangeLayout->addWidget(maxHeight_);
    rangeRow_ = labelled(QStringLiteral("框高范围"), range);
    body->addWidget(rangeRow_);
    body->addWidget(macro_ui::hint(QStringLiteral(
        "需要条件组（任意满足 / 取反）、找图、找色或指定区域？展开页面下方的“高级规则与调试”。")));

    connect(target_, &QCheckBox::toggled, this, [this] { readLimits(); });
    connect(height_, &QCheckBox::toggled, this, [this] { readLimits(); });
    connect(classes_, &QLineEdit::editingFinished, this, [this] { readLimits(); });
    for (auto* box : {minHeight_, maxHeight_})
        connect(box, &QSpinBox::editingFinished, this, [this] { readLimits(); });
}

void MacroConditionList::setProgram(const macros::Program& program)
{
    const bool same = program.conditions == program_.conditions;
    loading_ = true;
    program_ = program;
    target_->setChecked(program.targetOnly);
    height_->setChecked(program.heightFilter);
    QStringList ids;
    for (int id : program.classes) ids << QString::number(id);
    classes_->setText(ids.join(QStringLiteral(", ")));
    minHeight_->setValue(program.minHeightPercent);
    maxHeight_->setValue(program.maxHeightPercent);
    loading_ = false;
    warning_->hide();
    if (!same || rows_->count() == 0) rebuild();
    else {
        classesRow_->setVisible(program_.targetOnly);
        heightRow_->setVisible(program_.targetOnly);
        rangeRow_->setVisible(program_.targetOnly && program_.heightFilter);
        empty_->setVisible(program_.conditions.empty() && !program_.targetOnly);
    }
}

void MacroConditionList::rebuild()
{
    const bool wasLoading = loading_;
    loading_ = true;
    while (QLayoutItem* item = rows_->takeAt(0)) {
        if (QWidget* widget = item->widget()) { widget->hide(); widget->deleteLater(); }
        delete item;
    }
    for (int i = 0; i < static_cast<int>(program_.conditions.size()); ++i)
        rows_->addWidget(makeRow(i));
    empty_->setVisible(program_.conditions.empty() && !program_.targetOnly);
    add_->setEnabled(static_cast<int>(program_.conditions.size()) < macros::maxConditions);
    classesRow_->setVisible(program_.targetOnly);
    heightRow_->setVisible(program_.targetOnly);
    rangeRow_->setVisible(program_.targetOnly && program_.heightFilter);
    loading_ = wasLoading;
}

QWidget* MacroConditionList::makeRow(int index)
{
    const macros::Condition& c = program_.conditions[static_cast<size_t>(index)];
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* badge = new QLabel(QString::number(index + 1));
    badge->setFixedWidth(22);
    badge->setAlignment(Qt::AlignCenter);
    badge->setToolTip(QStringLiteral("条件编号：流程动作按这个编号引用条件"));
    badge->setStyleSheet(QStringLiteral("color:#ABA697;font-size:12px;"));
    layout->addWidget(badge);

    const Compact* info = findCompact(c.metric);
    if (!info || !representable(c, *info)) {
        auto* text = new QLabel(macro_summary::conditionText(c));
        text->setWordWrap(true);
        text->setToolTip(QStringLiteral("这个条件包含分组、区域或其他复杂设置，请在高级规则里编辑。"));
        layout->addWidget(text, 1);
        auto* edit = flatButton(QStringLiteral("在高级规则里编辑"), QStringLiteral("展开高级规则与调试"));
        connect(edit, &QToolButton::clicked, this, [this] { if (openAdvanced) openAdvanced(); });
        layout->addWidget(edit);
    } else {
        const QString name = QStringLiteral("macroCondition%1").arg(index);
        auto* metric = new QComboBox;
        metric->setObjectName(name + QStringLiteral("Metric"));
        metric->setMinimumWidth(200);
        for (const auto& entry : kCompact)
            metric->addItem(q(macros::label(macros::metrics(), entry.id)), QString::fromLatin1(entry.id));
        metric->setCurrentIndex(metric->findData(q(c.metric)));
        layout->addWidget(metric);
        connect(metric, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, index, metric] {
            if (loading_) return;
            program_.conditions[static_cast<size_t>(index)] =
                makeCondition(utf8(metric->currentData().toString()));
            rebuild();
            emitChanged();
        });

        const auto condition = [this, index]() -> macros::Condition& {
            return program_.conditions[static_cast<size_t>(index)];
        };
        if (info->kind == Kind::Flag) {
            auto* state = new QComboBox;
            state->setObjectName(name + QStringLiteral("State"));
            state->addItems({QStringLiteral("成立"), QStringLiteral("不成立")});
            state->setCurrentIndex(c.value == 0.0 ? 1 : 0);
            layout->addWidget(state);
            connect(state, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=, this] {
                if (loading_) return;
                condition().comparison = "==";
                condition().value = state->currentIndex() == 0 ? 1 : 0;
                emitChanged();
            });
        } else {
            auto* op = new QComboBox;
            op->setObjectName(name + QStringLiteral("Op"));
            const bool text = info->kind == Kind::Text;
            if (text) for (const auto& o : kTextOps) op->addItem(QString::fromUtf8(o.second), QString::fromLatin1(o.first));
            else for (const auto& o : kNumberOps) op->addItem(QString::fromUtf8(o.second), QString::fromLatin1(o.first));
            op->setCurrentIndex(std::max(0, op->findData(q(c.comparison))));
            layout->addWidget(op);
            if (!text) {
                auto* value = macro_ui::doubleSpin(-1e9, 1e9, info->decimals, 80);
                value->setObjectName(name + QStringLiteral("Value"));
                value->setValue(c.value);
                auto* upper = macro_ui::doubleSpin(-1e9, 1e9, info->decimals, 80);
                upper->setObjectName(name + QStringLiteral("Upper"));
                upper->setValue(c.upper);
                auto* upperLabel = new QLabel;
                const bool size = c.metric == "target.max_size";
                if (size) layout->addWidget(new QLabel(QStringLiteral("宽")));
                layout->addWidget(value);
                layout->addWidget(upperLabel);
                layout->addWidget(upper);
                const auto showUpper = [=, this] {
                    const bool range = op->currentData().toString() == QStringLiteral("range");
                    upperLabel->setText(size ? QStringLiteral("高") : QStringLiteral("到"));
                    upperLabel->setVisible(range || size);
                    upper->setVisible(range || size);
                };
                showUpper();
                connect(op, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=, this] {
                    showUpper();
                    if (loading_) return;
                    condition().comparison = utf8(op->currentData().toString());
                    emitChanged();
                });
                connect(value, &QDoubleSpinBox::editingFinished, this, [=, this] {
                    if (loading_) return;
                    condition().value = value->value();
                    emitChanged();
                });
                connect(upper, &QDoubleSpinBox::editingFinished, this, [=, this] {
                    if (loading_) return;
                    condition().upper = upper->value();
                    emitChanged();
                });
            } else {
                connect(op, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=, this] {
                    if (loading_) return;
                    condition().comparison = utf8(op->currentData().toString());
                    emitChanged();
                });
            }
        }

        if (wantsClass(c.metric)) {
            auto* cls = macro_ui::spin(-1, 99999, QString(), 90);
            cls->setObjectName(name + QStringLiteral("Class"));
            cls->setSpecialValueText(QStringLiteral("全部类别"));
            cls->setValue(c.classId);
            cls->setToolTip(QStringLiteral("只统计这个类别的目标；全部类别表示不区分。"));
            layout->addWidget(cls);
            connect(cls, &QSpinBox::editingFinished, this, [=, this] {
                if (loading_) return;
                condition().classId = cls->value();
                emitChanged();
            });
        }
        if (info->textLabel) {
            if (c.metric == "key") {
                auto* key = macro_ui::keyCombo(true, false);
                key->setEditable(true);
                key->setObjectName(name + QStringLiteral("Text"));
                key->setToolTip(QStringLiteral("可选单键，也可输入 LeftControl+U 这样的组合键。"));
                macro_ui::setKey(key, c.text);
                layout->addWidget(key, 1);
                const auto readKey = [=, this] {
                    if (loading_) return;
                    condition().text = macro_ui::currentKey(key);
                    emitChanged();
                };
                connect(key, QOverload<int>::of(&QComboBox::currentIndexChanged), this, readKey);
                connect(key->lineEdit(), &QLineEdit::editingFinished, this, readKey);
            } else {
                auto* edit = new QLineEdit(q(c.text));
                edit->setObjectName(name + QStringLiteral("Text"));
                edit->setMaxLength(4096);
                edit->setPlaceholderText(QString::fromUtf8(info->textLabel));
                layout->addWidget(edit, 1);
                connect(edit, &QLineEdit::editingFinished, this, [=, this] {
                    if (loading_) return;
                    condition().text = utf8(edit->text());
                    emitChanged();
                });
            }
        }
        layout->addStretch(1);
    }

    auto* remove = flatButton(QStringLiteral("✕"), QStringLiteral("删除这个条件"));
    remove->setObjectName(QStringLiteral("macroCondition%1Remove").arg(index));
    connect(remove, &QToolButton::clicked, this, [this, index] { removeRow(index); });
    layout->addWidget(remove);
    return row;
}

void MacroConditionList::addCondition(const std::string& metric)
{
    if (static_cast<int>(program_.conditions.size()) >= macros::maxConditions) return;
    program_.conditions.push_back(makeCondition(metric));
    warning_->hide();
    rebuild();
    emitChanged();
}

void MacroConditionList::removeRow(int index)
{
    if (!macro_ui::removeCondition(program_, index)) {
        warning_->setText(QStringLiteral("这个条件组里还有子条件，请先在高级规则里删除或移出它们。"));
        warning_->show();
        return;
    }
    warning_->hide();
    rebuild();
    emitChanged();
}

void MacroConditionList::readLimits()
{
    if (loading_) return;
    program_.targetOnly = target_->isChecked();
    program_.heightFilter = height_->isChecked();
    program_.minHeightPercent = minHeight_->value();
    program_.maxHeightPercent = maxHeight_->value();
    program_.classes.clear();
    for (const QString& part : classes_->text().split(QRegularExpression(QStringLiteral("[,，\\s]+")),
                                                      Qt::SkipEmptyParts)) {
        bool ok = false;
        const int id = part.toInt(&ok);
        if (ok && id >= 0) program_.classes.push_back(id);
    }
    classesRow_->setVisible(program_.targetOnly);
    heightRow_->setVisible(program_.targetOnly);
    rangeRow_->setVisible(program_.targetOnly && program_.heightFilter);
    empty_->setVisible(program_.conditions.empty() && !program_.targetOnly);
    emitChanged();
}

void MacroConditionList::emitChanged()
{
    if (!loading_ && changed) changed(program_);
}
