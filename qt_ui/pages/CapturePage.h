#pragma once

#include <QWidget>
#include <QString>
#include <vector>

#include "capture/capture_card_caps.h"

class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;
class ToggleSwitch;
class CardWidget;

class CapturePage : public QWidget {
    Q_OBJECT

public:
    explicit CapturePage(QWidget* parent = nullptr);

private slots:
    void onLoadConfig();
    void refreshDevices();
    void onDeviceChanged(int index);
    void onFormatChanged(int index);
    void onResolutionChanged(int index);
    void onFpsChanged(int index);

private:
    void applySelectionToConfig();

    void buildCardCard(QVBoxLayout* layout);

    void rebuildFormatCombo();
    void rebuildResolutionCombo();
    void rebuildFpsCombo();

    const MFDeviceInfo* currentDevice() const;
    void updateCapabilitySummary();
    void showError(const QString& text);
    void clearError();

    CardWidget*   m_cardCard{};
    QComboBox*    m_devCombo{};
    QComboBox*    m_fmtCombo{};
    QComboBox*    m_resCombo{};
    QComboBox*    m_fpsCombo{};
    ToggleSwitch* m_gpuDecode{};
    QLabel*       m_capSummary{};
    QLabel*       m_recommend{};
    QLabel*       m_error{};
    QPushButton*  m_refreshBtn{};

    std::vector<MFDeviceInfo> m_devices;

    bool m_restoring = false;
};
