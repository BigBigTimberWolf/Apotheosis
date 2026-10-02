#pragma once
#include <QWidget>
#include "macro/macro_config.h"

class MacroRuleEditor;
class QDoubleSpinBox;
class QListWidget;
class QTableWidget;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QSpinBox;
class QLabel;
class QPushButton;

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
    void readSettings();
    void rebuildActions(int row);
    void selectAction();
    void readAction();
    void addProgram(bool duplicate);
    void moveAction(int delta);
    void poll();
    macros::Program* selected();
    macros::Action* selectedAction();
    std::vector<macros::Program> programs_;
    bool loading_=false;
    QListWidget* library_{};
    QTableWidget* actions_{};
    QWidget *detail_{}, *inspector_{}, *keyRow_{}, *buttonRow_{}, *aRow_{}, *bRow_{};
    QCheckBox *master_{}, *enabled_{}, *target_{}, *height_{}, *blockTrigger_{};
    QLineEdit *name_{}, *classes_{};
    QComboBox *stopKey_{}, *trigger_{}, *mode_{}, *addType_{}, *type_{}, *key_{}, *button_{};
    QSpinBox *interval_{}, *minHeight_{}, *maxHeight_{}, *a_{}, *b_{};
    QLabel *status_{}, *modeHelp_{}, *aLabel_{}, *bLabel_{}, *actionHint_{}, *empty_{}, *blockStatus_{};
    QPushButton* run_{};
    MacroRuleEditor* rules_{};
    QSpinBox *c_{},*d_{};QDoubleSpinBox* value_{};QLineEdit* text_{};
    QWidget *cRow_{},*dRow_{},*valueRow_{},*textRow_{};
    QLabel *cLabel_{},*dLabel_{},*valueLabel_{},*textLabel_{};
};
