#pragma once

#include <QWidget>

class QLabel;
class QComboBox;
class QPushButton;
class QSpinBox;
class QSlider;
class QDoubleSpinBox;
class QVBoxLayout;
class ToggleSwitch;

class DebugPage : public QWidget {
    Q_OBJECT

public:
    explicit DebugPage(QWidget* parent = nullptr);

    void setFovReadout(const QString& text);

private slots:
    void onLoadConfig();

private:
    void buildScreenshotCard(QVBoxLayout* layout);
    void buildReplayCard(QVBoxLayout* layout);
    void buildDiagCard(QVBoxLayout* layout);
    void buildDynamicFovCard(QVBoxLayout* layout);

    QComboBox* m_screenshotKey{};
    QSlider* m_screenshotDelaySlider{};
    QSpinBox* m_screenshotDelay{};

    ToggleSwitch* m_enableRecording{};
    QSlider* m_replayDurationSlider{};
    QSpinBox* m_replayDuration{};
    QSlider* m_replaySpeedSlider{};
    QDoubleSpinBox* m_replaySpeed{};
    QPushButton* m_playReplay{};
    QPushButton* m_stopReplay{};
    QLabel* m_replayStatus{};

    ToggleSwitch* m_verboseLog{};
    ToggleSwitch* m_showFps{};
    ToggleSwitch* m_showWindow{};

    QLabel* m_fovReadout{};
};
