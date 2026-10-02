#pragma once

#include <QWidget>
#include <QList>
#include <vector>
#include "config/ConfigManager.h"
#include "crosshair/color_lab.h"

class QSlider;
class QCheckBox;
class QSpinBox;
class QDoubleSpinBox;
class QPushButton;
class QComboBox;
class QTimer;
class QVBoxLayout;

class CrosshairPage : public QWidget {
    Q_OBJECT

public:
    explicit CrosshairPage(QWidget* parent = nullptr, bool laserMode = false);
    ~CrosshairPage() override;

private:
    enum class PickRole { Direct, TargetSample, BackgroundSample };
    void loadConfig();
    void rebuildColorList();
    void saveCrosshairColors();

    void addPreset(int presetIdx);
    void addNewColor();
    void removeColorAt(int index);

    void toggleColorPick();
    void startColorPick(PickRole role);
    void pollPickedColor();
    void applyPickedColor(int h, int s, int v);
    void updateLabPreview();
    void applyLabProfile();
    void clearLabSamples();
    void finishPicking();

    QSpinBox* m_rectW{};
    QSpinBox* m_rectH{};
    QSpinBox* m_offsetY{};
    QComboBox* m_algorithm{};

    QSpinBox* m_minPixels{};
    QSpinBox* m_closeRadius{};
    bool m_laserMode = false;
    QSpinBox* m_laserCenterX{};
    QSpinBox* m_laserCenterY{};
    QSpinBox* m_laserTargetCenterX{};
    QSpinBox* m_laserTargetCenterY{};
    QSpinBox* m_laserTargetRectW{};
    QSpinBox* m_laserTargetRectH{};
    QDoubleSpinBox* m_laserElongation{};
    QDoubleSpinBox* m_laserSmooth{};

    QList<ConfigManager::ColorProfile> m_colors;
    QWidget* m_colorListContainer{};
    QVBoxLayout* m_colorListLayout{};
    QComboBox* m_presetCombo{};
    QPushButton* m_addPresetBtn{};
    QPushButton* m_addColorBtn{};

    QPushButton* m_pickColorBtn{};
    QTimer* m_pickTimer{};
    int m_pickToken = 0;
    PickRole m_pickRole = PickRole::Direct;
    QPushButton* m_labTargetBtn{};
    QPushButton* m_labBackgroundBtn{};
    QPushButton* m_labApplyBtn{};
    QCheckBox* m_labPreviewToggle{};
    class QLabel* m_labSummary{};
    std::vector<crosshair::ColorLabSample> m_labTargets;
    std::vector<crosshair::ColorLabSample> m_labBackgrounds;
};
