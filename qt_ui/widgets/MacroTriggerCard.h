#pragma once

#include "macro/macro_config.h"
#include "widgets/CardWidget.h"

#include <QString>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

// "① 触发器": when the macro starts. A macro is started either by a hotkey (with
// how it is pressed) or by a runtime event such as gaining a target, and then runs
// once, loops while held, toggles, or advances one step per trigger. Everything
// else about scheduling (priority, mutex groups, limits…) stays in the advanced
// section of the page.
//
// The card keeps its own copy of the program; every edit reports the whole updated
// copy through `changed`, and the page merges the fields this card owns.
class MacroTriggerCard : public CardWidget
{
public:
    explicit MacroTriggerCard(QWidget* parent = nullptr);

    void setProgram(const macros::Program& program);
    // Text computed by the page: stop-key clash, other macros sharing the key.
    void setNotice(const QString& text);
    // Live status of the key blocker, polled by the page.
    void setBlockStatus(const QString& text);

    std::function<void(const macros::Program&)> changed;

private:
    void read();
    void refresh();
    void switchKind();
    bool keyKind() const;

    macros::Program program_;
    bool loading_ = false;
    QCheckBox *enabled_{}, *block_{};
    QLineEdit* name_{};
    QComboBox *kind_{}, *trigger_{}, *when_{}, *event_{}, *mode_{};
    QSpinBox *interval_{}, *cooldown_{};
    QWidget *keyRow_{}, *eventRow_{}, *intervalRow_{};
    QLabel *modeHelp_{}, *notice_{}, *blockStatus_{};
};
