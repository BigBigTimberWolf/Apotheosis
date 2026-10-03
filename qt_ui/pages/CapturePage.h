#pragma once

#include <QWidget>
#include <QString>
#include <vector>

#include "capture/capture_card_caps.h"

class QComboBox;
class QLineEdit;
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
    void onSourceChanged(int index);
    void onStreamUrlEdited();
    void refreshNdiSources();
    void onNdiSourceEdited();
    void refreshDxgiOutputs();
    void onDxgiOutputChanged(int index);

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
    void updateSourceUi();

    CardWidget*   m_cardCard{};
    QComboBox*    m_sourceCombo{};
    QLineEdit*    m_streamUrl{};
    QWidget*      m_streamRow{};
    QComboBox*    m_ndiSource{};
    QPushButton*  m_ndiRefresh{};
    QWidget*      m_ndiRow{};
    bool          m_ndiSearching = false;
    QComboBox*    m_dxgiOutput{};
    QPushButton*  m_dxgiRefresh{};
    QWidget*      m_dxgiRow{};
    QLabel*       m_dxgiNote{};
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
