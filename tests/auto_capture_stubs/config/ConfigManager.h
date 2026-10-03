#pragma once
#include <QObject>
class ConfigManager : public QObject {
    Q_OBJECT
public:
    static ConfigManager& instance(){static ConfigManager c;return c;}
signals:
    void configLoaded();
};
