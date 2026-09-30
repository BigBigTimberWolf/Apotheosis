#pragma once

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QVector>

#include <chrono>
#include <cstdint>
#include <future>
#include <string>

class QStackedWidget;
class QTimer;
class QLabel;
class QPropertyAnimation;
class QProgressDialog;
class StatusBar;
class TopNavBar;
class SideNav;
class OverviewPage;
class QCloseEvent;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    ~MainWindow() override;

    void selectPage(int primary, int secondary);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onPrimaryChanged(int index);
    void onSecondaryChanged(int index);
    void onHeroToggleInference();
    void onSaveRequested();
    void pollMonitorTelemetry();

    void onProfileSwitchRequested(const QString& name);
    void onProfileSaveRequested();
    void onProfileSaveAsRequested();
    void onProfileRenameRequested();
    void onProfileDeleteRequested();
    void onProfileOpenDirRequested();
    void onProfileRefreshRequested();
    void refreshProfileControls();

private:
    void beginSessionOperation(bool start);
    void pollSessionOperation();
    void pollTrtBuildProgress();
    std::future<std::string> m_sessionOperation;
    bool m_starting = false;
    bool m_closeRequested = false;
    bool m_cleanupRequested = false;
    QProgressDialog* m_trtBuildDialog{};
    uint64_t m_trtBuildGeneration = 0;
    long long m_trtBuildFinishedMs = 0;
    void setupPages();
    QWidget* createPage(const QString& name);
    void switchPage(int index);
    void updateContextHeader(int primary, int secondary);

    TopNavBar* m_topNav{};
    SideNav* m_sideNav{};
    QStackedWidget* m_pageStack{};
    QLabel* m_contextTitle{};
    QLabel* m_contextPath{};
    StatusBar* m_statusBar{};
    QTimer* m_monitorTimer{};
    QPropertyAnimation* m_pageAnimation{};

    struct PageRange {
        int first{};
        int count{};
        QString name;
        QStringList subs;
        QStringList icons;
    };

    QVector<PageRange> m_tabPages;

    class OverviewPage*      m_overviewPage{};
    class TargetPage*        m_targetPage{};
    class AimSettingsPage*   m_hotkeyPage{};

    class StatsPage*         m_statsPage{};
    class LogPage*           m_logPage{};
    class DebugPage*         m_debugPage{};
    class AutoCapturePage*   m_autoCapPage{};

    int m_logCursor = 0;

    bool m_sessionRunning = false;
    std::chrono::steady_clock::time_point m_sessionStart{};
};
