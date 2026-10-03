#pragma once

#include "macro/macro_config.h"
#include "widgets/CardWidget.h"

#include <functional>

class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;

// "③ 动作时间线": the steps that run, top to bottom. Every step is one row that
// is edited in place (what to do and its settings), with move / duplicate /
// delete on the right and quick-add buttons for the common steps below. The less
// common steps (loops, branches, variables, aiming, …) are in the "更多动作" menu
// and show their own settings from the shared action schema.
//
// The card keeps its own copy of the program and reports the whole updated copy
// through `changed`; the page merges the actions.
class MacroActionTimeline : public CardWidget
{
public:
    explicit MacroActionTimeline(QWidget* parent = nullptr);

    void setProgram(const macros::Program& program);

    std::function<void(const macros::Program&)> changed;

private:
    void rebuild();
    void addAction(int type);
    void moveAction(int index, int delta);
    void duplicateAction(int index);
    void removeAction(int index);
    void emitChanged();
    QWidget* makeRow(int index);

    macros::Program program_;
    bool loading_ = false;
    QVBoxLayout* rows_{};
    QLabel* empty_{};
    QPushButton* more_{};
};
