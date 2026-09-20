#include "config/config_profiles.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

#include <atomic>
#include <mutex>
#include <string>

#include "Apotheosis.h"
#include "config.h"
#include "config/ConfigManager.h"
#include "config/config_bridge.h"
#include "runtime/config_snapshot.h"
#include "runtime/inference_session.h"

extern std::atomic<bool> detector_model_changed;

namespace
{

constexpr const char* kProfilesDirName   = "configs";
constexpr const char* kProfileSuffix     = ".ini";
constexpr const char* kCurveSuffix       = ".curves";
constexpr const char* kActiveMarkerName  = "active.txt";
constexpr int         kMaxNameLength     = 48;

QStringList internalNonProfileNames()
{
    return { QStringLiteral("live_tune") };
}

QString g_defaultProfileName()
{
    return QString::fromUtf8(u8"默认");
}

QString curveDirFor(const QString& iniPath)
{
    const QFileInfo info(iniPath);
    return info.dir().filePath(info.completeBaseName() + QString::fromLatin1(kCurveSuffix));
}

}

ConfigProfiles::ConfigProfiles() : QObject(nullptr) {}

ConfigProfiles& ConfigProfiles::instance()
{
    static ConfigProfiles s;
    return s;
}

QString ConfigProfiles::directory() const
{
    return QDir(QDir::currentPath()).filePath(QString::fromLatin1(kProfilesDirName));
}

QString ConfigProfiles::profileFilePath(const QString& name) const
{
    return QDir(directory()).filePath(name + QString::fromLatin1(kProfileSuffix));
}

QString ConfigProfiles::profileCurveDir(const QString& name) const
{
    return QDir(directory()).filePath(name + QString::fromLatin1(kCurveSuffix));
}

QString ConfigProfiles::activeMarkerPath() const
{
    return QDir(directory()).filePath(QString::fromLatin1(kActiveMarkerName));
}

QString ConfigProfiles::sanitizeName(const QString& raw)
{
    QString name = raw.trimmed();
    if (name.isEmpty())
        return {};
    if (name.size() > kMaxNameLength)
        name = name.left(kMaxNameLength).trimmed();
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        return {};

    static const QString kBad = QStringLiteral("\\/:*?\"<>|");
    for (const QChar ch : name) {
        if (kBad.contains(ch) || ch < QChar(0x20) || ch == QChar(0x7F))
            return {};
    }
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        name.chop(1);
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        return {};
    return name;
}

QStringList ConfigProfiles::names() const
{
    QStringList out;
    QDir dir(directory());
    if (!dir.exists())
        return out;

    const QStringList internal = internalNonProfileNames();
    const QStringList filters{QStringLiteral("*") + QString::fromLatin1(kProfileSuffix)};
    const auto files = dir.entryInfoList(filters, QDir::Files, QDir::Name);
    for (const auto& info : files) {
        if (internal.contains(info.completeBaseName()))
            continue;
        out << info.completeBaseName();
    }
    return out;
}

QList<ConfigProfiles::Entry> ConfigProfiles::entries() const
{
    QList<Entry> out;
    QDir dir(directory());
    if (!dir.exists())
        return out;

    const QStringList internal = internalNonProfileNames();
    const QStringList filters{QStringLiteral("*") + QString::fromLatin1(kProfileSuffix)};
    const auto files = dir.entryInfoList(filters, QDir::Files, QDir::Name);
    for (const auto& info : files) {
        if (internal.contains(info.completeBaseName()))
            continue;
        Entry e;
        e.name     = info.completeBaseName();
        e.filePath = info.absoluteFilePath();
        e.bytes    = info.size();
        e.modified = info.lastModified();
        e.active   = (e.name == m_active);
        out << e;
    }
    return out;
}

bool ConfigProfiles::exists(const QString& name) const
{
    const QString clean = sanitizeName(name);
    if (clean.isEmpty())
        return false;
    return QFileInfo::exists(profileFilePath(clean));
}

QString ConfigProfiles::activeName() const
{
    return m_active;
}

QString ConfigProfiles::activeFilePath() const
{
    if (m_active.isEmpty())
        return QDir(QDir::currentPath()).filePath(QStringLiteral("config.ini"));
    return profileFilePath(m_active);
}

bool ConfigProfiles::hasActive() const
{
    return !m_active.isEmpty();
}

QString ConfigProfiles::readActiveMarker() const
{
    QFile file(activeMarkerPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    const QString text = QString::fromUtf8(file.readAll()).trimmed();
    return sanitizeName(text);
}

void ConfigProfiles::writeActiveMarker(const QString& name) const
{
    QDir().mkpath(directory());
    QFile file(activeMarkerPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return;
    file.write(name.toUtf8());
    file.close();
}

void ConfigProfiles::initialize()
{
    QDir().mkpath(directory());

    if (names().isEmpty()) {
        const QString path = profileFilePath(g_defaultProfileName());
        bool ok = false;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            ok = config.saveConfig(path.toStdString());
        }
        if (!ok) {
            const QString msg = QString::fromUtf8(u8"无法创建默认配置方案: %1")
                                    .arg(QDir::toNativeSeparators(path));
            emit operationFailed(msg);
        } else {
            qInfo("[Profiles] Created default profile: %s",
                  qUtf8Printable(QDir::toNativeSeparators(path)));
        }
    }

    QString active = readActiveMarker();
    const QStringList all = names();
    if (active.isEmpty() || !all.contains(active)) {
        active = all.contains(g_defaultProfileName()) ? g_defaultProfileName()
                                                      : all.value(0);
    }

    m_active = active;
    if (!m_active.isEmpty()) {
        writeActiveMarker(m_active);
        QString error;
        if (!applyProfileFile(profileFilePath(m_active),  false, &error)) {
            emit operationFailed(QString::fromUtf8(u8"应用配置方案失败: %1").arg(error));
        }
    }

    emit profilesChanged();
}

void ConfigProfiles::refresh()
{
    const QStringList all = names();
    if (!m_active.isEmpty() && !all.contains(m_active)) {
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            config.retargetConfigPath("config.ini");
        }
        m_active.clear();
        QFile::remove(activeMarkerPath());
    }
    emit profilesChanged();
}

bool ConfigProfiles::flushCurrent(QString* error)
{
    ConfigBridge::instance().flush();

    const QString target = m_active.isEmpty()
                               ? QString::fromLatin1("config.ini")
                               : QDir::toNativeSeparators(profileFilePath(m_active));
    std::lock_guard<std::recursive_mutex> lk(configMutex);
    if (!config.saveConfig()) {
        if (error)
            *error = QString::fromUtf8(u8"当前方案写入失败: %1").arg(target);
        return false;
    }
    return true;
}

bool ConfigProfiles::applyProfileFile(const QString& targetPath, bool autoSaveCurrent,
                                      QString* error)
{
    if (!QFileInfo::exists(targetPath)) {
        if (error)
            *error = QString::fromUtf8(u8"方案文件不存在: %1")
                         .arg(QDir::toNativeSeparators(targetPath));
        return false;
    }

    if (autoSaveCurrent && !flushCurrent(error))
        return false;

    std::string oldModel;
    std::string oldInput;
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        oldModel = config.ai_model;
        oldInput = config.input_method;
        if (!config.loadConfig(targetPath.toStdString())) {
            config.retargetConfigPath("config.ini");
            if (error)
                *error = QString::fromUtf8(u8"方案解析失败: %1")
                             .arg(QDir::toNativeSeparators(targetPath));
            return false;
        }
    }

    runtime_config::publish();

    ConfigBridge::instance().syncFromRuntime();

    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (config.ai_model != oldModel) {
            detector_model_changed = true;
            runtime::preload_model_metadata("models/" + config.ai_model, false);
        }
        if (config.input_method != oldInput)
            input_method_changed = true;
    }

    ConfigManager::instance().notifyRuntimeReloaded();

    qInfo("[Profiles] Applied profile: %s", qUtf8Printable(QDir::toNativeSeparators(targetPath)));
    emit configApplied();
    return true;
}

bool ConfigProfiles::switchTo(const QString& name, QString* error)
{
    const QString clean = sanitizeName(name);
    if (clean.isEmpty()) {
        if (error) *error = QString::fromUtf8(u8"方案名无效。");
        return false;
    }
    if (!QFileInfo::exists(profileFilePath(clean))) {
        if (error) *error = QString::fromUtf8(u8"找不到配置方案「%1」。").arg(clean);
        return false;
    }
    if (clean == m_active) {
        emit profilesChanged();
        return true;
    }

    if (!applyProfileFile(profileFilePath(clean),  true, error))
        return false;

    m_active = clean;
    writeActiveMarker(clean);
    emit profilesChanged();
    return true;
}

bool ConfigProfiles::saveCurrent(QString* error)
{
    if (m_active.isEmpty())
        return saveAs(g_defaultProfileName(),  true, error);

    ConfigBridge::instance().flush();
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (!config.saveConfig()) {
            if (error) *error = QString::fromUtf8(u8"写入配置方案失败: %1")
                                    .arg(QDir::toNativeSeparators(profileFilePath(m_active)));
            return false;
        }
    }
    writeActiveMarker(m_active);
    emit profilesChanged();
    return true;
}

bool ConfigProfiles::saveAs(const QString& name, bool overwrite, QString* error)
{
    const QString clean = sanitizeName(name);
    if (clean.isEmpty()) {
        if (error)
            *error = QString::fromUtf8(u8"方案名不能为空, 且不能包含 \\ / : * ? \" < > | 等字符。");
        return false;
    }
    if (clean == m_active)
        return saveCurrent(error);

    const QString path = profileFilePath(clean);
    if (QFileInfo::exists(path) && !overwrite) {
        if (error) *error = QString::fromUtf8(u8"已存在同名配置方案「%1」。").arg(clean);
        return false;
    }

    ConfigBridge::instance().flush();
    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);
        if (!config.saveConfig(path.toStdString())) {
            if (error) *error = QString::fromUtf8(u8"无法写入配置方案: %1")
                                    .arg(QDir::toNativeSeparators(path));
            return false;
        }
    }

    m_active = clean;
    writeActiveMarker(clean);

    if (!applyProfileFile(path,  false, error))
        return false;

    emit profilesChanged();
    return true;
}

bool ConfigProfiles::renameProfile(const QString& from, const QString& to, QString* error)
{
    const QString source = sanitizeName(from);
    const QString target = sanitizeName(to);
    if (source.isEmpty() || target.isEmpty()) {
        if (error) *error = QString::fromUtf8(u8"方案名无效。");
        return false;
    }
    if (source == target) {
        emit profilesChanged();
        return true;
    }
    if (!QFileInfo::exists(profileFilePath(source))) {
        if (error) *error = QString::fromUtf8(u8"找不到配置方案「%1」。").arg(source);
        return false;
    }
    if (QFileInfo::exists(profileFilePath(target))) {
        if (error) *error = QString::fromUtf8(u8"已存在同名配置方案「%1」。").arg(target);
        return false;
    }

    if (source == m_active)
        flushCurrent(error);

    if (!QFile::rename(profileFilePath(source), profileFilePath(target))) {
        if (error) *error = QString::fromUtf8(u8"重命名失败, 请检查该文件是否被占用。");
        return false;
    }
    const QString oldCurves = profileCurveDir(source);
    if (QFileInfo::exists(oldCurves))
        QDir().rename(oldCurves, profileCurveDir(target));

    if (source == m_active) {
        m_active = target;
        writeActiveMarker(target);
        if (!applyProfileFile(profileFilePath(target),  false, error))
            return false;
    }

    emit profilesChanged();
    return true;
}

bool ConfigProfiles::remove(const QString& name, QString* error)
{
    const QString clean = sanitizeName(name);
    if (clean.isEmpty() || !QFileInfo::exists(profileFilePath(clean))) {
        if (error) *error = QString::fromUtf8(u8"找不到配置方案「%1」。").arg(name);
        return false;
    }

    const QStringList all = names();
    if (all.size() <= 1) {
        if (error)
            *error = QString::fromUtf8(u8"至少要保留一个配置方案 —— 否则当前配置没有落盘目标。");
        return false;
    }

    if (clean == m_active) {
        QString fallback;
        for (const QString& n : all) {
            if (n != clean) { fallback = n; break; }
        }
        if (!applyProfileFile(profileFilePath(fallback),  false, error))
            return false;
        m_active = fallback;
        writeActiveMarker(fallback);
    }

    QFile::remove(profileFilePath(clean));
    QDir(profileCurveDir(clean)).removeRecursively();

    qInfo("[Profiles] Removed profile: %s", qUtf8Printable(clean));
    emit profilesChanged();
    return true;
}

bool ConfigProfiles::openDirectory() const
{
    const QString dir = directory();
    QDir().mkpath(dir);
    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}
