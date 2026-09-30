#pragma once

#include "config/config.h"

#include <QWidget>
#include <QString>
#include <functional>
#include <utility>
#include <vector>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QSpinBox;

class TriggerTargetEditor final : public QWidget {
public:
    explicit TriggerTargetEditor(QWidget* parent = nullptr);
    void load(const HotkeyProfile& profile, const std::vector<ClassFilterState>& available);
    void save(HotkeyProfile& profile) const { profile.trigger_classes = classes_; }
    void setChanged(std::function<void()> callback) { changed_ = std::move(callback); }

private:
    void rebuildList(int selected);
    void refreshFields();
    void editSelected();
    void notifyChanged();
    QString className(int id) const;

    std::vector<TriggerAimClass> classes_;
    std::vector<std::pair<int, QString>> available_;
    std::function<void()> changed_;
    bool loading_ = false;
    int defaultRange_ = 100;
    QComboBox* addClass_ = nullptr;
    QListWidget* list_ = nullptr;
    QDoubleSpinBox* pointX_ = nullptr;
    QDoubleSpinBox* pointY_ = nullptr;
    QSpinBox* rangeX_ = nullptr;
    QSpinBox* rangeY_ = nullptr;
    QLabel* sizeLabel_ = nullptr;
    QWidget* preview_ = nullptr;
};
