#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;

class AutoFlashPage : public QWidget {
public:
    explicit AutoFlashPage(QWidget* parent = nullptr);

private:
    void refresh();
    void commit();
    void refreshArea();

    QCheckBox* enabled_ = nullptr;
    QDoubleSpinBox* area_ = nullptr;
    QComboBox* key_ = nullptr;
    QLabel* currentArea_ = nullptr;
};
