#pragma once

#include <QWidget>
#include <cstddef>

class QCheckBox;
class QComboBox;
class QLabel;
class QTimer;

class HeadBodyFusionPage : public QWidget {
public:
    explicit HeadBodyFusionPage(QWidget* parent = nullptr);

private:
    void refresh();
    void commit();
    QCheckBox* enabled_ = nullptr;
    QComboBox* head_ = nullptr;
    QComboBox* body_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer* poll_ = nullptr;
    std::size_t classFingerprint_ = 0;
};
