#pragma once

#include <QWidget>
#include <array>
#include <functional>
#include <utility>

struct HotkeyProfile;
struct TriggerParams;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGraphicsView;
class QPushButton;
class QSpinBox;
class QStackedWidget;

class TriggerWorkflowEditor final : public QWidget {
public:
    explicit TriggerWorkflowEditor(QWidget* parent = nullptr);
    void load(const HotkeyProfile& profile);
    void save(HotkeyProfile& profile) const;
    void load(const TriggerParams& params);
    void save(TriggerParams& params) const;
    void setChanged(std::function<void()> callback) { changed_ = std::move(callback); }

private:
    void refreshNodes();
    void selectNode(int index);
    void selectBranch(int mode);
    void changed();
    void fitCanvas();
    void zoomCanvas(qreal factor);
    static QSpinBox* spin(int low, int high, int step, int value);

    bool loading_ = false;
    bool hasClassRules_ = false;
    bool detailBranchSelected_ = false;
    std::function<void()> changed_;
    std::array<QPushButton*, 7> nodes_{};
    std::array<std::array<QPushButton*, 6>, 2> branchNodes_{};
    QPushButton* normalBranch_ = nullptr;
    QGraphicsView* canvas_ = nullptr;
    class QLabel* zoomLabel_ = nullptr;
    bool fitMode_ = true;
    qreal zoom_ = 1.0;
    QStackedWidget* inspector_ = nullptr;
    QCheckBox* enabled_ = nullptr;
    QComboBox* mode_ = nullptr;
    QWidget* snapPanel_ = nullptr;
    QWidget* classicDetail_ = nullptr;
    class QLabel* guide_ = nullptr;
    QDoubleSpinBox* snapPixelsPerCount_ = nullptr;
    QSpinBox* snapMaxCounts_ = nullptr;
    QSpinBox* snapCountsPerSecond_ = nullptr;
    QSpinBox* returnCountsPerSecond_ = nullptr;
    QSpinBox* returnYPercent_ = nullptr;
    QWidget* returnSettings_ = nullptr;
    QDoubleSpinBox* snapTolerance_ = nullptr;
    QSpinBox* disappearMs_ = nullptr;
    int legacySnapHoldMs_ = 30;
    int legacyCooldownMs_ = 200;
    QSpinBox* spinCountsPerTurn_ = nullptr;
    QSpinBox* turnStepDegrees_ = nullptr;
    QSpinBox* turnDurationMs_ = nullptr;
    QSpinBox* turnHoldMs_ = nullptr;
    QSpinBox* spinCountsPerSecond_ = nullptr;
    QSpinBox* spinTurns_ = nullptr;
    QWidget* spinSettings_ = nullptr;
    class QLabel* snapHint_ = nullptr;
    QSpinBox* zone_ = nullptr;
    QSpinBox* targetCooldown_ = nullptr;
    QComboBox* stopMode_ = nullptr;
    QSpinBox* stopBefore_ = nullptr;
    QSpinBox* stopAfter_ = nullptr;
    QComboBox* scopeMode_ = nullptr;
    QSpinBox* scopeDelay_ = nullptr;
    QSpinBox* fireDelay_ = nullptr;
    QCheckBox* prearmEnabled_ = nullptr;
    QSpinBox* prearmExpand_ = nullptr;
    QSpinBox* delayJitter_ = nullptr;
    QComboBox* fireMode_ = nullptr;
    QSpinBox* fireDuration_ = nullptr;
    QSpinBox* lossDelay_ = nullptr;
    QSpinBox* durationJitter_ = nullptr;
    QSpinBox* fireInterval_ = nullptr;
    QSpinBox* intervalJitter_ = nullptr;
    QCheckBox* switchEnabled_ = nullptr;
    QSpinBox* switchDelay_ = nullptr;
};
