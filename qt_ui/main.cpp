#include <QApplication>
#include "style/Theme.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QPalette>
#include <QPixmap>
#include <QStyleFactory>
#include <QThread>

#include <vector>

#include "MainWindow.h"
#include "widgets/IconFont.h"
#include "widgets/LoginDialog.h"


static QString loadStyleSheet() {
    QFile resource(":/style/theme.qss");
    if (resource.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString::fromUtf8(resource.readAll());
    }

    QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates = {
        appDir + "/../style/theme.qss",
        appDir + "/../../qt_ui/style/theme.qss",
        appDir + "/style/theme.qss",
        "./style/theme.qss",
        "../style/theme.qss",
    };
    for (const auto& path : candidates) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString::fromUtf8(file.readAll());
        }
    }

    return {};
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Apotheosis");
    app.setOrganizationName("Apotheosis");

    ApotheosisTheme::apply(app);

    IconFont::load();

    if (auto qss = loadStyleSheet(); !qss.isEmpty()) {
        app.setStyleSheet(qss);
    }

    const bool shotMode = app.arguments().contains("--shot");

    MainWindow window;
    window.resize(960, 640);
    window.show();

    if (shotMode) {
        QDir().mkpath("/tmp/apo_shots");
        const std::vector<std::tuple<int, int, QString>> shots = {
            {0, 0, "00_overview"},
            {1, 0, "01_session"},
            {1, 1, "02_modeltools"},
            {2, 0, "03_capture"},
            {2, 3, "04_aimodel"},
            {3, 0, "05_hotkey"},
            {4, 0, "06_stats"},
            {4, 1, "07_log"},
        };
        for (const auto& [a, b, name] : shots) {
            window.selectPage(a, b);
            for (int i = 0; i < 8; ++i)
                QCoreApplication::processEvents();
            QThread::msleep(150);
            QCoreApplication::processEvents();
            window.grab().save("/tmp/apo_shots/" + name + ".png");
        }
        return 0;
    }

    return app.exec();
}
