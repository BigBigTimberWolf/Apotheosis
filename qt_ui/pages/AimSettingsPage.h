#pragma once

#include <QWidget>
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
    void buildControllerCard();
    void showSensitivityCalibrateDialog(QDoubleSpinBox* spinK);
    void buildDynamicFovCard();
    void buildTriggerCard();
    void buildScopeCtlCard();
    void applyScopeCtlVisibility();
    void rebuildScopeCopyCombo();
    void buildTrajectoryCard();

    static QLabel* makeHint(const QString& text);
    static QLabel* makeSectionTitle(const QString& text);
    QWidget* makeDoubleRow(const char* obj, const char* label,
                           double lo, double hi, double step, double def);
    QWidget* makeIntRow(const char* obj, const char* label, int lo, int hi,
                        int step, int def, const QString& tip);
    QWidget* makeDoubleRowTip(const char* obj, const char* label, double lo, double hi,
                              double step, double def, const QString& tip);
    QWidget* makePathDoubleRow(const char* obj, const char* label, double lo, double hi,
                               double step, double def, const QString& tip);
    static void attachTip(QWidget* row, const QString& tip);

    void rebuildGroupCombo();
    void rebuildProfileList();
    void restyleProfileItems();
    void reloadProfileToUi();

    int currentRuntimeIndex() const;

    QVBoxLayout* m_rightLayout = nullptr;
    QComboBox*   m_groupCombo = nullptr;
    QListWidget* m_profileList = nullptr;
    QLabel*      m_leftTitle = nullptr;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_emptyHint = nullptr;

    std::vector<QDoubleSpinBox*> m_ctlDoubles;
    std::vector<QSpinBox*>       m_ctlInts;
    std::vector<QSpinBox*>       m_triggerInts;
    std::vector<QSpinBox*>       m_pathInts;
    std::vector<QDoubleSpinBox*> m_pathDoubles;

    // ── 开镜档 (自动开镜生效期间取代「瞄准控制器」整组参数) ──────────────
    // ★ 这两条管道【必须】与 m_ctlDoubles/m_ctlInts 分开: 后两者被
    //   buildControllerCard 的 commit 捕获, 会把值写进 hp.ctl_*; 开镜档写的是
    //   hp.ctl_scope。
    std::vector<QDoubleSpinBox*> m_scopeDoubles;
    std::vector<QSpinBox*>       m_scopeInts;
    QComboBox* m_scopeModeCombo = nullptr;   // 「自动开镜」卡里的跟随/独立开关
    QComboBox* m_scopeCopyCombo = nullptr;   // 一键复制的来源热键
    QLabel*    m_scopeCopyHint  = nullptr;   // 复制结果反馈
    QLabel*    m_scopeOffHint   = nullptr;   // 「跟随热键」时显示的提示
    std::vector<QWidget*> m_scopeParamRows;  // 随开关显隐的行(含分段标题)

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
