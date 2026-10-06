#pragma once

#include <QWidget>
#include <memory>
#include <vector>

#include "widgets/CurveCanvas.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QVBoxLayout;

class CardWidget;
class TargetPage;
class TriggerWorkflowEditor;
class TriggerTargetEditor;
class HotkeyActivationWidget;
struct HotkeyProfile;

class AimSettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit AimSettingsPage(QWidget* parent = nullptr);

    void setTargetPage(TargetPage* tp);

protected:
    void showEvent(QShowEvent* event) override;

public slots:
    void reloadFromRuntime();

private slots:
    void onGroupChanged(int index);
    void onProfileSelected(int row);
    void onAddProfile();
    void onDeleteProfile();
    void onCopyProfile();
    void onPasteProfile();
    void onRenameProfile();
    void onAddGroup();
    void onDeleteGroup();
    void onTargetClassesChanged();

private:
    void buildLeftPanel(QWidget* parent);
    void buildRightPanel(QWidget* parent);

    void buildKeyBindCard();
    void buildFovCard();
    void buildAimClassCard();
    void rebuildAimClassRows();
    void rebuildAddClassCombo();
    void moveAimClass(int from, int to);
    void buildCrosshairCard();
    void buildRecoveredControllerCard();
    void buildDynamicFovCard();
    void buildTriggerCard();
    void buildTrajectoryCard();

    static QLabel* makeHint(const QString& text);
    static QLabel* makeSectionTitle(const QString& text);
    QWidget* makeIntRow(const char* obj, const char* label, int lo, int hi,
                        int step, int def, const QString& tip);
    QWidget* makePathDoubleRow(const char* obj, const char* label, double lo, double hi,
                               double step, double def, const QString& tip);
    static void attachTip(QWidget* row, const QString& tip);

    void rebuildGroupCombo();
    void rebuildProfileList();
    void restyleProfileItems();
    void refreshActivation();
    void moveProfileInGroup(int fromRow, int toRow);
    void reloadProfileToUi();

    int currentRuntimeIndex() const;

    QVBoxLayout* m_rightLayout = nullptr;
    QComboBox*   m_groupCombo = nullptr;
    QListWidget* m_profileList = nullptr;
    std::shared_ptr<HotkeyProfile> m_copiedProfile;
    QLabel*      m_leftTitle = nullptr;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_emptyHint = nullptr;

    TriggerWorkflowEditor* m_triggerWorkflow = nullptr;
    TriggerTargetEditor* m_triggerTargetEditor = nullptr;
    HotkeyActivationWidget* m_activationWidget = nullptr;
    std::vector<QSpinBox*>       m_pathInts;
    std::vector<QDoubleSpinBox*> m_pathDoubles;


    // 轨迹卡片按模式显隐用的句柄（见 buildTrajectoryCard 的 applyMode）。
    QLabel* m_pathSectionBezier = nullptr;
    QLabel* m_pathSectionWind   = nullptr;
    QLabel* m_pathSectionCustom = nullptr;
    QLabel* m_pathSectionNeural = nullptr;
    QWidget* m_windThresholdRow = nullptr;
    std::vector<QWidget*> m_pathSectionBezierRows;
    std::vector<QWidget*> m_pathSectionWindRows;
    std::vector<QWidget*> m_pathSectionCustomRows;
    std::vector<QWidget*> m_pathSectionNeuralRows;
    CurveCanvas* m_curveCanvas = nullptr;
    CurveCanvas* m_neuralPreviewCanvas = nullptr;
    QLabel* m_neuralQualityLabel = nullptr;

    QWidget*     m_aimClassContainer = nullptr;
    QVBoxLayout* m_aimClassLayout    = nullptr;
    QComboBox*   m_addClassCombo     = nullptr;
    QPushButton* m_addClassBtn       = nullptr;

    TargetPage* m_targetPage = nullptr;
    bool m_loading = false;
};
