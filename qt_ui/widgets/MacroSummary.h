#pragma once

// A short plain-language sentence for a macro ("按下热键「F1」时，且存在目标，则依次
// 执行：…"), shown at the top of the macro page. Pure functions over the macro
// data model; no widgets.

#include "macro/rule_logic.h"
#include "widgets/MacroUiCommon.h"

#include <QString>
#include <QStringList>

#include <string>

namespace macro_summary {

using macro_ui::q;

inline bool isKeyEvent(const std::string& event)
{
    return event == "key_down" || event == "key_up" || event == "key_held" || event == "click" ||
           event == "double" || event == "triple" || event == "long" || event == "short" ||
           event == "sequence";
}

inline QString keyName(const std::string& id)
{
    for (const auto& key : macros::keys())
        if (key.id == id) return q(key.label);
    static const std::pair<const char*, const char*> special[] = {
        {"LeftMouseButton", "鼠标左键"}, {"RightMouseButton", "鼠标右键"},
        {"MiddleMouseButton", "鼠标中键"}, {"X1MouseButton", "鼠标侧键 4"},
        {"X2MouseButton", "鼠标侧键 5"}, {"WheelUp", "滚轮向上"}, {"WheelDown", "滚轮向下"}};
    for (const auto& entry : special)
        if (id == entry.first) return QString::fromUtf8(entry.second);
    return q(id);
}

// "LeftControl+U" -> "左 Ctrl + U"
inline QString chordName(const std::string& chord)
{
    QStringList parts;
    for (const auto& key : macros::split(chord, '+')) parts << keyName(key);
    return parts.join(QStringLiteral(" + "));
}

// "A>B>C" -> "A → B → C"
inline QString sequenceName(const std::string& sequence)
{
    QStringList parts;
    for (const auto& step : macros::split(sequence, '>')) parts << chordName(step);
    return parts.join(QStringLiteral(" → "));
}

inline QString triggerText(const macros::Program& p)
{
    const std::string event = macros::option(p, "event", "key_down");
    if (!isKeyEvent(event)) {
        return QStringLiteral("当「%1」时").arg(q(macros::label(macros::events(), event)));
    }
    if (event == "sequence") {
        const QString steps = p.trigger.empty() ? QStringLiteral("未设置") : sequenceName(p.trigger);
        return QStringLiteral("依次按下「%1」时").arg(steps);
    }
    const QString key = p.trigger.empty() ? QStringLiteral("未设置") : chordName(p.trigger);
    if (event == "key_up") return QStringLiteral("松开热键「%1」时").arg(key);
    if (event == "key_held") return QStringLiteral("按住热键「%1」期间").arg(key);
    if (event == "click") return QStringLiteral("单击热键「%1」时").arg(key);
    if (event == "double") return QStringLiteral("双击热键「%1」时").arg(key);
    if (event == "triple") return QStringLiteral("三击热键「%1」时").arg(key);
    if (event == "long") return QStringLiteral("长按热键「%1」时").arg(key);
    if (event == "short") return QStringLiteral("短按热键「%1」时").arg(key);
    return QStringLiteral("按下热键「%1」时").arg(key);
}

inline bool isFlagMetric(const std::string& metric)
{
    static const char* flags[] = {"target.exists", "aim.active", "trigger.active", "aim.key",
        "color.hit", "target.fov", "target.visible", "target.occluded", "target.locked",
        "image.hit", "pixels.multi", "window.exists", "process.exists", "cooldown.ready", "key"};
    for (const char* flag : flags)
        if (metric == flag) return true;
    return false;
}

inline QString compareText(const std::string& op)
{
    if (op == "==") return QStringLiteral("等于");
    if (op == "!=") return QStringLiteral("不等于");
    if (op == ">") return QStringLiteral("大于");
    if (op == ">=") return QStringLiteral("至少");
    if (op == "<") return QStringLiteral("小于");
    if (op == "<=") return QStringLiteral("至多");
    return q(op);
}

inline QString conditionText(const macros::Condition& c)
{
    const QString label = q(macros::label(macros::metrics(), c.metric));
    if (macros::groupMetric(c.metric)) return label;
    const bool classFilter = c.classId >= 0 && c.metric.rfind("target.", 0) == 0 &&
                             c.metric != "target.class";
    if (isFlagMetric(c.metric)) {
        const bool negative = (c.comparison == "==" && c.value == 0.0) ||
                              (c.comparison == "!=" && c.value != 0.0);
        if (c.metric == "key")
            return QStringLiteral("按键「%1」%2").arg(chordName(c.text),
                negative ? QStringLiteral("没有按下") : QStringLiteral("已按下"));
        if (c.metric == "target.exists") {
            const QString who = classFilter ? QStringLiteral("类别 %1 的目标").arg(c.classId)
                                            : QStringLiteral("目标");
            return (negative ? QStringLiteral("不存在") : QStringLiteral("存在")) + who;
        }
        return negative ? QStringLiteral("没有「%1」").arg(label) : label;
    }
    QString subject = label;
    if (classFilter) subject = QStringLiteral("类别 %1：").arg(c.classId) + subject;
    if (c.comparison == "contains") return QStringLiteral("%1 包含「%2」").arg(subject, q(c.text));
    if (c.comparison == "text_eq") return QStringLiteral("%1 是「%2」").arg(subject, q(c.text));
    if (c.comparison == "text_ne") return QStringLiteral("%1 不是「%2」").arg(subject, q(c.text));
    if (c.comparison == "range")
        return QStringLiteral("%1 介于 %2 与 %3 之间").arg(subject).arg(c.value).arg(c.upper);
    QString text = QStringLiteral("%1 %2 %3").arg(subject, compareText(c.comparison)).arg(c.value);
    if (c.metric == "target.max_size")
        text += QStringLiteral("，最大高度 %1 %2").arg(compareText(c.comparison)).arg(c.upper);
    return text;
}

inline QString actionText(const macros::Action& a)
{
    using AT = macros::ActionType;
    const auto button = [&] {
        return macro_ui::buttonNames().value(a.a - 1, QStringLiteral("无效按键"));
    };
    switch (a.type) {
    case AT::Delay:
        return a.a == a.b ? QStringLiteral("等待 %1 毫秒").arg(a.a)
                          : QStringLiteral("随机等待 %1～%2 毫秒").arg(a.a).arg(a.b);
    case AT::KeyDown: return QStringLiteral("按下键盘「%1」").arg(chordName(a.key));
    case AT::KeyUp: return QStringLiteral("松开键盘「%1」").arg(chordName(a.key));
    case AT::KeyPress:
        return QStringLiteral("点按键盘「%1」，保持 %2 毫秒后松开").arg(chordName(a.key)).arg(a.b);
    case AT::MouseDown: return QStringLiteral("按下鼠标「%1」").arg(button());
    case AT::MouseUp: return QStringLiteral("松开鼠标「%1」").arg(button());
    case AT::MouseClick:
        return QStringLiteral("点击鼠标「%1」，保持 %2 毫秒后松开").arg(button()).arg(a.b);
    case AT::MouseMove: return QStringLiteral("鼠标相对移动 X %1 / Y %2").arg(a.a).arg(a.b);
    case AT::Wheel: return QStringLiteral("滚轮 %1 格").arg(a.a);
    default: break;
    }
    const auto& labels = macros::actionLabels();
    const int index = static_cast<int>(a.type);
    QString text = index >= 0 && index < static_cast<int>(labels.size())
        ? QString::fromUtf8(labels[static_cast<size_t>(index)]) : QStringLiteral("未知动作");
    if (!a.text.empty()) text += QStringLiteral("「%1」").arg(q(a.text));
    return text;
}

inline QString describe(const macros::Program& p)
{
    QStringList conditions;
    if (p.targetOnly) conditions << QStringLiteral("存在符合类别 / 框高限制的目标");
    for (const auto& c : p.conditions)
        if (c.parent == -1) conditions << conditionText(c);

    QString text = triggerText(p);
    if (!conditions.isEmpty()) text += QStringLiteral("，且") + conditions.join(QStringLiteral("、"));
    if (p.actions.empty()) return text + QStringLiteral("。还没有添加动作。");

    constexpr size_t kShown = 12;
    QStringList steps;
    for (size_t i = 0; i < p.actions.size() && i < kShown; ++i) steps << actionText(p.actions[i]);
    text += QStringLiteral("，则依次执行：") + steps.join(QStringLiteral("；然后"));
    if (p.actions.size() > kShown)
        text += QStringLiteral("…（共 %1 步）").arg(p.actions.size());
    text += QStringLiteral("。");

    switch (p.mode) {
    case macros::Mode::Hold:
        text += QStringLiteral("按住期间每隔 %1 毫秒重复一轮，松开即停。").arg(p.loopIntervalMs);
        break;
    case macros::Mode::Toggle:
        text += QStringLiteral("按一次开始循环（每轮间隔 %1 毫秒），再按一次停止。").arg(p.loopIntervalMs);
        break;
    case macros::Mode::Sequence:
        text += QStringLiteral("每触发一次只执行下一步。");
        break;
    case macros::Mode::Once:
        break;
    }
    const int cooldown = macros::number(p, "cooldown_ms");
    if (cooldown > 0) text += QStringLiteral("触发冷却 %1 毫秒。").arg(cooldown);
    return text;
}

} // namespace macro_summary
