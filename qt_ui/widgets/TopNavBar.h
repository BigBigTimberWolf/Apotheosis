#pragma once

#include <QStringList>
#include <QWidget>

class QButtonGroup;
class QComboBox;
class QHBoxLayout;
class QPushButton;
class QTimer;
class QToolButton;
class StatusPill;

class TopNavBar : public QWidget {
    Q_OBJECT

public:
    explicit TopNavBar(QWidget* parent = nullptr);

    void setPrimaryItems(const QStringList& labels);
    void setCurrentPrimary(int index);
    int currentPrimary() const;
    void setSessionStatus(bool running, const QString& text);
    void showSaveFeedback();

    void setProfiles(const QStringList& names, const QString& active);
    QString currentProfile() const;
    void setProfileControlsEnabled(bool enabled);
    void showProfileFeedback(const QString& text);

signals:
    void primaryChanged(int index);
    void saveClicked();

    void profileSwitchRequested(const QString& name);
    void profileSaveRequested();
    void profileSaveAsRequested();
    void profileRenameRequested();
    void profileDeleteRequested();
    void profileOpenDirRequested();
    void profileRefreshRequested();

private:
    QButtonGroup* m_group{};
    QHBoxLayout* m_navRow{};
    StatusPill* m_status{};
    QComboBox* m_profileCombo{};
    QToolButton* m_profileMenuBtn{};
    QString m_profileTip;
    QPushButton* m_saveButton{};
    QTimer* m_saveFeedbackTimer{};
    QTimer* m_profileFeedbackTimer{};
};
