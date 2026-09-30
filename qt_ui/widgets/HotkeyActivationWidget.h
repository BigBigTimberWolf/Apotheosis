#pragma once
#include <QWidget>
#include <vector>

struct HotkeyProfile;
class QComboBox;
class QLabel;
class QPushButton;

class HotkeyActivationWidget : public QWidget
{
    Q_OBJECT
public:
    explicit HotkeyActivationWidget(QWidget* parent = nullptr);
    void load(const std::vector<HotkeyProfile>& profiles, int index);
signals:
    void activateRequested();
    void activationKeyChanged(const QString& key);
private:
    QPushButton* activate_ = nullptr;
    QComboBox* key_ = nullptr;
    QLabel* status_ = nullptr;
};
