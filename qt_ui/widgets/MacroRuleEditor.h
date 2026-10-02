#pragma once
#include <QWidget>
#include <functional>
#include "macro/rule_logic.h"
class QComboBox;class QTableWidget;class QLineEdit;class QDoubleSpinBox;class QSpinBox;class QLabel;class QPlainTextEdit;
class MacroRuleEditor:public QWidget {
public:
    explicit MacroRuleEditor(QWidget* parent=nullptr);
    void setProgram(const macros::Program& p);
    void setRuntimeText(const QString& text);
    std::function<void(const macros::Program&)> changed;
    std::function<void()> simulate;
private:
    void rebuild();void select();void commitCondition();void notify();
    macros::Program program_;bool loading_=false;
    QComboBox *event_{},*metric_{},*comparison_{};
    QTableWidget *conditions_{},*settings_{};
    QDoubleSpinBox *value_{},*upper_{};
    QSpinBox *class_{},*parent_{},*region_[4]{};
    QLineEdit* text_{};
    QLabel* summary_{};
    QPlainTextEdit* runtime_{};
};
