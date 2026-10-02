#include "MacroRuleEditor.h"
#include "widgets/FormKit.h"
#include <QComboBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
namespace {
QString q(const std::string& s){return QString::fromUtf8(s.c_str());}
QSpinBox* integer(int low,int high){auto* w=new QSpinBox;w->setRange(low,high);return w;}
QDoubleSpinBox* real(){auto* w=new QDoubleSpinBox;w->setRange(-1000000000,1000000000);w->setDecimals(4);return w;}
}
MacroRuleEditor::MacroRuleEditor(QWidget* parent):QWidget(parent) {
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);
    event_=new QComboBox;for(const auto& c:macros::events())event_->addItem(q(c.label),q(c.id));
    layout->addWidget(FormKit::fieldRow(QStringLiteral("触发事件"),event_));
    auto* tabs=new QTabWidget;layout->addWidget(tabs);
    auto* panel=new QWidget;auto* body=new QVBoxLayout(panel);
    auto* help=new QLabel(QStringLiteral("默认全部条件同时满足。添加 AND / OR / NOT 等条件组，再把子条件的“父组编号”指向它，可组合嵌套逻辑。"));help->setWordWrap(true);body->addWidget(help);
    conditions_=new QTableWidget(0,3);conditions_->setHorizontalHeaderLabels({QStringLiteral("编号"),QStringLiteral("条件 / 条件组"),QStringLiteral("规则")});
    conditions_->setSelectionBehavior(QAbstractItemView::SelectRows);conditions_->setSelectionMode(QAbstractItemView::SingleSelection);
    conditions_->setEditTriggers(QAbstractItemView::NoEditTriggers);conditions_->verticalHeader()->hide();
    conditions_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);
    conditions_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
    conditions_->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Stretch);conditions_->setMinimumHeight(160);body->addWidget(conditions_);
    auto* tools=new QHBoxLayout;auto* add=new QPushButton(QStringLiteral("＋ 添加条件"));auto* remove=new QPushButton(QStringLiteral("删除条件"));
    tools->addWidget(add);tools->addWidget(remove);tools->addStretch();body->addLayout(tools);
    metric_=new QComboBox;for(const auto& c:macros::metrics())metric_->addItem(q(c.label),q(c.id));
    comparison_=new QComboBox;const std::pair<const char*,const char*> ops[]={{"==","等于"},{"!=","不等于"},{">","大于"},{">=","大于等于"},
        {"<","小于"},{"<=","小于等于"},{"range","区间（含边界）"},{"contains","文本包含"},{"text_eq","文本相同"},{"text_ne","文本不同"}};
    for(const auto& op:ops)comparison_->addItem(q(op.second),q(op.first));
    value_=real();upper_=real();class_=integer(-1,99999);parent_=integer(-1,macros::maxConditions);
    text_=new QLineEdit;text_->setMaxLength(4096);
    body->addWidget(FormKit::fieldRow(QStringLiteral("判断内容"),metric_));
    body->addWidget(FormKit::fieldRow(QStringLiteral("比较方式"),comparison_));
    body->addWidget(FormKit::fieldRow(QStringLiteral("阈值 / 宽度 / 布尔值（真=1）"),value_));
    body->addWidget(FormKit::fieldRow(QStringLiteral("上限 / 高度 / 颜色容差 / 找图阈值"),upper_));
    body->addWidget(FormKit::fieldRow(QStringLiteral("限定类别（-1 表示全部）"),class_));
    body->addWidget(FormKit::fieldRow(QStringLiteral("父组（0=启动条件，-1=仅供动作判断）"),parent_));
    body->addWidget(FormKit::fieldRow(QStringLiteral("按键 / 变量名 / 文本 / 模板路径"),text_));
    text_->setToolTip(QStringLiteral("组合键：LeftControl+U；手柄：Pad0:A；摇杆：Pad0:LX。模板填图片路径。颜色填 RRGGBB；多点格式 x,y,RRGGBB;x,y,RRGGBB。OCR 填要匹配的文字。"));
    auto* region=new QWidget;auto* grid=new QHBoxLayout(region);grid->setContentsMargins(0,0,0,0);
    for(int i=0;i<4;++i){region_[i]=integer(i<2?-32768:0,32768);grid->addWidget(new QLabel(QStringList{"X","Y",QStringLiteral("宽"),QStringLiteral("高")}[i]));grid->addWidget(region_[i]);}
    body->addWidget(FormKit::fieldRow(QStringLiteral("图像区域（宽高 0 表示全图）"),region));tabs->addTab(panel,QStringLiteral("执行条件"));
    settings_=new QTableWidget(int(macros::settings().size()),2);settings_->setHorizontalHeaderLabels({QStringLiteral("规则设置"),QStringLiteral("值")});
    settings_->verticalHeader()->hide();settings_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);settings_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);
    for(size_t i=0;i<macros::settings().size();++i){const auto& s=macros::settings()[i];auto* label=new QTableWidgetItem(q(s.label));label->setFlags(label->flags()&~Qt::ItemIsEditable);label->setToolTip(q(s.help));
        settings_->setItem(int(i),0,label);auto* val=new QTableWidgetItem(q(s.initial));val->setToolTip(q(s.help));settings_->setItem(int(i),1,val);}
    settings_->setMinimumHeight(280);tabs->addTab(settings_,QStringLiteral("规则锁与调度"));
    auto* explain=new QWidget;auto* el=new QVBoxLayout(explain);summary_=new QLabel;summary_->setWordWrap(true);summary_->setTextFormat(Qt::PlainText);summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);el->addWidget(summary_);
    auto* dry=new QPushButton(QStringLiteral("模拟运行（不发送输入）"));el->addWidget(dry);runtime_=new QPlainTextEdit;runtime_->setReadOnly(true);runtime_->setMaximumBlockCount(200);runtime_->setMinimumHeight(160);el->addWidget(runtime_);tabs->addTab(explain,QStringLiteral("中文说明与调试"));
    connect(dry,&QPushButton::clicked,this,[this]{if(simulate)simulate();});
    connect(event_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{if(!loading_){program_.options["event"]=event_->currentData().toString().toStdString();notify();}});
    connect(settings_,&QTableWidget::itemChanged,this,[this](QTableWidgetItem* item){if(!loading_&&item->column()==1){program_.options[macros::settings()[item->row()].id]=item->text().toUtf8().toStdString();notify();}});
    connect(add,&QPushButton::clicked,this,[this]{if(program_.conditions.size()>=macros::maxConditions)return;program_.conditions.push_back({});rebuild();conditions_->selectRow(int(program_.conditions.size())-1);notify();});
    connect(remove,&QPushButton::clicked,this,[this]{const int r=conditions_->currentRow();if(r<0)return;
        // Keep references stable by refusing deletion of a group in use.
        for(const auto& c:program_.conditions)if(c.parent==r){runtime_->setPlainText(QStringLiteral("请先删除或移出这个组内的子条件。"));return;}
        program_.conditions.erase(program_.conditions.begin()+r);for(auto& c:program_.conditions)if(c.parent>r)--c.parent;
        for(auto& a:program_.actions)if(a.type==macros::ActionType::If||a.type==macros::ActionType::While||a.type==macros::ActionType::WaitCondition||a.type==macros::ActionType::Retry) {
            if(a.a==r+1)a.a=-1;else if(a.a>r+1)--a.a;
        }
        for(auto& a:program_.actions)if(a.type==macros::ActionType::Jump){if(a.b==r+1)a.b=-1;else if(a.b>r+1)--a.b;}
        const int cancel=macros::number(program_,"cancel_condition");if(cancel==r+1)program_.options["cancel_condition"]="-1";else if(cancel>r+1)program_.options["cancel_condition"]=std::to_string(cancel-1);
        rebuild();notify();});
    connect(conditions_,&QTableWidget::itemSelectionChanged,this,[this]{if(!loading_)select();});
    for(auto* c:{metric_,comparison_})connect(c,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{commitCondition();});
    for(auto* c:{value_,upper_})connect(c,&QDoubleSpinBox::editingFinished,this,[this]{commitCondition();});
    for(auto* c:{class_,parent_,region_[0],region_[1],region_[2],region_[3]})connect(c,&QSpinBox::editingFinished,this,[this]{commitCondition();});
    connect(text_,&QLineEdit::editingFinished,this,[this]{commitCondition();});
}
void MacroRuleEditor::setProgram(const macros::Program& p){loading_=true;program_=p;
    event_->setCurrentIndex(event_->findData(q(macros::option(p,"event","key_down"))));
    for(size_t i=0;i<macros::settings().size();++i){const auto& s=macros::settings()[i];settings_->item(int(i),1)->setText(q(macros::option(p,s.id,s.initial)));}
    loading_=false;rebuild();summary_->setText(q(macros::describeRule(program_)));}
void MacroRuleEditor::rebuild(){const int row=conditions_->currentRow();loading_=true;conditions_->setRowCount(int(program_.conditions.size()));
    for(size_t i=0;i<program_.conditions.size();++i){const auto& c=program_.conditions[i];conditions_->setItem(int(i),0,new QTableWidgetItem(QString::number(i+1)));
        conditions_->setItem(int(i),1,new QTableWidgetItem(q(macros::label(macros::metrics(),c.metric))));
        conditions_->setItem(int(i),2,new QTableWidgetItem(QStringLiteral("%1 %2 · %3 · 父组 %4").arg(q(c.comparison)).arg(c.value).arg(q(c.text)).arg(c.parent+1)));}
    if(!program_.conditions.empty())conditions_->selectRow(std::clamp(row,0,int(program_.conditions.size())-1));loading_=false;select();}
void MacroRuleEditor::select(){const int r=conditions_->currentRow();const bool valid=r>=0&&r<int(program_.conditions.size());
    for(QWidget* w:std::initializer_list<QWidget*>{metric_,comparison_,value_,upper_,class_,parent_,text_,region_[0],region_[1],region_[2],region_[3]})w->setEnabled(valid);
    if(!valid)return;loading_=true;const auto& c=program_.conditions[r];metric_->setCurrentIndex(metric_->findData(q(c.metric)));comparison_->setCurrentIndex(comparison_->findData(q(c.comparison)));
    value_->setValue(c.value);upper_->setValue(c.upper);class_->setValue(c.classId);parent_->setValue(c.parent+1);text_->setText(q(c.text));for(int i=0;i<4;++i)region_[i]->setValue(c.region[i]);loading_=false;}
void MacroRuleEditor::commitCondition(){if(loading_)return;const int r=conditions_->currentRow();if(r<0||r>=int(program_.conditions.size()))return;
    auto& c=program_.conditions[r];c.metric=metric_->currentData().toString().toStdString();c.comparison=comparison_->currentData().toString().toStdString();
    c.value=value_->value();c.upper=upper_->value();c.classId=class_->value();c.parent=parent_->value()-1;c.text=text_->text().toUtf8().toStdString();for(int i=0;i<4;++i)c.region[i]=region_[i]->value();rebuild();notify();}
void MacroRuleEditor::notify(){summary_->setText(q(macros::describeRule(program_)));if(changed)changed(program_);}
void MacroRuleEditor::setRuntimeText(const QString& text){if(runtime_->toPlainText()!=text)runtime_->setPlainText(text);}
