#include "runtime/live_tune.h"

#include "Apotheosis.h"
#include "config/config.h"
#include "runtime/config_snapshot.h"
#include "qt_ui/config/config_bridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QTimer>
#include <QString>

#include <cstdio>
#include <string>

namespace live_tune
{
namespace
{
QTimer* g_timer = nullptr;
bool g_enabled_once = false;
qint64 g_applied_write_time = 0;
qint64 g_applied_size = -1;
int g_seq = 0;
qint64 g_rejected_write_time = -1;
qint64 g_rejected_size = -1;

QString configDir()
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QString profiles = exeDir + "/configs";
    if (QDir(profiles).exists())
        return profiles;

    return QDir::current().absolutePath();
}

QString sentinelPath() { return configDir() + "/live_tune.enable"; }
QString livePath()     { return configDir() + "/live_tune.ini"; }
QString ackPath()      { return configDir() + "/live_tune_ack.txt"; }

void writeAck(const QString& status, int seq, const QString& detail)
{
    QFile f(ackPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    const QString line = QString("%1 seq=%2 status=%3 %4\n")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"))
        .arg(seq).arg(status).arg(detail);
    f.write(line.toUtf8());
}

bool applyLiveFile(const QString& path, bool suppress_report)
{
    const auto reject = [&](const QString& why) {
        if (!suppress_report)
            writeAck("rejected", g_seq, why);
        return false;
    };

    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;
        const QByteArray head = f.read(256 * 1024);
        f.close();
        for (const char* key : {"[hotkey.0]"})
        {
            if (!head.contains(key))
            {
                return reject(QString("missing '%1' (truncated or not a config?)").arg(key));
            }
        }
    }

    Config probe;
    if (!probe.loadConfig(path.toStdString()))
        return false;

    if (probe.hotkeys.empty())
    {
        return reject("no hotkey profiles parsed");
    }

    {
        std::lock_guard<std::recursive_mutex> lk(configMutex);

        const std::string saved_path = config.configPath();
        const bool loaded = config.loadConfig(path.toStdString());
        config.setConfigPath(saved_path);
        if (!loaded)
            return false;
    }

    runtime_config::publish();

    ConfigBridge::instance().syncFromRuntime();

    return true;
}

void poll()
{
    if (!QFile::exists(sentinelPath()))
    {
        if (g_enabled_once)
        {
            g_enabled_once = false;
            writeAck("disabled", g_seq, "sentinel removed");
        }
        return;
    }

    const QString live = livePath();
    QFileInfo fi(live);
    if (!fi.exists())
        return;

    const qint64 wt = fi.lastModified().toMSecsSinceEpoch();
    const qint64 sz = fi.size();
    if (wt == g_applied_write_time && sz == g_applied_size)
        return;

    if (!fi.exists() || fi.size() == 0)
        return;

    const bool already_rejected =
        (wt == g_rejected_write_time && sz == g_rejected_size);

    if (applyLiveFile(live, already_rejected))
    {
        g_applied_write_time = wt;
        g_applied_size = sz;
        g_rejected_write_time = -1;
        g_rejected_size = -1;
        ++g_seq;
        g_enabled_once = true;

        std::string detail;
        {
            std::lock_guard<std::recursive_mutex> lk(configMutex);
            const int idx = config.hotkeys.empty() ? -1 : 0;
            if (idx >= 0)
            {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                    "profile_idx=%d profiles=%zu",
                    idx, config.hotkeys.size());
                detail = buf;
            }
        }
        writeAck("applied", g_seq, QString::fromStdString(detail));
    }
    else
    {
        g_rejected_write_time = wt;
        g_rejected_size = sz;
    }
}
}

void start(int poll_ms)
{
    if (g_timer)
        return;
    if (poll_ms <= 0)
        poll_ms = 200;

    g_timer = new QTimer(QCoreApplication::instance());
    QObject::connect(g_timer, &QTimer::timeout, [] { poll(); });
    g_timer->start(poll_ms);

    writeAck("started", 0,
             QString("poll=%1ms dir=%2").arg(poll_ms).arg(configDir()));
}

void stop()
{
    if (g_timer)
    {
        g_timer->stop();
        g_timer->deleteLater();
        g_timer = nullptr;
    }
}

void poll_now()
{
    poll();
}

bool active() { return g_enabled_once; }
}
