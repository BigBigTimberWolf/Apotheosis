#pragma once
#include <QObject>
class ConfigBridge : public QObject {
    Q_OBJECT
public:
    static ConfigBridge& instance();
    void markDirty();
    int dirtyCount = 0;
};
