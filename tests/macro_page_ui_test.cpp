// Drives the real macro page (offscreen) the way a person would: create a macro,
// choose its trigger, add conditions, add steps, and check what lands in the saved
// configuration. The page's globals (config, engine, key blocker) are stand-ins
// from macro_page_stubs/.

#include "Apotheosis.h"
#include "config/config_profiles.h"
#include "macro/macro_engine.h"
#include "pages/MacroPage.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>

#include <cstdio>

namespace macros {
int testStopCount();
std::string testLastRun();
}

namespace {
int failures = 0;
void check(bool okay, const char* message)
{
    if (!okay) { std::printf("FAIL: %s\n", message); ++failures; }
}

QApplication* gApp = nullptr;

// Hidden rows are deleted later; flush that so a lookup never finds a stale widget.
void settle()
{
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        gApp->processEvents();
    }
}

void wait(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) gApp->processEvents(QEventLoop::AllEvents, 10);
    settle();
}

template <class T> T* find(QWidget& root, const char* name)
{
    return root.findChild<T*>(QString::fromLatin1(name));
}

void click(QAbstractButton* button)
{
    if (button) button->click();
    settle();
}

void pick(QComboBox* combo, const QString& data)
{
    if (!combo) return;
    combo->setCurrentIndex(combo->findData(data));
    settle();
}

void setSpin(QSpinBox* spin, int value)
{
    if (!spin) return;
    spin->setValue(value);
    emit spin->editingFinished();
    settle();
}

void setDouble(QDoubleSpinBox* spin, double value)
{
    if (!spin) return;
    spin->setValue(value);
    emit spin->editingFinished();
    settle();
}

void setText(QLineEdit* edit, const QString& text)
{
    if (!edit) return;
    edit->setText(text);
    emit edit->editingFinished();
    settle();
}

QAction* findAction(QMenu* menu, const QString& text)
{
    if (!menu) return nullptr;
    for (QAction* action : menu->actions()) {
        if (action->text() == text) return action;
        if (QAction* inner = findAction(action->menu(), text)) return inner;
    }
    return nullptr;
}

const macros::Program& saved(size_t index = 0)
{
    static const macros::Program empty;
    return index < config.macro_programs.size() ? config.macro_programs[index] : empty;
}

QString summaryOf(QWidget& page)
{
    auto* label = find<QLabel>(page, "macroSummary");
    return label ? label->text() : QString();
}
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    gApp = &app;
    config.macro_programs.clear();
    config.macro_programs_enabled = true;
    config.macro_stop_key = "F12";

    MacroPage page;
    page.resize(1200, 1000);
    page.show();
    settle();

    auto* library = find<QListWidget>(page, "macroLibrary");
    if (!library) { std::puts("FAIL: the macro library list is missing"); return 1; }
    constexpr int kEnabledRole = Qt::UserRole + 1, kSubtitleRole = Qt::UserRole + 2;

    // 1. Nothing yet: only the hint, no editor.
    check(find<QWidget>(page, "macroName") && !find<QWidget>(page, "macroName")->isVisible(),
          "no editor is shown before a macro exists");

    // 2. Create a macro. It is saved at once and the editor appears.
    click(find<QPushButton>(page, "macroNew"));
    check(config.macro_programs.size() == 1 && library->count() == 1, "a new macro is created and saved");
    check(find<QWidget>(page, "macroName")->isVisible(), "the editor appears for the selected macro");
    check(summaryOf(page).contains(QString::fromUtf8("还没有添加动作")), "an empty macro says it has no steps yet");

    // 3. Name and enable.
    setText(find<QLineEdit>(page, "macroName"), QString::fromUtf8("连点"));
    check(saved().name == "连点", "the name is saved");
    check(library->item(0)->text().contains(QString::fromUtf8("连点")), "the list shows the new name");
    find<QCheckBox>(page, "macroEnabled")->setChecked(true);
    settle();
    check(saved().enabled && library->item(0)->data(kEnabledRole).toBool(), "enabling is saved and shown in the list");

    // 4. Hotkey trigger.
    auto* key = find<QComboBox>(page, "macroTriggerKey");
    pick(key, "F1");
    check(saved().trigger == "F1" && macros::option(saved(), "event", "key_down") == "key_down",
          "choosing a hotkey stores the key and the default 'pressed' event");
    check(library->item(0)->data(kSubtitleRole).toString().contains(QString::fromUtf8("热键 · F1")),
          "the list subtitle shows the hotkey");
    pick(find<QComboBox>(page, "macroTriggerWhen"), "long");
    check(macros::option(saved(), "event", "") == "long" &&
          summaryOf(page).contains(QString::fromUtf8("长按热键「F1」时")),
          "'long press' is stored and explained");

    // 5. Switch to a runtime event and back; the hotkey is not lost.
    auto* kind = find<QComboBox>(page, "macroTriggerKind");
    kind->setCurrentIndex(1);
    settle();
    check(macros::option(saved(), "event", "") == "target_found" && saved().trigger == "F1",
          "switching to an event picks a sensible default and keeps the stored key");
    check(find<QWidget>(page, "macroTriggerEvent")->isVisible() && !key->isVisible(),
          "the event chooser replaces the key chooser");
    pick(find<QComboBox>(page, "macroTriggerEvent"), "target_lost");
    check(summaryOf(page).contains(QString::fromUtf8("当「失去检测目标」时")) &&
          library->item(0)->data(kSubtitleRole).toString().contains(QString::fromUtf8("事件 · 失去检测目标")),
          "an event trigger is explained and shown in the list");
    kind->setCurrentIndex(0);
    settle();
    check(macros::option(saved(), "event", "") == "key_down" && saved().trigger == "F1",
          "switching back to a hotkey restores a key event with the same key");

    // 6. How it runs.
    find<QComboBox>(page, "macroMode")->setCurrentIndex(1);
    settle();
    check(saved().mode == macros::Mode::Hold && find<QWidget>(page, "macroInterval")->isVisible(),
          "hold mode shows the loop interval");
    setSpin(find<QSpinBox>(page, "macroInterval"), 33);
    setSpin(find<QSpinBox>(page, "macroCooldown"), 120);
    check(saved().loopIntervalMs == 33 && macros::number(saved(), "cooldown_ms") == 120, "interval and cooldown are saved");
    find<QComboBox>(page, "macroMode")->setCurrentIndex(0);
    settle();
    check(!find<QWidget>(page, "macroInterval")->isVisible(), "the interval is hidden when it does not apply");

    // 7. Conditions.
    auto* addCondition = find<QPushButton>(page, "macroAddCondition");
    QAction* exists = findAction(addCondition ? addCondition->menu() : nullptr, QString::fromUtf8("存在目标"));
    check(exists != nullptr, "the add-condition menu offers 'target exists'");
    if (exists) exists->trigger();
    settle();
    check(saved().conditions.size() == 1 && saved().conditions[0].metric == "target.exists" &&
          saved().conditions[0].value == 1.0 && saved().conditions[0].parent == -1,
          "a new condition is a plain top-level test");
    check(summaryOf(page).contains(QString::fromUtf8("且存在目标")), "the condition appears in the sentence");
    find<QComboBox>(page, "macroCondition0State")->setCurrentIndex(1);
    settle();
    check(saved().conditions[0].value == 0.0 && summaryOf(page).contains(QString::fromUtf8("不存在目标")),
          "'not satisfied' flips the test");
    pick(find<QComboBox>(page, "macroCondition0Metric"), "target.count");
    check(saved().conditions[0].metric == "target.count" && saved().conditions[0].comparison == ">=" &&
          saved().conditions[0].value == 1.0, "changing the kind starts from that kind's defaults");
    setDouble(find<QDoubleSpinBox>(page, "macroCondition0Value"), 3);
    setSpin(find<QSpinBox>(page, "macroCondition0Class"), 5);
    check(saved().conditions[0].value == 3.0 && saved().conditions[0].classId == 5, "value and class are saved");
    pick(find<QComboBox>(page, "macroCondition0Op"), "range");
    check(find<QWidget>(page, "macroCondition0Upper")->isVisible() &&
          saved().conditions[0].comparison == "range", "a range shows its upper bound");

    QAction* keyCondition = findAction(addCondition->menu(), QString::fromUtf8("指定按键 / 组合键按下"));
    if (keyCondition) keyCondition->trigger();
    settle();
    auto* keyText = find<QComboBox>(page, "macroCondition1Text");
    check(saved().conditions.size() == 2 && keyText != nullptr, "a key condition has a key chooser");
    if (keyText) {
        keyText->setEditText("LeftControl+U");
        emit keyText->lineEdit()->editingFinished();
        settle();
    }
    check(saved().conditions.size() == 2 && saved().conditions[1].text == "LeftControl+U" &&
          summaryOf(page).contains(QString::fromUtf8("左 Ctrl + U")), "a typed key chord is kept and named");
    click(find<QToolButton>(page, "macroCondition0Remove"));
    check(saved().conditions.size() == 1 && saved().conditions[0].metric == "key", "a condition can be removed");

    // The older target limits.
    find<QCheckBox>(page, "macroTargetOnly")->setChecked(true);
    settle();
    setText(find<QLineEdit>(page, "macroClasses"), QString::fromUtf8("1, 2，3"));
    find<QCheckBox>(page, "macroHeightFilter")->setChecked(true);
    settle();
    check(saved().targetOnly && saved().heightFilter && saved().classes == std::vector<int>({1, 2, 3}),
          "the target limits are saved");
    check(summaryOf(page).contains(QString::fromUtf8("存在符合类别 / 框高限制的目标")), "the limits are in the sentence");

    // 8. Steps.
    click(find<QPushButton>(page, "macroAddWait"));
    click(find<QPushButton>(page, "macroAddKey"));
    click(find<QPushButton>(page, "macroAddMouse"));
    click(find<QPushButton>(page, "macroAddMove"));
    check(saved().actions.size() == 4 && saved().actions[0].type == macros::ActionType::Delay &&
          saved().actions[1].type == macros::ActionType::KeyPress &&
          saved().actions[2].type == macros::ActionType::MouseClick &&
          saved().actions[3].type == macros::ActionType::MouseMove, "the quick buttons add the four common steps");
    // Edits to one card must never undo another card's work.
    check(saved().name == "连点" && saved().trigger == "F1" && saved().conditions.size() == 1 &&
          macros::number(saved(), "cooldown_ms") == 120, "adding steps keeps the trigger and conditions");

    setSpin(find<QSpinBox>(page, "macroAction0A"), 80);
    check(saved().actions[0].a == 80 && saved().actions[0].b >= 80, "a longer minimum wait pulls the maximum up");
    setSpin(find<QSpinBox>(page, "macroAction0B"), 10);
    check(saved().actions[0].b == 80, "the maximum wait never goes below the minimum");
    setSpin(find<QSpinBox>(page, "macroAction0A"), 50);
    setSpin(find<QSpinBox>(page, "macroAction0B"), 50);
    pick(find<QComboBox>(page, "macroAction1Key"), "U");
    setSpin(find<QSpinBox>(page, "macroAction1B"), 20);
    find<QComboBox>(page, "macroAction2Button")->setCurrentIndex(1);
    settle();
    setSpin(find<QSpinBox>(page, "macroAction3A"), 10);
    setSpin(find<QSpinBox>(page, "macroAction3B"), -5);
    check(saved().actions[1].key == "U" && saved().actions[1].b == 20 && saved().actions[2].a == 2 &&
          saved().actions[3].a == 10 && saved().actions[3].b == -5, "step settings are saved");
    check(summaryOf(page).contains(QString::fromUtf8("等待 50 毫秒；然后点按键盘「U」，保持 20 毫秒后松开")) &&
          summaryOf(page).contains(QString::fromUtf8("点击鼠标「右键」")) &&
          summaryOf(page).contains(QString::fromUtf8("鼠标相对移动 X 10 / Y -5")), "the sentence lists the steps in order");

    // With steps present, the sentence also says how the macro repeats and its cooldown.
    find<QComboBox>(page, "macroMode")->setCurrentIndex(1);
    settle();
    check(summaryOf(page).contains(QString::fromUtf8("按住期间每隔 33 毫秒重复一轮")) &&
          summaryOf(page).contains(QString::fromUtf8("触发冷却 120 毫秒")), "the summary mentions repeat interval and cooldown");
    find<QComboBox>(page, "macroMode")->setCurrentIndex(0);
    settle();

    click(find<QToolButton>(page, "macroAction0Down"));
    check(saved().actions[0].type == macros::ActionType::KeyPress && saved().actions[1].type == macros::ActionType::Delay,
          "a step moves down");
    click(find<QToolButton>(page, "macroAction1Up"));
    check(saved().actions[0].type == macros::ActionType::Delay, "a step moves up");
    click(find<QToolButton>(page, "macroAction0Copy"));
    check(saved().actions.size() == 5 && saved().actions[1].type == macros::ActionType::Delay &&
          saved().actions[1] == saved().actions[0], "a step is duplicated right after itself");
    click(find<QToolButton>(page, "macroAction1Remove"));
    check(saved().actions.size() == 4, "a step is deleted");

    // A flow step shows the settings its schema defines.
    find<QComboBox>(page, "macroAction3Type")->setCurrentIndex(12); // loop
    settle();
    check(saved().actions[3].type == macros::ActionType::Loop && saved().actions[3].a == 2,
          "changing a step's type starts from that type's defaults");
    setSpin(find<QSpinBox>(page, "macroAction3Field0"), 5);
    check(saved().actions[3].a == 5, "a schema-described setting is editable in the row");

    // 9. A trigger edit made now still keeps every step and condition.
    setSpin(find<QSpinBox>(page, "macroCooldown"), 250);
    check(saved().actions.size() == 4 && saved().conditions.size() == 1 && macros::number(saved(), "cooldown_ms") == 250,
          "editing the trigger keeps steps and conditions");

    // 10. Stop-all key clash is called out.
    pick(find<QComboBox>(page, "macroTriggerKey"), "F12");
    bool clash = false;
    for (auto* label : page.findChildren<QLabel*>())
        if (label->isVisible() && label->text().contains(QString::fromUtf8("与全部停止键冲突"))) clash = true;
    check(clash, "using the stop-all key as the trigger is flagged");
    pick(find<QComboBox>(page, "macroTriggerKey"), "F1");

    // 11. Complex conditions stay safe: shown read-only and edited in the advanced section.
    {
        macros::Program complex = saved();
        complex.conditions.clear();
        macros::Condition group;
        group.metric = "any";
        group.parent = -1;
        macros::Condition child;
        child.metric = "target.exists";
        child.parent = 0;
        complex.conditions = {group, child};
        config.macro_programs[0] = complex;
        emit ConfigProfiles::instance().configApplied();
        settle();
        bool hasEditLink = false;
        for (auto* button : page.findChildren<QToolButton*>())
            if (button->isVisible() && button->text() == QString::fromUtf8("在高级规则里编辑")) hasEditLink = true;
        check(hasEditLink, "a condition group is shown read-only with a link to the advanced editor");
        check(find<QComboBox>(page, "macroCondition0Metric") == nullptr, "a condition group has no inline editor");
        click(find<QToolButton>(page, "macroCondition0Remove"));
        check(saved().conditions.size() == 2, "a group that still has children cannot be deleted");
        auto* toggle = find<QToolButton>(page, "macroAdvancedToggle");
        for (auto* button : page.findChildren<QToolButton*>())
            if (button->isVisible() && button->text() == QString::fromUtf8("在高级规则里编辑")) { button->click(); break; }
        settle();
        check(toggle && toggle->isChecked(), "the link opens the advanced section");
    }

    // 12. Running and deleting.
    macros::Program runnable = saved();
    runnable.conditions.clear();
    runnable.enabled = true;
    config.macro_programs[0] = runnable;
    emit ConfigProfiles::instance().configApplied();
    wait(250);
    auto* run = find<QPushButton>(page, "macroRunOnce");
    check(run && run->isEnabled(), "an enabled macro with steps can be run once");
    click(run);
    check(macros::testLastRun() == saved().id, "run once asks the engine to run this macro");

    click(find<QPushButton>(page, "macroCopy"));
    check(config.macro_programs.size() == 2 && saved(1).id != saved(0).id && !saved(1).enabled,
          "copy makes a separate, disabled macro");
    click(find<QPushButton>(page, "macroDelete"));
    check(config.macro_programs.size() == 1, "delete removes the selected macro");

    if (argc > 1) {
        page.resize(1200, 1300);
        settle();
        page.grab().save(QString::fromLocal8Bit(argv[1]));
    }
    std::printf("macro page ui: %d failures\n", failures);
    return failures ? 1 : 0;
}
