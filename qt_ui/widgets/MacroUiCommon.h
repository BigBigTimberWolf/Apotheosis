#pragma once

// Small helpers shared by the macro editor widgets (trigger card, condition list,
// action timeline, page). Header-only; depends only on Qt widgets and the macro
// data model, never on the running application.

#include "macro/macro_config.h"
#include "macro/rule_schema.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QSpinBox>
#include <QString>
#include <QStringList>
#include <QWheelEvent>

#include <algorithm>
#include <string>

namespace macro_ui {

inline QString q(const std::string& s) { return QString::fromUtf8(s.c_str()); }
inline std::string utf8(const QString& s) { return s.toUtf8().toStdString(); }

// Action::a is 1..5 for the mouse actions.
inline const QStringList& buttonNames()
{
    static const QStringList names = {QStringLiteral("左键"), QStringLiteral("右键"),
        QStringLiteral("中键"), QStringLiteral("侧键 4"), QStringLiteral("侧键 5")};
    return names;
}

// Order matches macros::Mode.
inline const QStringList& modeNames()
{
    static const QStringList names = {QStringLiteral("按一次，执行一轮"),
        QStringLiteral("按住循环，松开停止"), QStringLiteral("按一次开始，再按停止"),
        QStringLiteral("每按一次，执行下一步")};
    return names;
}

// Wheel events only act on a field the user clicked into, so scrolling the page
// never edits a value by accident.
class Spin : public QSpinBox
{
public:
    using QSpinBox::QSpinBox;
protected:
    void wheelEvent(QWheelEvent* e) override
    {
        if (!hasFocus()) e->ignore(); else QSpinBox::wheelEvent(e);
    }
};

class DoubleSpin : public QDoubleSpinBox
{
public:
    using QDoubleSpinBox::QDoubleSpinBox;
protected:
    void wheelEvent(QWheelEvent* e) override
    {
        if (!hasFocus()) e->ignore(); else QDoubleSpinBox::wheelEvent(e);
    }
};

inline QSpinBox* spin(int low, int high, const QString& suffix = {}, int width = 0)
{
    auto* s = new Spin;
    s->setRange(low, high);
    s->setSuffix(suffix);
    if (width > 0) s->setMinimumWidth(width);
    return s;
}

inline QDoubleSpinBox* doubleSpin(double low, double high, int decimals = 4, int width = 0)
{
    auto* s = new DoubleSpin;
    s->setRange(low, high);
    s->setDecimals(decimals);
    if (width > 0) s->setMinimumWidth(width);
    return s;
}

inline QLabel* hint(const QString& text)
{
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setStyleSheet(QStringLiteral("color:#ABA697;font-size:12px;"));
    return label;
}

// A key chooser. `mouse` adds the five mouse buttons, `none` an "unset" entry.
inline QComboBox* keyCombo(bool mouse, bool none)
{
    auto* combo = new QComboBox;
    if (none) combo->addItem(QStringLiteral("未设置"), QString());
    if (mouse) {
        const char* ids[] = {"LeftMouseButton", "RightMouseButton", "MiddleMouseButton",
                             "X1MouseButton", "X2MouseButton"};
        for (int i = 0; i < 5; ++i)
            combo->addItem(QStringLiteral("鼠标 · ") + buttonNames()[i], QString::fromLatin1(ids[i]));
    }
    // 滚轮不是按键：选中后是“滚一格”的输出/一次触发（见 macros::wheelKeys）。
    for (const auto& wheel : macros::wheelKeys())
        combo->addItem(q(wheel.label), QString::fromLatin1(wheel.id));
    for (const auto& key : macros::keys())
        combo->addItem(q(key.label), q(key.id));
    return combo;
}

inline void setKey(QComboBox* combo, const std::string& key)
{
    const QString text = q(key);
    combo->setCurrentIndex(combo->findData(text));
    if (combo->isEditable() && combo->currentIndex() < 0) combo->setEditText(text);
}

// The id of the chosen key, or the typed text (chords such as LeftControl+U).
inline std::string currentKey(const QComboBox* combo)
{
    const int index = combo->currentIndex();
    const bool picked = index >= 0 && combo->currentText() == combo->itemText(index);
    return utf8((picked ? combo->itemData(index).toString() : combo->currentText()).trimmed());
}

// Delete condition `index` (0-based) and repair every reference to it: the parent
// links of other conditions, the condition numbers used by flow actions and the
// extra cancel condition. Refuses (returns false) while it still has children.
inline bool removeCondition(macros::Program& program, int index)
{
    auto& conditions = program.conditions;
    if (index < 0 || index >= static_cast<int>(conditions.size())) return false;
    for (const auto& c : conditions)
        if (c.parent == index) return false;
    conditions.erase(conditions.begin() + index);
    for (auto& c : conditions)
        if (c.parent > index) --c.parent;
    using AT = macros::ActionType;
    for (auto& a : program.actions) {
        if (a.type == AT::If || a.type == AT::While || a.type == AT::WaitCondition || a.type == AT::Retry) {
            if (a.a == index + 1) a.a = -1;
            else if (a.a > index + 1) --a.a;
        }
        if (a.type == AT::Jump) {
            if (a.b == index + 1) a.b = -1;
            else if (a.b > index + 1) --a.b;
        }
    }
    const int cancel = macros::number(program, "cancel_condition");
    if (cancel == index + 1) program.options["cancel_condition"] = "-1";
    else if (cancel > index + 1) program.options["cancel_condition"] = std::to_string(cancel - 1);
    return true;
}

} // namespace macro_ui
