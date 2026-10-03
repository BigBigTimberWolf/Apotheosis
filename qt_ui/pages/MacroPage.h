#pragma once
#include <QWidget>
#include "macro/macro_config.h"

class MacroActionTimeline;
class MacroConditionList;
class MacroRuleEditor;
class MacroTriggerCard;
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QScrollArea;
class QToolButton;

// The macro editor: a list of macros on the left and, for the selected one, three
// plain steps on the right - when it starts (trigger), what must hold (conditions)
// and what it does (action timeline) - under a one-sentence summary. Scheduling,
// groups and debugging live in a collapsed "advanced" section.
class MacroPage : public QWidget {
    Q_OBJECT
public:
    explicit MacroPage(QWidget* parent=nullptr);
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    void load();
    void save();
    void rebuildLibrary(int row);
    void selectProgram();
    void addProgram(bool duplicate);
    // One of the cards edited the selected macro: store it and refresh the rest.
    void edited(const macros::Program& program,const QWidget* source);
    void syncCards(const QWidget* except=nullptr);
    void refreshSummary();
    void refreshLibraryItem();
    void refreshNotice();
    void showAdvanced(bool visible);
    void poll();
    macros::Program* selected();
    std::vector<macros::Program> programs_;
    bool loading_=false;
    QListWidget* library_{};
    QScrollArea* scroll_{};
    QWidget* detail_{};
    QCheckBox* master_{};
    QComboBox* stopKey_{};
    QLabel *status_{}, *empty_{}, *summary_{};
    QPushButton* run_{};
    MacroTriggerCard* trigger_{};
    MacroConditionList* conditions_{};
    MacroActionTimeline* timeline_{};
    QToolButton* advancedToggle_{};
    MacroRuleEditor* rules_{};
};
