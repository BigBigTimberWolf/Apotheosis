#include "pages/MacroPage.h"
#include "Apotheosis.h"
#include "config/config_bridge.h"
#include "config/config_profiles.h"
#include "macro/macro_engine.h"
#include "macro/rule_schema.h"
#include "widgets/MacroActionTimeline.h"
#include "widgets/MacroConditionList.h"
#include "widgets/MacroRuleEditor.h"
#include "widgets/MacroSummary.h"
#include "widgets/MacroTriggerCard.h"
#include "widgets/MacroUiCommon.h"
#include "keyboard/hotkey_blocking.h"
#include "widgets/CardWidget.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

namespace {
using macro_ui::q;

enum { EnabledRole = Qt::UserRole + 1, SubtitleRole };

// Two lines per macro: its name, and when it starts ("热键 · F1", "事件 · 失去检测目标").
class MacroListDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        const QString title = item.text;
        item.text.clear();
        const QWidget* widget = item.widget;
        (widget ? widget->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);
        const bool selected = item.state & QStyle::State_Selected;
        const QRect box = option.rect.adjusted(12, 7, -10, -7);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(index.data(EnabledRole).toBool() ? QColor("#53C583") : QColor("#5A5A60"));
        painter->drawEllipse(QPointF(box.left() + 4, box.top() + 9), 4, 4);
        const int left = box.left() + 18;
        QFont titleFont = item.font;
        titleFont.setWeight(QFont::DemiBold);
        painter->setFont(titleFont);
        painter->setPen(item.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
        const QFontMetrics titleMetrics(titleFont);
        painter->drawText(QRect(left, box.top(), box.right() - left, titleMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          titleMetrics.elidedText(title, Qt::ElideRight, box.right() - left));
        QFont subFont = item.font;
        subFont.setPointSizeF(std::max(8.0, subFont.pointSizeF() - 1.5));
        painter->setFont(subFont);
        painter->setPen(selected ? item.palette.color(QPalette::HighlightedText) : QColor("#ABA697"));
        const QFontMetrics subMetrics(subFont);
        const QString sub = index.data(SubtitleRole).toString();
        painter->drawText(QRect(left, box.top() + titleMetrics.height() + 2, box.right() - left, subMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          subMetrics.elidedText(sub, Qt::ElideRight, box.right() - left));
        painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return QSize(option.rect.width(), 52);
    }
};

QString subtitleOf(const macros::Program& p) {
    const std::string event = macros::option(p, "event", "key_down");
    if (macro_summary::isKeyEvent(event))
        return QStringLiteral("热键 · ") + (p.trigger.empty() ? QStringLiteral("未设置")
                                                               : macro_summary::chordName(p.trigger));
    return QStringLiteral("事件 · ") + q(macros::label(macros::events(), event));
}

QString titleOf(const macros::Program& p) {
    const std::string group = macros::option(p, "group");
    return (group.empty() ? QString() : q(group) + QStringLiteral(" / ")) + q(p.name);
}
} // namespace

MacroPage::MacroPage(QWidget* parent):QWidget(parent) {
    auto* outer=new QVBoxLayout(this); outer->setContentsMargins(16,16,16,16); outer->setSpacing(12);
    auto* header=new CardWidget(QStringLiteral("宏编排"),QStringLiteral("keyboard"));
    auto* toolbar=new QHBoxLayout;
    master_=new QCheckBox(QStringLiteral("启用宏编排")); master_->setObjectName("macroMaster"); toolbar->addWidget(master_);
    toolbar->addStretch(); toolbar->addWidget(new QLabel(QStringLiteral("全部停止键")));
    stopKey_=macro_ui::keyCombo(true,false); stopKey_->setMaximumWidth(140); stopKey_->setObjectName("macroStopKey"); toolbar->addWidget(stopKey_);
    auto* stop=new QPushButton(QStringLiteral("■ 全部停止")); toolbar->addWidget(stop);
    header->contentLayout()->addLayout(toolbar);
    status_=macro_ui::hint(QString()); header->contentLayout()->addWidget(status_);
    outer->addWidget(header);

    auto* split=new QSplitter(Qt::Horizontal); outer->addWidget(split,1);
    auto* left=new QWidget; auto* ll=new QVBoxLayout(left); ll->setContentsMargins(0,0,10,0);
    auto* libraryTitle=new QLabel(QStringLiteral("宏方案")); libraryTitle->setStyleSheet("font-weight:600;font-size:15px;");
    ll->addWidget(libraryTitle);
    library_=new QListWidget; library_->setObjectName("macroLibrary"); library_->setItemDelegate(new MacroListDelegate(library_));
    library_->setSpacing(2); ll->addWidget(library_,1);
    auto* libraryTools=new QHBoxLayout;
    auto* add=new QPushButton(QStringLiteral("＋ 新建")); add->setObjectName("macroNew");
    auto* copy=new QPushButton(QStringLiteral("复制")); copy->setObjectName("macroCopy");
    auto* remove=new QPushButton(QStringLiteral("删除")); remove->setObjectName("macroDelete");
    libraryTools->addWidget(add); libraryTools->addWidget(copy); libraryTools->addWidget(remove);
    ll->addLayout(libraryTools);
    library_->setToolTip(QStringLiteral("每个宏独立绑定按键。配置随当前方案保存；本页在前台时暂停热键，切回其他程序即可触发。"));
    left->setMinimumWidth(210); split->addWidget(left);

    scroll_=new QScrollArea; scroll_->setWidgetResizable(true); scroll_->setFrameShape(QFrame::NoFrame);
    auto* body=new QWidget; auto* bl=new QVBoxLayout(body); bl->setContentsMargins(6,0,0,0); bl->setSpacing(12);
    empty_=macro_ui::hint(QStringLiteral("还没有宏。点击左侧“＋ 新建”，选择触发方式，再添加第一个动作。"));
    bl->addWidget(empty_);
    detail_=new QWidget; auto* dl=new QVBoxLayout(detail_); dl->setContentsMargins(0,0,0,0); dl->setSpacing(12); bl->addWidget(detail_);

    auto* summaryFrame=new QFrame; summaryFrame->setObjectName("macroSummaryFrame");
    summaryFrame->setStyleSheet(QStringLiteral("QFrame#macroSummaryFrame{background:#1B1B1F;border:1px solid #2C2C32;"
        "border-left:3px solid #D5B56B;border-radius:8px;}"));
    auto* sl=new QVBoxLayout(summaryFrame); sl->setContentsMargins(14,10,14,10); sl->setSpacing(4);
    auto* summaryTitle=new QLabel(QStringLiteral("这条宏会做什么")); summaryTitle->setStyleSheet("font-weight:600;");
    summary_=new QLabel; summary_->setObjectName("macroSummary"); summary_->setWordWrap(true); summary_->setTextFormat(Qt::PlainText);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sl->addWidget(summaryTitle); sl->addWidget(summary_);
    dl->addWidget(summaryFrame);

    trigger_=new MacroTriggerCard; conditions_=new MacroConditionList; timeline_=new MacroActionTimeline;
    dl->addWidget(trigger_); dl->addWidget(conditions_); dl->addWidget(timeline_);
    trigger_->changed=[this](const macros::Program& p){ edited(p,trigger_); };
    conditions_->changed=[this](const macros::Program& p){ edited(p,conditions_); };
    timeline_->changed=[this](const macros::Program& p){ edited(p,timeline_); };
    conditions_->openAdvanced=[this]{ showAdvanced(true); };

    run_=new QPushButton(QStringLiteral("▶ 运行一次")); run_->setObjectName("macroRunOnce");
    run_->setToolTip(QStringLiteral("会发送真实输入；需要切回目标窗口时，可在第一步添加等待。\n键鼠输出动作暂时接管自动瞄准和扳机；修改瞄准参数的动作保持瞄准运行。移动单位是设备计数；右／下为正。"
        "停止会释放宏按住的键。键盘支持 Windows 原生、KMBox Net 或本项目固件的独立键盘设备。"));
    dl->addWidget(run_,0,Qt::AlignLeft);

    advancedToggle_=new QToolButton; advancedToggle_->setObjectName("macroAdvancedToggle");
    advancedToggle_->setCheckable(true); advancedToggle_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    advancedToggle_->setText(QStringLiteral("▸ 高级规则与调试（条件组、调度、互斥、模拟运行）"));
    dl->addWidget(advancedToggle_,0,Qt::AlignLeft);
    rules_=new MacroRuleEditor; rules_->setEventRowVisible(false); rules_->hide(); dl->addWidget(rules_);
    rules_->changed=[this](const macros::Program& e){
        if(loading_) return;
        if(auto* p=selected()){ p->conditions=e.conditions; p->options=e.options; p->actions=e.actions; save(); syncCards(); refreshSummary(); refreshLibraryItem(); }
    };
    rules_->simulate=[this]{ if(auto* p=selected()) macros::simulate(p->id); };
    bl->addStretch(); scroll_->setWidget(body); split->addWidget(scroll_); split->setStretchFactor(1,1); split->setSizes({240,640});

    connect(stop,&QPushButton::clicked,this,[]{ macros::stopAll(); });
    connect(master_,&QCheckBox::toggled,this,[this]{ if(!loading_) save(); });
    connect(stopKey_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{ if(!loading_){ save(); refreshNotice(); } });
    connect(library_,&QListWidget::currentRowChanged,this,[this]{ if(!loading_) selectProgram(); });
    connect(add,&QPushButton::clicked,this,[this]{ addProgram(false); });
    connect(copy,&QPushButton::clicked,this,[this]{ addProgram(true); });
    connect(remove,&QPushButton::clicked,this,[this]{
        const int row=library_->currentRow(); if(!selected()) return;
        programs_.erase(programs_.begin()+row); save(); rebuildLibrary(std::min(row,static_cast<int>(programs_.size())-1));
    });
    connect(advancedToggle_,&QToolButton::toggled,this,[this](bool on){ showAdvanced(on); });
    connect(run_,&QPushButton::clicked,this,[this]{ if(auto* p=selected()) macros::runOnce(p->id); });
    connect(&ConfigProfiles::instance(),&ConfigProfiles::configApplied,this,&MacroPage::load);
    connect(qApp,&QGuiApplication::applicationStateChanged,this,[this](Qt::ApplicationState state){
        if(isVisible()) macros::setEditing(state==Qt::ApplicationActive);
    });
    auto* timer=new QTimer(this); timer->setInterval(100); connect(timer,&QTimer::timeout,this,&MacroPage::poll); timer->start();
    load();
}

void MacroPage::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    macros::setEditing(QGuiApplication::applicationState()==Qt::ApplicationActive);
    load();
}
void MacroPage::hideEvent(QHideEvent* e) { macros::setEditing(false); QWidget::hideEvent(e); }
macros::Program* MacroPage::selected() {
    const int r=library_->currentRow(); return r>=0 && r<static_cast<int>(programs_.size()) ? &programs_[r] : nullptr;
}
void MacroPage::load() {
    const int row=library_->currentRow(); loading_=true;
    { std::lock_guard<std::recursive_mutex> lock(configMutex);
      programs_=config.macro_programs; master_->setChecked(config.macro_programs_enabled); macro_ui::setKey(stopKey_,config.macro_stop_key); }
    loading_=false; rebuildLibrary(std::max(0,row));
}
void MacroPage::save() {
    if(loading_) return;
    macros::stopAll();
    { std::lock_guard<std::recursive_mutex> lock(configMutex);
      for(auto& p:programs_) macros::normalize(p);
      config.macro_programs=programs_; config.macro_programs_enabled=master_->isChecked();
      config.macro_stop_key=macro_ui::utf8(stopKey_->currentData().toString()); }
    ConfigBridge::instance().markDirty();
    poll();
}
void MacroPage::rebuildLibrary(int row) {
    loading_=true; library_->clear();
    for(const auto& p:programs_){
        auto* item=new QListWidgetItem(titleOf(p)); item->setData(EnabledRole,p.enabled); item->setData(SubtitleRole,subtitleOf(p));
        library_->addItem(item);
    }
    library_->setCurrentRow(std::clamp(row,-1,static_cast<int>(programs_.size())-1));
    loading_=false; selectProgram();
}
void MacroPage::refreshLibraryItem() {
    const auto* p=selected(); auto* item=library_->currentItem(); if(!p||!item) return;
    item->setText(titleOf(*p)); item->setData(EnabledRole,p->enabled); item->setData(SubtitleRole,subtitleOf(*p));
    library_->viewport()->update();
}
void MacroPage::selectProgram() {
    auto* p=selected(); detail_->setVisible(p); empty_->setVisible(!p); if(!p) return;
    syncCards(); refreshSummary(); refreshNotice(); poll();
}
// Every card holds a copy of the whole macro, so after an edit all cards except the
// one that made it (already up to date, and mid-edit) are refreshed.
void MacroPage::syncCards(const QWidget* except) {
    const auto* p=selected(); if(!p) return;
    loading_=true;
    if(except!=trigger_) trigger_->setProgram(*p);
    if(except!=conditions_) conditions_->setProgram(*p);
    if(except!=timeline_) timeline_->setProgram(*p);
    if(rules_->isVisible()) rules_->setProgram(*p);
    loading_=false;
}
void MacroPage::edited(const macros::Program& program,const QWidget* source) {
    if(loading_) return;
    auto* p=selected(); if(!p) return;
    *p=program; save(); syncCards(source); refreshSummary(); refreshLibraryItem(); refreshNotice();
}
void MacroPage::refreshSummary() {
    if(const auto* p=selected()) summary_->setText(macro_summary::describe(*p));
}
// Warn about the two ways a key can clash: with the stop-all key, or with another enabled macro.
void MacroPage::refreshNotice() {
    const auto* p=selected(); if(!p) return;
    QString note;
    const std::string event=macros::option(*p,"event","key_down");
    if(macro_summary::isKeyEvent(event) && !p->trigger.empty()) {
        if(p->trigger==macro_ui::utf8(stopKey_->currentData().toString()))
            note=QStringLiteral("触发键与全部停止键冲突，请修改其中一个。");
        else for(const auto& other:programs_) if(other.id!=p->id && other.enabled && p->enabled && other.trigger==p->trigger){
            note=QStringLiteral("多个宏共用触发键，请在高级规则中设置优先级和互斥。"); break;
        }
    }
    trigger_->setNotice(note);
}
void MacroPage::showAdvanced(bool visible) {
    advancedToggle_->blockSignals(true); advancedToggle_->setChecked(visible); advancedToggle_->blockSignals(false);
    advancedToggle_->setText(visible ? QStringLiteral("▾ 高级规则与调试（条件组、调度、互斥、模拟运行）")
                                     : QStringLiteral("▸ 高级规则与调试（条件组、调度、互斥、模拟运行）"));
    rules_->setVisible(visible);
    if(visible) {
        if(const auto* p=selected()){ loading_=true; rules_->setProgram(*p); loading_=false; }
        QTimer::singleShot(0,this,[this]{ scroll_->ensureWidgetVisible(rules_,0,40); });
    }
}
void MacroPage::addProgram(bool duplicate) {
    if(programs_.size()>=macros::maxPrograms || (duplicate && !selected())) return;
    macros::Program p=duplicate ? *selected() : macros::Program{};
    if(duplicate) p.name+=u8" 副本";
    p.id=macro_ui::utf8(QUuid::createUuid().toString(QUuid::WithoutBraces)); p.enabled=false; p.trigger.clear();
    programs_.push_back(std::move(p)); save(); rebuildLibrary(static_cast<int>(programs_.size())-1);
}
void MacroPage::poll() {
    if(!isVisible()) return;
    const auto s=macros::status();
    rules_->setRuntimeText(QString::fromUtf8(macros::ruleDiagnostics().c_str()));
    trigger_->setBlockStatus(QString::fromUtf8(hotkey_blocking::status().c_str()));
    status_->setText(QString::fromUtf8(s.message.c_str())+(s.running ? QStringLiteral("  ·  步骤 %1 / %2").arg(s.step).arg(s.total) : QString()));
    detail_->setEnabled(!s.running);
    if(auto* p=selected()) run_->setEnabled(master_->isChecked() && p->enabled && !p->actions.empty() && !s.running);
}
