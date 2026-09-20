#pragma once

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSlider;
class QSpinBox;
class ToggleSwitch;

class AiModelPage : public QWidget {
    Q_OBJECT

public:
    explicit AiModelPage(QWidget* parent = nullptr);

private slots:
    void reloadFromConfig();

private:
    void browseModel();
    void onSmallTargetToggled(bool enabled);
    void updateModelInfo();
    void updateBackendStatus();

    QComboBox* m_modelCombo{};
    QLineEdit* m_modelPath{};
    QLabel* m_fixedInputLabel{};
    QLabel* m_backendStatusLabel{};

    QSlider* m_confSlider{};
    QDoubleSpinBox* m_confSpin{};
    QSlider* m_nmsSlider{};
    QDoubleSpinBox* m_nmsSpin{};

    ToggleSwitch* m_smallTargetEnabled{};
    QSlider* m_smallTargetConfSlider{};
    QDoubleSpinBox* m_smallTargetConfSpin{};
    QSlider* m_smallTargetAreaSlider{};
    QDoubleSpinBox* m_smallTargetAreaSpin{};
    QWidget* m_smallTargetConfRow{};
    QWidget* m_smallTargetAreaRow{};

};
