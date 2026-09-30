#include "config/ConfigManager.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) return 1;
    const QString path = dir.filePath(QStringLiteral("config.ini"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return 2;
    file.close();

    auto& config = ConfigManager::instance();
    if (!config.load(path)) return 3;
    config.setInputMethod(QStringLiteral("KMBOXNET"));
    if (config.inputMethod() != QStringLiteral("KMBOXNET")) return 4;
    if (!config.load(path) || config.inputMethod() != QStringLiteral("KMBOXNET")) return 5;
    config.setInputMethod(QStringLiteral("MAKCUNEW"));
    if (config.inputMethod() != QStringLiteral("MAKCUNEW")) return 6;
    config.setInputMethod(QStringLiteral("FERRUM"));
    if (config.inputMethod() != QStringLiteral("FERRUM")) return 8;
    config.setInputMethod(QStringLiteral("DHZBOX_MINI"));
    if (config.inputMethod() != QStringLiteral("DHZBOX_MINI")) return 9;
    config.setDhzboxIp(QStringLiteral("192.168.1.22"));
    config.setDhzboxPort(9012);
    config.setDhzboxKey(88);
    config.setFerrumPort(QStringLiteral("COM11"));
    config.setFerrumBaudrate(3000000);
    if (!config.load(path)) return 10;
    if (config.inputMethod() != QStringLiteral("DHZBOX_MINI") ||
        config.dhzboxIp() != QStringLiteral("192.168.1.22") ||
        config.dhzboxPort() != 9012 || config.dhzboxKey() != 88 ||
        config.ferrumPort() != QStringLiteral("COM11") ||
        config.ferrumBaudrate() != 3000000) return 11;
    config.setInputMethod(QStringLiteral("unknown"));
    if (config.inputMethod() != QStringLiteral("MAKCU")) return 7;
    return 0;
}
