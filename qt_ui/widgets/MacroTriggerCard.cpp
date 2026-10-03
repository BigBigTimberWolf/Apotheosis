#include "widgets/MacroTriggerCard.h"

#include "widgets/FormKit.h"
#include "widgets/MacroSummary.h"
#include "widgets/MacroUiCommon.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace {

using macro_ui::q;
using macro_ui::utf8;

// Events people reach for first; the rest follow after a separator.
const char* const kCommonEvents[] = {"target_found", "target_lost", "aim_start", "aim_stop",
                                     "trigger_start", "trigger_stop"};

bool isCommonEvent(const std::string& id)
{
    return std::find_if(std::begin(kCommonEvents), std::end(kCommonEvents),
                        [&](const char* e) { return id == e; }) != std::end(kCommonEvents);
}

QComboBox* makeTriggerKeyCombo()
{
    auto* combo = macro_ui::keyCombo(true, true);
    combo->setEditable(true);
    combo->addItem(QStringLiteral("滚轮向上"), QStringLiteral("WheelUp"));
    combo->addItem(QStringLiteral("滚轮向下"), QStringLiteral("WheelDown"));
    for (int pad = 0; pad < 4; ++pad)
        for (const char* key : {"A", "B", "X", "Y", "Up", "Down", "Left", "Right", "Start", "Back",
                                "LB", "RB", "LS", "RS", "LT", "RT"}) {
            const QString id = QStringLiteral("Pad%1:%2").arg(pad).arg(QLatin1String(key));
            combo->addItem(id, id);
        }
    combo->setToolTip(QStringLiteral("可选择单键，也可输入 LeftControl+U 组合键或 A>B>C 按键序列。"
                                     "手柄采用 Pad0:A 形式。"));
    return combo;
}

QComboBox* makeWhenCombo()
{
    auto* combo = new QComboBox;
    const std::pair<const char*, const char*> items[] = {
        {"按下时", "key_down"}, {"松开时", "key_up"}, {"按住期间", "key_held"}, {"单击", "click"},
        {"双击", "double"}, {"三击", "triple"}, {"长按", "long"}, {"短按", "short"},
        {"按键序列", "sequence"}};
    for (const auto& item : items)
        combo->addItem(QString::fromUtf8(item.first), QString::fromLatin1(item.second));
    return combo;
}

QComboBox* makeEventCombo()
{
    auto* combo = new QComboBox;
    for (const char* id : kCommonEvents)
        combo->addItem(q(macros::label(macros::events(), id)), QString::fromLatin1(id));
    combo->insertSeparator(combo->count());
    for (const auto& choice : macros::events()) {
        const std::string id = choice.id;
        if (macro_summary::isKeyEvent(id) || isCommonEvent(id)) continue;
        combo->addItem(q(choice.label), q(id));
    }
    return combo;
}

} // namespace

MacroTriggerCard::MacroTriggerCard(QWidget* parent)
    : CardWidget(QStringLiteral("① 触发器 · 什么时候运行"), QStringLiteral("key"), parent)
{
    auto* body = contentLayout();
    body->setSpacing(10);

    name_ = new QLineEdit;
    name_->setMaxLength(48);
    name_->setObjectName("macroName");
    enabled_ = new QCheckBox(QStringLiteral("启用此宏"));
    enabled_->setObjectName("macroEnabled");
    auto* nameLine = new QHBoxLayout;
    nameLine->addWidget(FormKit::fieldRow(QStringLiteral("宏名称"), name_), 1);
    nameLine->addWidget(enabled_);
    body->addLayout(nameLine);

    kind_ = new QComboBox;
    kind_->setObjectName("macroTriggerKind");
    kind_->addItem(QStringLiteral("热键"));
    kind_->addItem(QStringLiteral("运行时事件"));
    trigger_ = makeTriggerKeyCombo();
    trigger_->setObjectName("macroTriggerKey");
    when_ = makeWhenCombo();
    when_->setObjectName("macroTriggerWhen");
    event_ = makeEventCombo();
    event_->setObjectName("macroTriggerEvent");

    auto* keyRowBody = new QWidget;
    auto* keyLayout = new QHBoxLayout(keyRowBody);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->addWidget(FormKit::fieldRow(QStringLiteral("触发键"), trigger_), 2);
    keyLayout->addWidget(FormKit::fieldRow(QStringLiteral("何时触发"), when_), 1);
    keyRow_ = keyRowBody;
    eventRow_ = FormKit::fieldRow(QStringLiteral("事件"), event_);

    auto* triggerLine = new QHBoxLayout;
    triggerLine->addWidget(FormKit::fieldRow(QStringLiteral("触发方式"), kind_), 1);
    triggerLine->addWidget(keyRow_, 3);
    triggerLine->addWidget(eventRow_, 3);
    body->addLayout(triggerLine);

    mode_ = new QComboBox;
    mode_->setObjectName("macroMode");
    mode_->addItems(macro_ui::modeNames());
    interval_ = macro_ui::spin(1, 60000, QStringLiteral(" ms"));
    interval_->setObjectName("macroInterval");
    cooldown_ = macro_ui::spin(0, 600000, QStringLiteral(" ms"));
    cooldown_->setObjectName("macroCooldown");
    cooldown_->setToolTip(QStringLiteral("从一次成功启动起，这段时间内不会再次触发。0 表示不限制。"));
    intervalRow_ = FormKit::fieldRow(QStringLiteral("循环间隔"), interval_);
    auto* modeLine = new QHBoxLayout;
    modeLine->addWidget(FormKit::fieldRow(QStringLiteral("执行方式"), mode_), 3);
    modeLine->addWidget(intervalRow_, 2);
    modeLine->addWidget(FormKit::fieldRow(QStringLiteral("冷却"), cooldown_), 2);
    body->addLayout(modeLine);

    block_ = new QCheckBox(QStringLiteral("屏蔽触发键（只用于触发宏）"));
    block_->setObjectName("macroBlock");
    block_->setToolTip(QStringLiteral("宏启用后屏蔽原始触发键，宏动作正常输出；条件未满足时也会屏蔽。"
        "鼠标已接入所有输入方式，MAKCU 自定义旧固件需更新。键盘支持本机，以及 KMBox Net / Ferrum "
        "接入的被控端键盘；其他硬件的独立键盘尚不支持。修改后请松开再按。"));
    body->addWidget(block_);
    blockStatus_ = macro_ui::hint(QString());
    body->addWidget(blockStatus_);
    modeHelp_ = macro_ui::hint(QString());
    body->addWidget(modeHelp_);
    notice_ = macro_ui::hint(QString());
    notice_->setStyleSheet(QStringLiteral("color:#E0A458;font-size:12px;"));
    notice_->hide();
    body->addWidget(notice_);

    connect(enabled_, &QCheckBox::toggled, this, [this] { read(); });
    connect(block_, &QCheckBox::toggled, this, [this] { read(); });
    connect(name_, &QLineEdit::editingFinished, this, [this] { read(); });
    connect(kind_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { switchKind(); });
    for (auto* combo : {when_, event_, mode_})
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { read(); });
    connect(trigger_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { read(); });
    connect(trigger_->lineEdit(), &QLineEdit::editingFinished, this, [this] { read(); });
    for (auto* box : {interval_, cooldown_})
        connect(box, &QSpinBox::editingFinished, this, [this] { read(); });
}

bool MacroTriggerCard::keyKind() const
{
    return kind_->currentIndex() == 0;
}

void MacroTriggerCard::setProgram(const macros::Program& program)
{
    loading_ = true;
    program_ = program;
    name_->setText(q(program.name));
    enabled_->setChecked(program.enabled);

    const std::string event = macros::option(program, "event", "key_down");
    const bool isKey = macro_summary::isKeyEvent(event);
    kind_->setCurrentIndex(isKey ? 0 : 1);
    if (isKey) {
        when_->setCurrentIndex(std::max(0, when_->findData(q(event))));
    } else {
        int index = event_->findData(q(event));
        if (index < 0) { // an event this page does not list: keep it rather than lose it
            event_->addItem(q(event), q(event));
            index = event_->count() - 1;
        }
        event_->setCurrentIndex(index);
    }
    macro_ui::setKey(trigger_, program.trigger);
    mode_->setCurrentIndex(std::clamp(static_cast<int>(program.mode), 0, mode_->count() - 1));
    interval_->setValue(program.loopIntervalMs);
    cooldown_->setValue(macros::number(program, "cooldown_ms"));
    block_->setChecked(program.blockTrigger);
    loading_ = false;
    refresh();
}

void MacroTriggerCard::refresh()
{
    const bool isKey = keyKind();
    keyRow_->setVisible(isKey);
    eventRow_->setVisible(!isKey);
    block_->setVisible(isKey);
    blockStatus_->setVisible(isKey);
    const auto mode = static_cast<macros::Mode>(std::clamp(mode_->currentIndex(), 0, 3));
    intervalRow_->setVisible(mode == macros::Mode::Hold || mode == macros::Mode::Toggle);

    QString help; // "run once" needs no explanation; the others do
    if (mode == macros::Mode::Hold)
        help = QStringLiteral("按住触发键期间循环执行，松开立即停止；每轮之间的间隔见“循环间隔”。");
    else if (mode == macros::Mode::Toggle)
        help = QStringLiteral("按一次开始循环执行，再按一次（或按全部停止键）停止；每轮之间的间隔见“循环间隔”。");
    else if (mode == macros::Mode::Sequence)
        help = QStringLiteral("每次触发只执行下一步，全部停止键可中断。");
    modeHelp_->setText(help);
    modeHelp_->setVisible(!help.isEmpty());
    const bool sequence = isKey && when_->currentData().toString() == QStringLiteral("sequence");
    trigger_->setToolTip(sequence
        ? QStringLiteral("按键序列用 > 连接，例如 A>B>C：依次按下这些键才会触发。")
        : QStringLiteral("可选择单键，也可输入 LeftControl+U 组合键。手柄采用 Pad0:A 形式。"));
}

void MacroTriggerCard::switchKind()
{
    if (loading_) return;
    loading_ = true; // pick a sensible default for the new kind without firing read() twice
    if (keyKind()) when_->setCurrentIndex(0);
    else event_->setCurrentIndex(0);
    loading_ = false;
    refresh();
    read();
}

void MacroTriggerCard::read()
{
    if (loading_) return;
    program_.name = utf8(name_->text().trimmed());
    if (program_.name.empty()) program_.name = u8"未命名宏";
    program_.enabled = enabled_->isChecked();
    const bool isKey = keyKind();
    const QString event = (isKey ? when_ : event_)->currentData().toString();
    if (!event.isEmpty()) program_.options["event"] = utf8(event);
    // An event macro ignores the key, so the stored one is left untouched there.
    if (isKey) program_.trigger = macro_ui::currentKey(trigger_);
    program_.mode = static_cast<macros::Mode>(std::clamp(mode_->currentIndex(), 0, 3));
    program_.loopIntervalMs = interval_->value();
    if (cooldown_->value() > 0 || program_.options.count("cooldown_ms"))
        program_.options["cooldown_ms"] = std::to_string(cooldown_->value());
    program_.blockTrigger = block_->isChecked();
    refresh();
    if (changed) changed(program_);
}

void MacroTriggerCard::setNotice(const QString& text)
{
    notice_->setText(text);
    notice_->setVisible(!text.isEmpty());
}

void MacroTriggerCard::setBlockStatus(const QString& text)
{
    if (blockStatus_->text() != text) blockStatus_->setText(text);
}
