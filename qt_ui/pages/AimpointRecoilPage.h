#pragma once

#include <QWidget>

class QDoubleSpinBox;
class QComboBox;
class QShowEvent;

class AimpointRecoilPage : public QWidget {
public:
    explicit AimpointRecoilPage(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void refresh();
    void commit();

    QDoubleSpinBox* speed_ = nullptr;
    QDoubleSpinBox* maximum_ = nullptr;
    QComboBox* fireKey_ = nullptr;
    bool loading_ = false;
};
