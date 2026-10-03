#include "widgets/MacroActionTimeline.h"

#include "widgets/MacroUiCommon.h"

#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {

using AT = macros::ActionType;
using macro_ui::q;
using macro_ui::utf8;

const QStringList& actionNames()
{
    static const QStringList names = [] {
        QStringList list;
        for (const auto* name : macros::actionLabels()) list << QString::fromUtf8(name);
        return list;
    }();
    return names;
}

macros::Action defaultAction(int type)
{
    macros::Action a;
    a.type = static_cast<AT>(type);
    if (type > 11) a.a = a.b = 0;
    if (a.type == AT::MouseDown || a.type == AT::MouseUp || a.type == AT::MouseClick) { a.a = 1; a.b = 30; }
    else if (a.type == AT::MouseMove) a.a = a.b = 0;
    else if (a.type == AT::KeyPress) a.b = 30;
    else if (a.type == AT::Wheel) a.a = 1;
    if (a.type == AT::Loop) a.a = 2;
    if (a.type == AT::WaitCondition) a.b = 1000;
    if (a.type == AT::Retry) { a.b = 3; a.c = 100; }
    if (a.type == AT::SmoothMove || a.type == AT::CurveMove) a.c = 100;
    if (a.type == AT::ForTargets || a.type == AT::AimClass) a.c = a.a = -1;
    if (a.type == AT::AimPart) a.a = a.b = 50;
    if (a.type == AT::Smoothing) a.value = 1;
    return a;
}

QToolButton* flatButton(const QString& text, const QString& tip, const QString& name)
{
    auto* button = new QToolButton;
    button->setText(text);
    button->setToolTip(tip);
    button->setObjectName(name);
    button->setAutoRaise(true);
    return button;
}

} // namespace

MacroActionTimeline::MacroActionTimeline(QWidget* parent)
    : CardWidget(QStringLiteral("③ 动作时间线 · 按顺序执行"), QStringLiteral("player-play"), parent)
{
    auto* body = contentLayout();
    body->setSpacing(8);
    body->addWidget(macro_ui::hint(QStringLiteral(
        "从上到下依次执行。直接在每一行里修改，移动单位是设备计数（右 / 下为正）。")));
    rows_ = new QVBoxLayout;
    rows_->setSpacing(6);
    body->addLayout(rows_);
    empty_ = macro_ui::hint(QStringLiteral("还没有动作，点下面的按钮添加第一步。"));
    body->addWidget(empty_);

    auto* tools = new QHBoxLayout;
    for (const auto& shortcut : std::initializer_list<std::pair<const char*, std::pair<int, const char*>>>{
             {"＋ 等待", {0, "macroAddWait"}}, {"＋ 键盘", {3, "macroAddKey"}},
             {"＋ 鼠标", {7, "macroAddMouse"}}, {"＋ 移动", {4, "macroAddMove"}}}) {
        auto* button = new QPushButton(QString::fromUtf8(shortcut.first));
        button->setObjectName(QString::fromLatin1(shortcut.second.second));
        const int type = shortcut.second.first;
        connect(button, &QPushButton::clicked, this, [this, type] { addAction(type); });
        tools->addWidget(button);
    }
    tools->addStretch();
    more_ = new QPushButton(QStringLiteral("更多动作 ▾"));
    more_->setObjectName("macroAddMore");
    more_->setStyleSheet(QStringLiteral("QPushButton::menu-indicator{image:none;width:0;}"));
    auto* catalog = new QMenu(more_);
    const auto category = [&](const QString& title, std::initializer_list<int> types) {
        auto* menu = catalog->addMenu(title);
        for (const int type : types) {
            auto* item = menu->addAction(actionNames().value(type));
            connect(item, &QAction::triggered, this, [this, type] { addAction(type); });
        }
    };
    category(QStringLiteral("基础输入"), {0, 1, 2, 3, 4, 5, 6, 7, 8, 30, 31, 32, 33});
    category(QStringLiteral("循环与条件"), {12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 52, 53, 54, 55, 56, 57, 58, 59});
    category(QStringLiteral("变量与规则"), {22, 23, 24, 25, 26, 27, 28, 29});
    category(QStringLiteral("目标与瞄准"), {9, 10, 11, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45});
    category(QStringLiteral("通知与配置"), {46, 47, 48, 49, 50, 51});
    more_->setMenu(catalog);
    tools->addWidget(more_);
    body->addLayout(tools);
}

void MacroActionTimeline::setProgram(const macros::Program& program)
{
    // Rows are only rebuilt when the steps really differ, so a refresh caused by an
    // edit elsewhere on the page never steals focus from a field being edited here.
    const bool same = program.actions == program_.actions;
    program_ = program;
    if (!same || rows_->count() == 0) rebuild();
}

void MacroActionTimeline::rebuild()
{
    const bool wasLoading = loading_;
    loading_ = true;
    while (QLayoutItem* item = rows_->takeAt(0)) {
        if (QWidget* widget = item->widget()) { widget->hide(); widget->deleteLater(); }
        delete item;
    }
    for (int i = 0; i < static_cast<int>(program_.actions.size()); ++i)
        rows_->addWidget(makeRow(i));
    empty_->setVisible(program_.actions.empty());
    loading_ = wasLoading;
}

QWidget* MacroActionTimeline::makeRow(int index)
{
    const macros::Action& a = program_.actions[static_cast<size_t>(index)];
    const QString name = QStringLiteral("macroAction%1").arg(index);
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("macroActionRow"));
    frame->setStyleSheet(QStringLiteral(
        "QFrame#macroActionRow{background:#1B1B1F;border:1px solid #2C2C32;border-radius:8px;}"));
    auto* outer = new QVBoxLayout(frame);
    outer->setContentsMargins(10, 8, 8, 8);
    outer->setSpacing(6);
    auto* line = new QHBoxLayout;
    line->setSpacing(8);
    outer->addLayout(line);

    auto* badge = new QLabel(QString::number(index + 1));
    badge->setFixedWidth(22);
    badge->setAlignment(Qt::AlignCenter);
    badge->setToolTip(QStringLiteral("步骤编号：流程动作按这个编号跳转"));
    badge->setStyleSheet(QStringLiteral("color:#D5B56B;font-weight:600;"));
    line->addWidget(badge);

    auto* type = new QComboBox;
    type->setObjectName(name + QStringLiteral("Type"));
    type->setMinimumWidth(170);
    type->setMaxVisibleItems(24);
    type->addItems(actionNames());
    type->setCurrentIndex(static_cast<int>(a.type));
    line->addWidget(type);
    connect(type, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, index](int t) {
        if (loading_) return;
        program_.actions[static_cast<size_t>(index)] = defaultAction(t);
        rebuild();
        emitChanged();
    });

    const auto action = [this, index]() -> macros::Action& {
        return program_.actions[static_cast<size_t>(index)];
    };
    const bool keyboard = a.type == AT::KeyDown || a.type == AT::KeyUp || a.type == AT::KeyPress;
    const bool mouse = a.type == AT::MouseDown || a.type == AT::MouseUp || a.type == AT::MouseClick;

    if (a.type == AT::Delay) {
        auto* low = macro_ui::spin(0, 60000, QStringLiteral(" ms"), 90);
        auto* high = macro_ui::spin(0, 60000, QStringLiteral(" ms"), 90);
        low->setObjectName(name + QStringLiteral("A"));
        high->setObjectName(name + QStringLiteral("B"));
        low->setValue(a.a);
        high->setValue(a.b);
        low->setToolTip(QStringLiteral("最短等待。两个值相同为固定延时，不同则在区间内随机等待。"));
        high->setToolTip(QStringLiteral("最长等待"));
        line->addWidget(low);
        line->addWidget(new QLabel(QStringLiteral("～")));
        line->addWidget(high);
        connect(low, &QSpinBox::editingFinished, this, [=, this] {
            if (loading_) return;
            action().a = low->value();
            if (action().b < action().a) { action().b = action().a; high->setValue(action().b); }
            emitChanged();
        });
        connect(high, &QSpinBox::editingFinished, this, [=, this] {
            if (loading_) return;
            action().b = std::max(high->value(), action().a);
            if (action().b != high->value()) high->setValue(action().b);
            emitChanged();
        });
    } else if (keyboard) {
        auto* key = macro_ui::keyCombo(false, false);
        key->setEditable(true);
        key->setObjectName(name + QStringLiteral("Key"));
        key->setMinimumWidth(130);
        key->setToolTip(QStringLiteral("可选单键，也可输入 LeftControl+U 组合键。媒体键和 VK:0xNN 通用键需要 "
                                       "Windows 原生输出；硬件最多同时按住 6 个普通键。"));
        macro_ui::setKey(key, a.key);
        line->addWidget(key);
        const auto read = [=, this] {
            if (loading_) return;
            action().key = macro_ui::currentKey(key);
            emitChanged();
        };
        connect(key, QOverload<int>::of(&QComboBox::currentIndexChanged), this, read);
        connect(key->lineEdit(), &QLineEdit::editingFinished, this, read);
        if (a.type == AT::KeyPress) {
            auto* hold = macro_ui::spin(1, 2000, QStringLiteral(" ms"), 90);
            hold->setObjectName(name + QStringLiteral("B"));
            hold->setToolTip(QStringLiteral("按下后保持多久再松开"));
            hold->setValue(a.b);
            line->addWidget(hold);
            connect(hold, &QSpinBox::editingFinished, this, [=, this] {
                if (loading_) return;
                action().b = hold->value();
                emitChanged();
            });
        }
    } else if (mouse) {
        auto* button = new QComboBox;
        button->setObjectName(name + QStringLiteral("Button"));
        button->addItems(macro_ui::buttonNames());
        button->setCurrentIndex(std::clamp(a.a - 1, 0, 4));
        button->setToolTip(QStringLiteral("侧键输出支持 Windows 原生、MAKCU、MAKCUNEW、KMBox Net。"
                                          "停止宏时会释放宏按住的按钮。"));
        line->addWidget(button);
        connect(button, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=, this] {
            if (loading_) return;
            action().a = button->currentIndex() + 1;
            emitChanged();
        });
        if (a.type == AT::MouseClick) {
            auto* hold = macro_ui::spin(1, 2000, QStringLiteral(" ms"), 90);
            hold->setObjectName(name + QStringLiteral("B"));
            hold->setToolTip(QStringLiteral("按下后保持多久再松开"));
            hold->setValue(a.b);
            line->addWidget(hold);
            connect(hold, &QSpinBox::editingFinished, this, [=, this] {
                if (loading_) return;
                action().b = hold->value();
                emitChanged();
            });
        }
    } else if (a.type == AT::MouseMove) {
        auto* x = macro_ui::spin(-32767, 32767, QString(), 90);
        auto* y = macro_ui::spin(-32767, 32767, QString(), 90);
        x->setObjectName(name + QStringLiteral("A"));
        y->setObjectName(name + QStringLiteral("B"));
        x->setValue(a.a);
        y->setValue(a.b);
        x->setToolTip(QStringLiteral("X 位移（设备计数，右为正）"));
        y->setToolTip(QStringLiteral("Y 位移（设备计数，下为正）"));
        line->addWidget(new QLabel(QStringLiteral("X")));
        line->addWidget(x);
        line->addWidget(new QLabel(QStringLiteral("Y")));
        line->addWidget(y);
        connect(x, &QSpinBox::editingFinished, this, [=, this] {
            if (loading_) return;
            action().a = x->value();
            emitChanged();
        });
        connect(y, &QSpinBox::editingFinished, this, [=, this] {
            if (loading_) return;
            action().b = y->value();
            emitChanged();
        });
    } else if (a.type == AT::Wheel) {
        auto* notches = macro_ui::spin(-127, 127, QStringLiteral(" 格"), 90);
        notches->setObjectName(name + QStringLiteral("A"));
        notches->setValue(a.a);
        notches->setToolTip(QStringLiteral("滚动量，正数向上"));
        line->addWidget(notches);
        connect(notches, &QSpinBox::editingFinished, this, [=, this] {
            if (loading_) return;
            action().a = notches->value();
            emitChanged();
        });
    }

    line->addStretch(1);
    const int last = static_cast<int>(program_.actions.size()) - 1;
    auto* up = flatButton(QStringLiteral("↑"), QStringLiteral("上移"), name + QStringLiteral("Up"));
    auto* down = flatButton(QStringLiteral("↓"), QStringLiteral("下移"), name + QStringLiteral("Down"));
    auto* copy = flatButton(QStringLiteral("⧉"), QStringLiteral("复制这一步"), name + QStringLiteral("Copy"));
    auto* remove = flatButton(QStringLiteral("✕"), QStringLiteral("删除这一步"), name + QStringLiteral("Remove"));
    up->setEnabled(index > 0);
    down->setEnabled(index < last);
    copy->setEnabled(static_cast<int>(program_.actions.size()) < macros::maxActions);
    for (auto* button : {up, down, copy, remove}) line->addWidget(button);
    connect(up, &QToolButton::clicked, this, [this, index] { moveAction(index, -1); });
    connect(down, &QToolButton::clicked, this, [this, index] { moveAction(index, 1); });
    connect(copy, &QToolButton::clicked, this, [this, index] { duplicateAction(index); });
    connect(remove, &QToolButton::clicked, this, [this, index] { removeAction(index); });

    // Steps outside the common set describe their own settings in the action schema.
    if (!keyboard && !mouse && a.type != AT::Delay && a.type != AT::MouseMove && a.type != AT::Wheel) {
        const auto fields = macros::actionFields(a.type);
        struct Field { const char* label; char slot; };
        std::vector<Field> used;
        if (*fields.a) used.push_back({fields.a, 'a'});
        if (*fields.b) used.push_back({fields.b, 'b'});
        if (*fields.c) used.push_back({fields.c, 'c'});
        if (*fields.d) used.push_back({fields.d, 'd'});
        if (*fields.value) used.push_back({fields.value, 'v'});
        if (*fields.text) used.push_back({fields.text, 't'});
        if (!used.empty()) {
            auto* grid = new QGridLayout;
            grid->setHorizontalSpacing(10);
            grid->setVerticalSpacing(6);
            grid->setColumnStretch(1, 1);
            grid->setColumnStretch(3, 1);
            for (size_t i = 0; i < used.size(); ++i) {
                const int r = static_cast<int>(i / 2), col = static_cast<int>(i % 2) * 2;
                auto* label = new QLabel(QString::fromUtf8(used[i].label));
                label->setWordWrap(true);
                label->setStyleSheet(QStringLiteral("color:#ABA697;font-size:12px;"));
                grid->addWidget(label, r, col);
                const char slot = used[i].slot;
                QWidget* editor = nullptr;
                if (slot == 'v') {
                    auto* v = macro_ui::doubleSpin(-1e9, 1e9, 4, 100);
                    v->setValue(a.value);
                    connect(v, &QDoubleSpinBox::editingFinished, this, [=, this] {
                        if (loading_) return;
                        action().value = v->value();
                        emitChanged();
                    });
                    editor = v;
                } else if (slot == 't') {
                    auto* t = new QLineEdit(q(a.text));
                    t->setMaxLength(4096);
                    connect(t, &QLineEdit::editingFinished, this, [=, this] {
                        if (loading_) return;
                        action().text = utf8(t->text());
                        emitChanged();
                    });
                    editor = t;
                } else {
                    auto* n = macro_ui::spin(-32767, 1000000, QString(), 100);
                    n->setValue(slot == 'a' ? a.a : slot == 'b' ? a.b : slot == 'c' ? a.c : a.d);
                    connect(n, &QSpinBox::editingFinished, this, [=, this] {
                        if (loading_) return;
                        (slot == 'a' ? action().a : slot == 'b' ? action().b : slot == 'c' ? action().c : action().d) = n->value();
                        emitChanged();
                    });
                    editor = n;
                }
                editor->setObjectName(name + QStringLiteral("Field%1").arg(i));
                grid->addWidget(editor, r, col + 1);
            }
            outer->addLayout(grid);
        }
        if (*fields.help) outer->addWidget(macro_ui::hint(QString::fromUtf8(fields.help)));
    }
    return frame;
}

void MacroActionTimeline::addAction(int type)
{
    if (static_cast<int>(program_.actions.size()) >= macros::maxActions) return;
    program_.actions.push_back(defaultAction(type));
    rebuild();
    emitChanged();
}

void MacroActionTimeline::moveAction(int index, int delta)
{
    const int next = index + delta;
    if (index < 0 || next < 0 || index >= static_cast<int>(program_.actions.size()) ||
        next >= static_cast<int>(program_.actions.size()))
        return;
    std::swap(program_.actions[static_cast<size_t>(index)], program_.actions[static_cast<size_t>(next)]);
    rebuild();
    emitChanged();
}

void MacroActionTimeline::duplicateAction(int index)
{
    if (index < 0 || index >= static_cast<int>(program_.actions.size()) ||
        static_cast<int>(program_.actions.size()) >= macros::maxActions)
        return;
    const macros::Action copy = program_.actions[static_cast<size_t>(index)];
    program_.actions.insert(program_.actions.begin() + index + 1, copy);
    rebuild();
    emitChanged();
}

void MacroActionTimeline::removeAction(int index)
{
    if (index < 0 || index >= static_cast<int>(program_.actions.size())) return;
    program_.actions.erase(program_.actions.begin() + index);
    rebuild();
    emitChanged();
}

void MacroActionTimeline::emitChanged()
{
    if (!loading_ && changed) changed(program_);
}
