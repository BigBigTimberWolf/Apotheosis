#include "widgets/MacroRuleEditor.h"
#include <QApplication>
#include <QComboBox>
#include <QTableWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QDir>
#include <QFontDatabase>
#include <stdexcept>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
int main(int argc,char** argv){
    QApplication app(argc,argv);QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");app.setFont(QFont("Microsoft YaHei",10));
    MacroRuleEditor editor;editor.resize(960,860);
    macros::Program p;p.id="ui";p.name=u8"锁定目标 + X1 触发 U";p.options["event"]="target_found";
    macros::Condition c;c.metric="key";c.text="X1MouseButton";p.conditions={c};
    macros::Action wait;wait.type=macros::ActionType::Delay;wait.a=wait.b=50;
    macros::Action key;key.type=macros::ActionType::KeyPress;key.key="U";key.b=50;p.actions={wait,key};
    editor.setProgram(p);int changes=0,simulations=0;
    editor.changed=[&](const macros::Program& value){p=value;++changes;};editor.simulate=[&]{++simulations;};
    QComboBox* events=nullptr;for(auto* combo:editor.findChildren<QComboBox*>())if(combo->findData("target_found")>=0)events=combo;
    CHECK(events&&events->count()==int(macros::events().size()));events->setCurrentIndex(events->findData("ocr_found"));CHECK(p.options["event"]=="ocr_found"&&changes==1);
    for(auto* button:editor.findChildren<QPushButton*>())if(button->text().contains(QStringLiteral("模拟运行")))button->click();CHECK(simulations==1);
    editor.setProgram(p);CHECK(changes==1);editor.show();app.processEvents();
    CHECK(editor.grab().save(QDir::current().filePath("macro_rules_conditions.png")));
    auto* tabs=editor.findChild<QTabWidget*>();CHECK(tabs);tabs->setCurrentIndex(2);app.processEvents();
    CHECK(editor.grab().save(QDir::current().filePath("macro_rules_explanation.png")));
    return 0;
}
