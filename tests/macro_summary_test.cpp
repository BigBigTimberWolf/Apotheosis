// Plain-language macro summary and the condition-removal bookkeeping shared by the
// macro editor widgets. No widgets are created.

#include "widgets/MacroSummary.h"
#include "widgets/MacroUiCommon.h"

#include <cstdio>

namespace {
int failures = 0;
void check(bool okay, const char* message)
{
    if (!okay) { std::printf("FAIL: %s\n", message); ++failures; }
}
bool has(const QString& text, const char* part)
{
    return text.contains(QString::fromUtf8(part));
}

macros::Action action(macros::ActionType type, int a = 0, int b = 0, const char* key = "Key1")
{
    macros::Action x;
    x.type = type;
    x.a = a;
    x.b = b;
    x.key = key;
    return x;
}
} // namespace

int main()
{
    using AT = macros::ActionType;

    // Trigger wording.
    macros::Program p;
    p.trigger = "F1";
    check(has(macro_summary::triggerText(p), "按下热键「F1」时"), "default key event reads as 'pressed'");
    p.trigger = "LeftControl+U";
    check(has(macro_summary::triggerText(p), "左 Ctrl + U"), "a chord names every key");
    p.options["event"] = "long";
    check(has(macro_summary::triggerText(p), "长按热键「左 Ctrl + U」时"), "long press wording");
    p.options["event"] = "sequence";
    p.trigger = "A>B>C";
    check(has(macro_summary::triggerText(p), "依次按下「A → B → C」时"), "a key sequence is spelled out in order");
    p.trigger.clear();
    p.options["event"] = "key_down";
    check(has(macro_summary::triggerText(p), "「未设置」"), "a missing key is called out");
    p.options["event"] = "target_lost";
    check(has(macro_summary::triggerText(p), "当「失去检测目标」时"), "an event trigger names the event");
    p.options["event"] = "some_future_event";
    check(has(macro_summary::triggerText(p), "当「some_future_event」时"), "an unknown event id is shown, not hidden");
    check(macro_summary::isKeyEvent("click") && !macro_summary::isKeyEvent("aim_start"),
          "key events and runtime events are told apart");
    check(has(macro_summary::keyName("RightMouseButton"), "鼠标右键") &&
          has(macro_summary::keyName("WheelUp"), "滚轮向上"), "mouse and wheel triggers have readable names");

    // Step wording.
    check(has(macro_summary::actionText(action(AT::Delay, 50, 50)), "等待 50 毫秒"), "fixed wait");
    check(has(macro_summary::actionText(action(AT::Delay, 10, 40)), "随机等待 10～40 毫秒"), "random wait");
    check(has(macro_summary::actionText(action(AT::KeyPress, 0, 20, "U")), "点按键盘「U」，保持 20 毫秒后松开"),
          "key press with hold time");
    check(has(macro_summary::actionText(action(AT::MouseClick, 2, 30)), "点击鼠标「右键」，保持 30 毫秒后松开"),
          "mouse click names the button");
    check(has(macro_summary::actionText(action(AT::MouseDown, 9, 0)), "无效按键"), "an invalid button is flagged");
    check(has(macro_summary::actionText(action(AT::MouseMove, 10, -5)), "X 10 / Y -5"), "mouse move shows both axes");
    check(has(macro_summary::actionText(action(AT::Wheel, 3)), "滚轮 3 格"), "wheel notches");
    macros::Action text = action(AT::Text);
    text.text = "hello";
    check(has(macro_summary::actionText(text), "输入文本「hello」"), "a step with a text setting shows it");
    check(has(macro_summary::actionText(action(static_cast<AT>(999))), "未知动作"), "an unknown step type does not crash");

    // Condition wording.
    macros::Condition c;
    c.metric = "target.exists";
    check(has(macro_summary::conditionText(c), "存在目标"), "flag condition, true");
    c.value = 0;
    check(has(macro_summary::conditionText(c), "不存在目标"), "flag condition, false");
    c.value = 1;
    c.classId = 5;
    check(has(macro_summary::conditionText(c), "存在类别 5 的目标"), "target class filter is named");
    macros::Condition count;
    count.metric = "target.count";
    count.comparison = ">=";
    count.value = 2;
    check(has(macro_summary::conditionText(count), "至少 2"), "numeric comparison");
    count.comparison = "range";
    count.upper = 4;
    check(has(macro_summary::conditionText(count), "介于 2 与 4 之间"), "range comparison");
    macros::Condition key;
    key.metric = "key";
    key.text = "LeftControl+U";
    check(has(macro_summary::conditionText(key), "按键「左 Ctrl + U」已按下"), "key condition");
    key.value = 0;
    check(has(macro_summary::conditionText(key), "没有按下"), "negated key condition");
    macros::Condition title;
    title.metric = "window.title";
    title.comparison = "contains";
    title.text = "Game";
    check(has(macro_summary::conditionText(title), "包含「Game」"), "text condition");
    macros::Condition group;
    group.metric = "any";
    check(has(macro_summary::conditionText(group), "任意满足"), "a group names its logic");

    // Whole sentence.
    macros::Program empty;
    empty.trigger = "F1";
    check(has(macro_summary::describe(empty), "还没有添加动作"), "an empty macro says so");

    macros::Program full;
    full.trigger = "F1";
    full.targetOnly = true;
    full.conditions = {c, count};
    full.conditions[0].classId = -1;
    macros::Condition hidden;
    hidden.metric = "aim.active";
    hidden.parent = -2; // only used by flow steps: must not be listed as a start condition
    full.conditions.push_back(hidden);
    full.actions = {action(AT::Delay, 50, 50), action(AT::KeyPress, 0, 20, "U")};
    full.mode = macros::Mode::Hold;
    full.loopIntervalMs = 33;
    full.options["cooldown_ms"] = "120";
    const QString sentence = macro_summary::describe(full);
    check(has(sentence, "按下热键「F1」时，且存在符合类别 / 框高限制的目标、存在目标、检测数量"), "conditions follow the trigger");
    check(!has(sentence, "自瞄激活"), "flow-only conditions are left out");
    check(has(sentence, "则依次执行：等待 50 毫秒；然后点按键盘「U」"), "steps are chained in order");
    check(has(sentence, "按住期间每隔 33 毫秒重复一轮") && has(sentence, "触发冷却 120 毫秒"), "mode and cooldown are appended");
    full.mode = macros::Mode::Toggle;
    check(has(macro_summary::describe(full), "再按一次停止"), "toggle wording");
    full.mode = macros::Mode::Sequence;
    check(has(macro_summary::describe(full), "只执行下一步"), "sequence wording");
    full.actions.assign(20, action(AT::Delay, 1, 1));
    check(has(macro_summary::describe(full), "（共 20 步）"), "a long macro is shortened with its step count");

    // Removing a condition repairs every reference to it.
    {
        macros::Program r;
        macros::Condition a, b, g, child;
        a.metric = "target.exists";                        // 1
        b.metric = "aim.active";                           // 2  <- removed
        g.metric = "any";                                  // 3 -> becomes 2
        child.metric = "trigger.active"; child.parent = 2; // 4 -> becomes 3, child of the group
        r.conditions = {a, b, g, child};
        r.actions = {action(AT::If, 3), action(AT::While, 2), action(AT::WaitCondition, 4, 100),
                     action(AT::Retry, 1), action(AT::Jump, 1, 3), action(AT::Jump, 1, 2)};
        r.options["cancel_condition"] = "4";
        check(macro_ui::removeCondition(r, 1), "a plain condition can be removed");
        check(r.conditions.size() == 3 && r.conditions[1].metric == "any" && r.conditions[2].parent == 1,
              "parent links shift down past the removed condition");
        check(r.actions[0].a == 2 && r.actions[1].a == -1 && r.actions[2].a == 3 && r.actions[3].a == 1,
              "flow steps follow their condition (or lose it when it was the removed one)");
        check(r.actions[4].b == 2 && r.actions[5].b == -1, "jump conditions are repaired too");
        check(macros::number(r, "cancel_condition") == 3, "the extra cancel condition keeps pointing at the same one");
        r.options["cancel_condition"] = "2"; // the group at number 2 is about to go
        macros::Program snapshot = r;
        check(!macro_ui::removeCondition(r, 1) && r == snapshot, "a group that still has children is refused, untouched");
        check(macro_ui::removeCondition(r, 2) && macro_ui::removeCondition(r, 1), "children first, then the group");
        check(macros::number(r, "cancel_condition") == -1, "removing the cancel condition clears it");
        check(!macro_ui::removeCondition(r, 5) && !macro_ui::removeCondition(r, -1), "out-of-range indexes are refused");
    }

    std::printf("macro summary: %d failures\n", failures);
    return failures ? 1 : 0;
}
