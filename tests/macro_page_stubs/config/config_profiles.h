#pragma once
#include <QObject>
class ConfigProfiles : public QObject {
    Q_OBJECT
public:
    static ConfigProfiles& instance();
signals:
    void configApplied();
};
