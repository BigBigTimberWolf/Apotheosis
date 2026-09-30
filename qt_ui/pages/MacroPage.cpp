#include "pages/MacroPage.h"
#include "Apotheosis.h"
#include "config/config_bridge.h"
#include "config/config_profiles.h"
#include "macro/macro_engine.h"
#include "keyboard/hotkey_blocking.h"
#include "widgets/CardWidget.h"
#include "widgets/FormKit.h"
#include <QCheckBox>
#include <QApplication>
#include <QComboBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QShowEvent>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {
using AT=macros::ActionType;
const QStringList actionNames={QStringLiteral("等待"),QStringLiteral("键盘按住"),
    QStringLiteral("键盘松开"),QStringLiteral("键盘点按"),QStringLiteral("鼠标移动"),
    QStringLiteral("鼠标按住"),QStringLiteral("鼠标松开"),QStringLiteral("鼠标点击"),
    QStringLiteral("滚轮"),QStringLiteral("重置 PID · XY"),QStringLiteral("重置 PID · X"),QStringLiteral("重置 PID · Y")};
const QStringList buttonNames={QStringLiteral("左键"),QStringLiteral("右键"),QStringLiteral("中键"),
    QStringLiteral("侧键 4"),QStringLiteral("侧键 5")};
const QStringList modeNames={QStringLiteral("按一次，执行一轮"),QStringLiteral("按住循环，松开停止"),
    QStringLiteral("按一次开始，再按停止"),QStringLiteral("每按一次，执行下一步")};
class Spin : public QSpinBox {
public: using QSpinBox::QSpinBox;
protected: void wheelEvent(QWheelEvent* e) override {
    if(!hasFocus()) e->ignore(); else QSpinBox::wheelEvent(e);
}
};
QSpinBox* spin(int low,int high,const QString& suffix={}) {
    auto* s=new Spin; s->setRange(low,high); s->setSuffix(suffix); return s;
}
QLabel* hint(const QString& text) {
    auto* l=new QLabel(text); l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    l->setStyleSheet("color:#64748b;font-size:12px;"); return l;
}
QComboBox* keyCombo(bool mouse,bool none) {
    auto* c=new QComboBox;
    if(none) c->addItem(QStringLiteral("未设置"),QString());
    if(mouse) {
        const char* ids[]={"LeftMouseButton","RightMouseButton","MiddleMouseButton","X1MouseButton","X2MouseButton"};
        for(int i=0;i<5;++i) c->addItem(QStringLiteral("鼠标 · ")+buttonNames[i],QString::fromLatin1(ids[i]));
    }
    for(const auto& k:macros::keys()) c->addItem(QString::fromUtf8(k.label.c_str()),QString::fromStdString(k.id));
    return c;
}
void setKey(QComboBox* c,const std::string& key) {
    c->setCurrentIndex(c->findData(QString::fromStdString(key)));
}
macros::Action defaultAction(int type) {
    macros::Action a; a.type=static_cast<AT>(type);
    if(a.type==AT::MouseDown || a.type==AT::MouseUp || a.type==AT::MouseClick) { a.a=1; a.b=30; }
    else if(a.type==AT::MouseMove) a.a=a.b=0;
    else if(a.type==AT::KeyPress) a.b=30;
    else if(a.type==AT::Wheel) a.a=1;
    return a;
}
QString description(const macros::Action& a) {
    switch(a.type) {
    case AT::Delay: return a.a==a.b ? QStringLiteral("%1 ms").arg(a.a) : QStringLiteral("随机 %1～%2 ms").arg(a.a).arg(a.b);
    case AT::MouseMove: return QStringLiteral("X %1 / Y %2 计数").arg(a.a).arg(a.b);
    case AT::MouseDown: case AT::MouseUp: case AT::MouseClick:
        return buttonNames.value(a.a-1,QStringLiteral("无效按键"))+
            (a.type==AT::MouseClick ? QStringLiteral(" · %1 ms").arg(a.b) : QString());
    case AT::KeyDown: case AT::KeyUp: case AT::KeyPress: {
        QString label=QString::fromStdString(a.key);
        for(const auto& k:macros::keys()) if(k.id==a.key) label=QString::fromUtf8(k.label.c_str());
        return label+(a.type==AT::KeyPress ? QStringLiteral(" · %1 ms").arg(a.b) : QString());
    }
    case AT::Wheel: return QStringLiteral("%1 格（正数向上）").arg(a.a);
    default: return QStringLiteral("清空对应轴的 PID 状态");
    }
}
} // namespace

MacroPage::MacroPage(QWidget* parent):QWidget(parent) {
    auto* outer=new QVBoxLayout(this); outer->setContentsMargins(16,16,16,16); outer->setSpacing(12);
    auto* header=new CardWidget(QStringLiteral("宏编排"),QStringLiteral("keyboard"));
    auto* toolbar=new QHBoxLayout;
    master_=new QCheckBox(QStringLiteral("启用宏编排")); toolbar->addWidget(master_);
    toolbar->addStretch(); toolbar->addWidget(new QLabel(QStringLiteral("全部停止键")));
    stopKey_=keyCombo(true,false); stopKey_->setMaximumWidth(140); toolbar->addWidget(stopKey_);
    auto* stop=new QPushButton(QStringLiteral("■ 全部停止")); toolbar->addWidget(stop);
    header->contentLayout()->addLayout(toolbar);
    status_=hint(QString()); header->contentLayout()->addWidget(status_);
    outer->addWidget(header);

    auto* split=new QSplitter(Qt::Horizontal); outer->addWidget(split,1);
    auto* left=new QWidget; auto* ll=new QVBoxLayout(left); ll->setContentsMargins(0,0,10,0);
    auto* libraryTitle=new QLabel(QStringLiteral("我的宏")); libraryTitle->setStyleSheet("font-weight:600;font-size:15px;");
    ll->addWidget(libraryTitle);
    library_=new QListWidget; library_->setSpacing(4); ll->addWidget(library_,1);
    auto* libraryTools=new QHBoxLayout;
    auto* add=new QPushButton(QStringLiteral("＋ 新建"));
    auto* copy=new QPushButton(QStringLiteral("复制"));
    auto* remove=new QPushButton(QStringLiteral("删除"));
    libraryTools->addWidget(add); libraryTools->addWidget(copy); libraryTools->addWidget(remove);
    ll->addLayout(libraryTools);
    library_->setToolTip(QStringLiteral("每个宏独立绑定按键。配置随当前方案保存；本页在前台时暂停热键，切回其他程序即可触发。"));
    left->setMinimumWidth(190); split->addWidget(left);

    auto* scroll=new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto* body=new QWidget; auto* bl=new QVBoxLayout(body); bl->setContentsMargins(6,0,0,0); bl->setSpacing(12);
    empty_=hint(QStringLiteral("未选择宏"));
    empty_->setToolTip(QStringLiteral("从左侧新建一个宏，再依次添加动作。例如：键盘点按 → 等待 → 鼠标点击。"));
    bl->addWidget(empty_);
    detail_=new QWidget; auto* dl=new QVBoxLayout(detail_); dl->setContentsMargins(0,0,0,0); dl->setSpacing(12); bl->addWidget(detail_);

    auto* settings=new CardWidget(QStringLiteral("触发与条件")); auto* sl=settings->contentLayout();
    name_=new QLineEdit; name_->setMaxLength(48);
    enabled_=new QCheckBox(QStringLiteral("启用此宏"));
    sl->addWidget(FormKit::fieldRow(QStringLiteral("名称"),name_)); sl->addWidget(enabled_);
    trigger_=keyCombo(true,true); mode_=new QComboBox; mode_->addItems(modeNames);
    sl->addWidget(FormKit::fieldRow(QStringLiteral("触发按键"),trigger_));
    blockTrigger_=new QCheckBox(QStringLiteral("屏蔽触发键（只用于触发宏）"));
    sl->addWidget(blockTrigger_);
    blockTrigger_->setToolTip(QStringLiteral("宏启用后屏蔽原始触发键，宏动作正常输出；目标条件未满足也会屏蔽。鼠标已接入所有输入方式，MAKCU 自定义旧固件需更新。键盘支持本机，以及 KMBox Net / Ferrum 接入的被控端键盘；其他硬件的独立键盘尚不支持。修改后请松开再按。"));
    blockStatus_=hint(QString()); sl->addWidget(blockStatus_);
    sl->addWidget(FormKit::fieldRow(QStringLiteral("执行方式"),mode_));
    modeHelp_=hint(QString()); sl->addWidget(modeHelp_);
    interval_=spin(1,60000,QStringLiteral(" ms"));
    sl->addWidget(FormKit::fieldRow(QStringLiteral("每轮间隔"),interval_));
    target_=new QCheckBox(QStringLiteral("仅检测到符合条件的目标时执行")); sl->addWidget(target_);
    classes_=new QLineEdit; classes_->setPlaceholderText(QStringLiteral("留空表示任意类别；多个 ID 用逗号分隔"));
    classes_->setMaxLength(256);
    classes_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9,，\\s]*")),classes_));
    sl->addWidget(FormKit::fieldRow(QStringLiteral("目标类别 ID"),classes_));
    height_=new QCheckBox(QStringLiteral("限制目标框高度占画面的比例")); sl->addWidget(height_);
    auto* range=new QWidget; auto* rl=new QHBoxLayout(range); rl->setContentsMargins(0,0,0,0);
    minHeight_=spin(0,100,QStringLiteral(" %")); maxHeight_=spin(0,100,QStringLiteral(" %"));
    rl->addWidget(minHeight_); rl->addWidget(new QLabel(QStringLiteral("至"))); rl->addWidget(maxHeight_);
    sl->addWidget(FormKit::fieldRow(QStringLiteral("框高范围"),range));
    dl->addWidget(settings);

    auto* flow=new CardWidget(QStringLiteral("动作顺序")); auto* fl=flow->contentLayout();
    auto* actionTools=new QHBoxLayout;
    addType_=new QComboBox; addType_->addItems(actionNames); actionTools->addWidget(addType_,1);
    auto* addAction=new QPushButton(QStringLiteral("＋ 添加动作")); actionTools->addWidget(addAction);
    fl->addLayout(actionTools);
    actions_=new QTableWidget(0,3); actions_->setHorizontalHeaderLabels({QStringLiteral("步骤"),QStringLiteral("动作"),QStringLiteral("内容")});
    actions_->verticalHeader()->hide(); actions_->setSelectionBehavior(QAbstractItemView::SelectRows);
    actions_->setSelectionMode(QAbstractItemView::SingleSelection); actions_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    actions_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);
    actions_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
    actions_->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Stretch);
    actions_->setMinimumHeight(190); fl->addWidget(actions_);
    auto* rowTools=new QHBoxLayout;
    auto* up=new QPushButton(QStringLiteral("↑ 上移")); auto* down=new QPushButton(QStringLiteral("↓ 下移"));
    auto* duplicateAction=new QPushButton(QStringLiteral("复制步骤")); auto* deleteAction=new QPushButton(QStringLiteral("删除步骤"));
    rowTools->addWidget(up); rowTools->addWidget(down); rowTools->addStretch(); rowTools->addWidget(duplicateAction); rowTools->addWidget(deleteAction);
    fl->addLayout(rowTools);

    inspector_=new QWidget; auto* il=new QVBoxLayout(inspector_); il->setContentsMargins(0,8,0,0);
    type_=new QComboBox; type_->addItems(actionNames); il->addWidget(FormKit::fieldRow(QStringLiteral("选中步骤"),type_));
    key_=keyCombo(false,false); keyRow_=FormKit::fieldRow(QStringLiteral("键盘按键"),key_); il->addWidget(keyRow_);
    button_=new QComboBox; button_->addItems(buttonNames); buttonRow_=FormKit::fieldRow(QStringLiteral("鼠标按键"),button_); il->addWidget(buttonRow_);
    auto valueRow=[&](QWidget*& row,QLabel*& label,QSpinBox*& field) {
        row=new QWidget; auto* l=new QHBoxLayout(row); l->setContentsMargins(0,0,0,0);
        label=new QLabel; field=spin(-32767,60000); l->addWidget(label); l->addStretch(); l->addWidget(field); il->addWidget(row);
    };
    valueRow(aRow_,aLabel_,a_); valueRow(bRow_,bLabel_,b_);
    actionHint_=hint(QString()); actionHint_->setParent(inspector_); actionHint_->hide(); fl->addWidget(inspector_);
    dl->addWidget(flow);
    run_=new QPushButton(QStringLiteral("▶ 运行一次"));
    run_->setToolTip(QStringLiteral("会发送真实输入；需要切回目标窗口时，可在第一步添加等待。"));
    dl->addWidget(run_);
    run_->setToolTip(run_->toolTip() + QStringLiteral("\n运行时暂时接管自动瞄准和扳机输出。移动单位是设备计数；右／下为正。"
        "停止会释放宏按住的键。键盘支持 Windows 原生、KMBox Net 或本项目固件的独立键盘设备。"));
    bl->addStretch(); scroll->setWidget(body); split->addWidget(scroll); split->setStretchFactor(1,1); split->setSizes({220,620});

    connect(stop,&QPushButton::clicked,this,[]{ macros::stopAll(); });
    connect(master_,&QCheckBox::toggled,this,[this]{ if(!loading_) save(); });
    connect(stopKey_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{ if(!loading_) save(); });
    connect(library_,&QListWidget::currentRowChanged,this,[this]{ if(!loading_) selectProgram(); });
    connect(add,&QPushButton::clicked,this,[this]{ addProgram(false); });
    connect(copy,&QPushButton::clicked,this,[this]{ addProgram(true); });
    connect(remove,&QPushButton::clicked,this,[this]{
        const int row=library_->currentRow(); if(!selected()) return;
        programs_.erase(programs_.begin()+row); save(); rebuildLibrary(std::min(row,static_cast<int>(programs_.size())-1));
    });
    connect(name_,&QLineEdit::editingFinished,this,&MacroPage::readSettings);
    connect(classes_,&QLineEdit::editingFinished,this,&MacroPage::readSettings);
    for(auto* c:{enabled_,target_,height_,blockTrigger_}) connect(c,&QCheckBox::toggled,this,&MacroPage::readSettings);
    for(auto* c:{trigger_,mode_}) connect(c,QOverload<int>::of(&QComboBox::currentIndexChanged),this,&MacroPage::readSettings);
    for(auto* s:{interval_,minHeight_,maxHeight_}) connect(s,&QSpinBox::editingFinished,this,&MacroPage::readSettings);
    connect(actions_,&QTableWidget::itemSelectionChanged,this,[this]{ if(!loading_) selectAction(); });
    connect(addAction,&QPushButton::clicked,this,[this]{ if(auto* p=selected();p && p->actions.size()<macros::maxActions) {
        p->actions.push_back(defaultAction(addType_->currentIndex())); save(); rebuildActions(static_cast<int>(p->actions.size())-1);
    }});
    connect(up,&QPushButton::clicked,this,[this]{ moveAction(-1); });
    connect(down,&QPushButton::clicked,this,[this]{ moveAction(1); });
    connect(duplicateAction,&QPushButton::clicked,this,[this]{ if(auto* p=selected();p && selectedAction() && p->actions.size()<macros::maxActions) {
        const int r=actions_->currentRow(); const auto a=p->actions[r]; p->actions.insert(p->actions.begin()+r+1,a); save(); rebuildActions(r+1);
    }});
    connect(deleteAction,&QPushButton::clicked,this,[this]{ if(auto* p=selected();p && selectedAction()) {
        const int r=actions_->currentRow(); p->actions.erase(p->actions.begin()+r); save(); rebuildActions(std::min(r,static_cast<int>(p->actions.size())-1));
    }});
    connect(type_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int t){
        if(loading_) return; if(auto* a=selectedAction()) { *a=defaultAction(t); save(); rebuildActions(actions_->currentRow()); }
    });
    for(auto* c:{key_,button_}) connect(c,QOverload<int>::of(&QComboBox::currentIndexChanged),this,&MacroPage::readAction);
    for(auto* s:{a_,b_}) connect(s,&QSpinBox::editingFinished,this,&MacroPage::readAction);
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
macros::Action* MacroPage::selectedAction() {
    auto* p=selected(); const int r=actions_->currentRow();
    return p && r>=0 && r<static_cast<int>(p->actions.size()) ? &p->actions[r] : nullptr;
}
void MacroPage::load() {
    const int row=library_->currentRow(); loading_=true;
    { std::lock_guard<std::recursive_mutex> lock(configMutex);
      programs_=config.macro_programs; master_->setChecked(config.macro_programs_enabled); setKey(stopKey_,config.macro_stop_key); }
    loading_=false; rebuildLibrary(std::max(0,row));
}
void MacroPage::save() {
    if(loading_) return;
    macros::stopAll();
    { std::lock_guard<std::recursive_mutex> lock(configMutex);
      for(auto& p:programs_) macros::normalize(p);
      config.macro_programs=programs_; config.macro_programs_enabled=master_->isChecked();
      config.macro_stop_key=stopKey_->currentData().toString().toStdString(); }
    ConfigBridge::instance().markDirty();
    poll();
}
void MacroPage::rebuildLibrary(int row) {
    loading_=true; library_->clear();
    for(const auto& p:programs_) library_->addItem((p.enabled ? QStringLiteral("●  ") : QStringLiteral("○  "))+QString::fromUtf8(p.name.c_str()));
    library_->setCurrentRow(std::clamp(row,-1,static_cast<int>(programs_.size())-1));
    loading_=false; selectProgram();
}
void MacroPage::selectProgram() {
    auto* p=selected(); detail_->setVisible(p); empty_->setVisible(!p); if(!p) return;
    loading_=true; name_->setText(QString::fromUtf8(p->name.c_str())); enabled_->setChecked(p->enabled);
    setKey(trigger_,p->trigger); mode_->setCurrentIndex(static_cast<int>(p->mode)); interval_->setValue(p->loopIntervalMs);
    target_->setChecked(p->targetOnly); height_->setChecked(p->heightFilter);
    blockTrigger_->setChecked(p->blockTrigger);
    minHeight_->setValue(p->minHeightPercent); maxHeight_->setValue(p->maxHeightPercent);
    QStringList ids; for(int id:p->classes) ids<<QString::number(id); classes_->setText(ids.join(", "));
    classes_->setEnabled(p->targetOnly); height_->setEnabled(p->targetOnly);
    minHeight_->setEnabled(p->targetOnly && p->heightFilter); maxHeight_->setEnabled(p->targetOnly && p->heightFilter);
    interval_->setEnabled(p->mode==macros::Mode::Hold || p->mode==macros::Mode::Toggle);
    const QString modeHelp = p->mode==macros::Mode::Sequence ?
        QStringLiteral("每次按键推进一个动作；按住动作可跨步骤保持。最后一步、停止或条件失效时统一松开。") :
        QStringLiteral("不需要按住瞄准键。循环每轮结束会释放未配对的按住动作；目标条件失效立即停止。");
    mode_->setToolTip(modeHelp);
    modeHelp_->clear();
    if(!p->trigger.empty() && p->trigger==stopKey_->currentData().toString().toStdString())
        modeHelp_->setText(QStringLiteral("触发键与全部停止键相同，请修改其中一个。"));
    else for(const auto& other:programs_) if(other.id!=p->id && other.enabled && p->enabled && !p->trigger.empty() && other.trigger==p->trigger)
        modeHelp_->setText(QStringLiteral("此触发键已被其他启用的宏占用，请设置不同按键。"));
    loading_=false; rebuildActions(0); poll();
}
void MacroPage::readSettings() {
    if(loading_) return; auto* p=selected(); if(!p) return;
    p->name=name_->text().trimmed().toUtf8().toStdString(); if(p->name.empty()) p->name=u8"未命名宏";
    p->enabled=enabled_->isChecked(); p->trigger=trigger_->currentData().toString().toStdString();
    p->mode=static_cast<macros::Mode>(mode_->currentIndex()); p->loopIntervalMs=interval_->value();
    p->targetOnly=target_->isChecked(); p->heightFilter=height_->isChecked();
    p->blockTrigger=blockTrigger_->isChecked();
    p->minHeightPercent=minHeight_->value(); p->maxHeightPercent=maxHeight_->value();
    p->classes.clear(); for(const auto& text:classes_->text().split(QRegularExpression("[,，\\s]+"),Qt::SkipEmptyParts)) {
        bool ok=false; const int id=text.toInt(&ok); if(ok && id>=0) p->classes.push_back(id);
    }
    save(); rebuildLibrary(library_->currentRow());
}
void MacroPage::rebuildActions(int row) {
    loading_=true; actions_->setRowCount(0);
    if(auto* p=selected()) for(size_t i=0;i<p->actions.size();++i) {
        const auto& a=p->actions[i]; const int r=actions_->rowCount(); actions_->insertRow(r);
        actions_->setItem(r,0,new QTableWidgetItem(QString::number(static_cast<int>(i+1))));
        actions_->setItem(r,1,new QTableWidgetItem(actionNames.value(static_cast<int>(a.type),QStringLiteral("未知动作"))));
        actions_->setItem(r,2,new QTableWidgetItem(description(a)));
    }
    if(row>=0 && row<actions_->rowCount()) actions_->selectRow(row);
    loading_=false; selectAction();
}
void MacroPage::selectAction() {
    const auto* a=selectedAction(); inspector_->setVisible(a); if(!a) return;
    loading_=true; type_->setCurrentIndex(static_cast<int>(a->type)); setKey(key_,a->key); button_->setCurrentIndex(a->a-1);
    const bool delay=a->type==AT::Delay, move=a->type==AT::MouseMove, wheel=a->type==AT::Wheel;
    const bool keyboard=a->type==AT::KeyDown || a->type==AT::KeyUp || a->type==AT::KeyPress;
    const bool mouse=a->type==AT::MouseDown || a->type==AT::MouseUp || a->type==AT::MouseClick;
    const bool click=a->type==AT::KeyPress || a->type==AT::MouseClick;
    keyRow_->setVisible(keyboard); buttonRow_->setVisible(mouse); aRow_->setVisible(delay||move||wheel); bRow_->setVisible(delay||move||click);
    a_->setRange(move ? -32767 : wheel ? -127 : 0,move ? 32767 : wheel ? 127 : 60000);
    b_->setRange(move ? -32767 : click ? 1 : 0,move ? 32767 : click ? 2000 : 60000);
    a_->setSuffix(delay ? " ms" : wheel ? QStringLiteral(" 格") : QStringLiteral(" 计数"));
    b_->setSuffix(move ? QStringLiteral(" 计数") : " ms"); a_->setValue(a->a); b_->setValue(a->b);
    aLabel_->setText(delay ? QStringLiteral("最短等待") : move ? QStringLiteral("X 位移") : QStringLiteral("滚动量"));
    bLabel_->setText(delay ? QStringLiteral("最长等待") : move ? QStringLiteral("Y 位移") : QStringLiteral("按下时长"));
    actionHint_->setText(delay ? QStringLiteral("两个值相同为固定延时，不同则在区间内随机等待。") :
        keyboard ? QStringLiteral("支持 Windows 原生、KMBox Net 和本项目固件的独立 MAKCU 键盘设备。组合键：按住 Ctrl → 点按其他键 → 松开 Ctrl；最多同时按住 6 个普通键。") :
        mouse ? QStringLiteral("侧键输出支持 Windows 原生、MAKCU、MAKCUNEW、KMBox Net。停止宏时释放宏按住的按钮。") :
        move ? QStringLiteral("相对设备位移，不是画面像素；X 正数向右，Y 正数向下。") :
        QStringLiteral("选择动作并设置参数，修改会自动保存到当前配置。"));
    type_->setToolTip(actionHint_->text());
    loading_=false;
}
void MacroPage::readAction() {
    if(loading_) return; auto* a=selectedAction(); if(!a) return;
    a->key=key_->currentData().toString().toStdString();
    if(a->type==AT::MouseDown || a->type==AT::MouseUp || a->type==AT::MouseClick) a->a=button_->currentIndex()+1;
    else a->a=a_->value();
    a->b=b_->value(); const int row=actions_->currentRow(); save(); rebuildActions(row);
}
void MacroPage::addProgram(bool duplicate) {
    if(programs_.size()>=macros::maxPrograms || (duplicate && !selected())) return;
    macros::Program p=duplicate ? *selected() : macros::Program{};
    if(duplicate) p.name+=u8" 副本";
    p.id=QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(); p.enabled=false; p.trigger.clear();
    programs_.push_back(std::move(p)); save(); rebuildLibrary(static_cast<int>(programs_.size())-1);
}
void MacroPage::moveAction(int delta) {
    auto* p=selected(); const int row=actions_->currentRow(), next=row+delta;
    if(!p || row<0 || next<0 || next>=static_cast<int>(p->actions.size())) return;
    std::swap(p->actions[row],p->actions[next]); save(); rebuildActions(next);
}
void MacroPage::poll() {
    if(!isVisible()) return;
    const auto s=macros::status();
    blockStatus_->setText(QString::fromUtf8(hotkey_blocking::status().c_str()));
    status_->setText(QString::fromUtf8(s.message.c_str())+(s.running ? QStringLiteral("  ·  步骤 %1 / %2").arg(s.step).arg(s.total) : QString()));
    detail_->setEnabled(!s.running);
    if(auto* p=selected()) run_->setEnabled(master_->isChecked() && p->enabled && !p->actions.empty() && !s.running);
}
