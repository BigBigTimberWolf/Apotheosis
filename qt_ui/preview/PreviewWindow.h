#pragma once

#include <QMainWindow>
#include <QStringList>
#include <QVector>

class TopNavBar;
class SideNav;
class OverviewPage;
class QStackedWidget;
class QWidget;
class QTimer;

class PreviewWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit PreviewWindow(QWidget* parent = nullptr);

    void selectPrimary(int index);

private slots:
    void onPrimaryChanged(int index);
    void onSecondaryChanged(int index);
    void tickMock();
    void toggleRunning();

private:
    void buildPages();
    QWidget* makePlaceholder(const QString& title, const QString& iconName);

    TopNavBar* m_top{};
    SideNav* m_side{};
    QStackedWidget* m_stack{};
    OverviewPage* m_overview{};
    QTimer* m_timer{};

    struct Range {
        int first = 0;
        QString name;
        QStringList subs;
        QStringList icons;
    };
    QVector<Range> m_ranges;

    bool m_running = true;
    int m_tick = 0;
};
