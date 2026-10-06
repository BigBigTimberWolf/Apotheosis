#pragma once

#include <QWidget>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QVBoxLayout;

class AutoFlashPage : public QWidget {
public:
    explicit AutoFlashPage(QWidget* parent = nullptr);

private:
    void refresh();
    void commit();
    void refreshArea();
    // 按瞄准类别单独设阈值的行：每行 = 类别下拉 + 面积 % + 删除。
    void rebuildRuleRows();
    QWidget* addRuleRow(int classId, double areaPercent);
    void commitRules();

    QCheckBox* enabled_ = nullptr;
    QDoubleSpinBox* area_ = nullptr;
    QComboBox* key_ = nullptr;
    QLabel* currentArea_ = nullptr;
    QLabel* ruleSummary_ = nullptr;
    QVBoxLayout* ruleLayout_ = nullptr;
    std::vector<QWidget*> ruleRows_;
};
