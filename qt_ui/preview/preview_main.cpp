#include <QApplication>
#include "style/Theme.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>
#include <QThread>

#include "preview/PreviewWindow.h"
#include "widgets/IconFont.h"


static QString loadQss() {
    QFile resource(":/style/theme.qss");
    if (resource.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString::fromUtf8(resource.readAll());

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + "/preview.qss",
        appDir + "/../preview.qss",
        appDir + "/../../preview/preview.qss",
        "preview.qss",
        "../preview.qss",
    };
    for (const auto& path : candidates) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString::fromUtf8(file.readAll());
    }
    return {};
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Apotheosis Preview");

    ApotheosisTheme::apply(app);
    IconFont::load();

    if (auto qss = loadQss(); !qss.isEmpty())
        app.setStyleSheet(qss);

    PreviewWindow window;
    window.show();

    if (app.arguments().contains(QStringLiteral("--shot"))) {
        for (int i = 0; i < 12; ++i) {
            QThread::msleep(80);
            QCoreApplication::processEvents();
        }
        QDir().mkpath(QStringLiteral("/tmp/apo_preview"));
        window.grab().save(QStringLiteral("/tmp/apo_preview/overview.png"));
        window.selectPrimary(2);
        for (int i = 0; i < 5; ++i) {
            QThread::msleep(60);
            QCoreApplication::processEvents();
        }
        window.grab().save(QStringLiteral("/tmp/apo_preview/config.png"));
        return 0;
    }

    return app.exec();
}
