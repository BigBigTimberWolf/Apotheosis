#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class ConfigProfiles : public QObject {
    Q_OBJECT

public:
    struct Entry {
        QString   name;
        QString   filePath;
        qint64    bytes = 0;
        QDateTime modified;
        bool      active = false;
    };

    static ConfigProfiles& instance();

    void initialize();
    void refresh();

    QString     directory() const;
    QString     activeName() const;
    bool        hasActive() const;

    QString     activeFilePath() const;
    QList<Entry> entries() const;
    QStringList names() const;

    bool switchTo(const QString& name, QString* error = nullptr);

    bool saveCurrent(QString* error = nullptr);

    bool saveAs(const QString& name, bool overwrite, QString* error = nullptr);

    bool renameProfile(const QString& from, const QString& to, QString* error = nullptr);

    bool remove(const QString& name, QString* error = nullptr);

    bool openDirectory() const;

    static QString sanitizeName(const QString& raw);
    bool exists(const QString& name) const;

signals:
    void profilesChanged();
    void configApplied();
    void operationFailed(const QString& message);

private:
    ConfigProfiles();
    ConfigProfiles(const ConfigProfiles&) = delete;
    ConfigProfiles& operator=(const ConfigProfiles&) = delete;

    QString profileFilePath(const QString& name) const;
    QString profileCurveDir(const QString& name) const;
    QString activeMarkerPath() const;

    void     writeActiveMarker(const QString& name) const;
    QString  readActiveMarker() const;

    bool applyProfileFile(const QString& targetPath, bool autoSaveCurrent,
                          QString* error = nullptr);

    bool flushCurrent(QString* error);

    QString m_active;
};
