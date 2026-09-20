#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QSpinBox;
class ToggleSwitch;

class SessionPage : public QWidget {
    Q_OBJECT

public:
    explicit SessionPage(QWidget* parent = nullptr);

private slots:
    void onShowWindowChanged(bool checked);
    void loadConfig();

private:
    QLabel* m_backendStatusLabel{};

    ToggleSwitch* m_showWindow{};

    ToggleSwitch* m_cudaGraph{};
    ToggleSwitch* m_gpuExclusive{};
    QSpinBox* m_gpuReserve{};
    QSpinBox* m_cpuReserve{};
    QSpinBox* m_systemMemoryReserve{};
};
