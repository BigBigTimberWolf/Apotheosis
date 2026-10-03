#pragma once

#include "macro/macro_config.h"
#include "widgets/CardWidget.h"

#include <QString>

#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QVBoxLayout;

// "② 执行条件": all of these must hold for the macro to start. One row per
// condition (what to check, how to compare, the value). Only conditions the row
// can represent exactly are editable here (a plain test at the top level, no
// region); groups, nested conditions and region/image conditions are listed
// read-only and edited in the advanced section, so nothing is ever rewritten
// behind the user's back.
//
// Also hosts the older "only run while a target exists" limits (class ids and
// box height), which the engine still evaluates next to the condition list.
//
// The card keeps its own copy of the program and reports the whole updated copy
// through `changed`; the page merges the conditions, actions (flow steps refer to
// conditions by number), options and target limits.
class MacroConditionList : public CardWidget
{
public:
    explicit MacroConditionList(QWidget* parent = nullptr);

    void setProgram(const macros::Program& program);

    std::function<void(const macros::Program&)> changed;
    std::function<void()> openAdvanced; // jump to the advanced rule editor

private:
    void rebuild();
    void addCondition(const std::string& metric);
    void removeRow(int index);
    void readLimits();
    void emitChanged();
    QWidget* makeRow(int index);

    macros::Program program_;
    bool loading_ = false;
    QVBoxLayout* rows_{};
    QLabel *empty_{}, *warning_{};
    QPushButton* add_{};
    QCheckBox *target_{}, *height_{};
    QLineEdit* classes_{};
    QSpinBox *minHeight_{}, *maxHeight_{};
    QWidget *classesRow_{}, *heightRow_{}, *rangeRow_{};
};
