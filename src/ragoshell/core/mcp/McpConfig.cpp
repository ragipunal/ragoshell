#include "ragoshell/core/mcp/McpConfig.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QThread>

#include "ragoshell/core/settings/SettingsManager.h"

namespace RagoShell
{
    QString McpConfig::defaultPath()
    {
        return configDirectory() + QStringLiteral("mcp.json");
    }

    bool McpConfig::load(const QString &path, McpConfig &out, QString *error)
    {
        out = McpConfig();
        QFile file(path);
        if (!file.exists())
            return true;
        if (!file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
            return false;
        }

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (error)
                *error = QStringLiteral("Invalid %1: %2").arg(path, parseError.errorString());
            return false;
        }

        const QJsonObject obj = doc.object();
        out.enabled = obj.value("enabled").toBool(false);
        for (const QJsonValue &v : obj.value("allowedConnections").toArray()) {
            const QString uuid = v.toString().trimmed();
            if (!uuid.isEmpty() && !out.allowedConnections.contains(uuid))
                out.allowedConnections.append(uuid);
        }
        out.maxDocuments = obj.value("maxDocuments").toInt(kDefaultMaxDocuments);
        out.maxTimeMs = obj.value("maxTimeMS").toInt(kDefaultMaxTimeMs);
        out.maxResultBytes = obj.value("maxResultBytes").toInt(kDefaultMaxResultBytes);
        out = out.normalized();
        return true;
    }

    bool McpConfig::save(const QString &path, QString *error) const
    {
        QJsonObject obj;
        obj["enabled"] = enabled;
        obj["allowedConnections"] = QJsonArray::fromStringList(allowedConnections);
        obj["maxDocuments"] = maxDocuments;
        obj["maxTimeMS"] = maxTimeMs;
        obj["maxResultBytes"] = maxResultBytes;

        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)
            || file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented)) < 0
            || !file.commit()) {
            if (error)
                *error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
            return false;
        }
        return true;
    }

    bool McpConfig::modify(const QString &path, const std::function<void(McpConfig &)> &change,
                           QString *error)
    {
        // The app and ragoshell-mcp --allow/--revoke both change the file: the
        // lock makes each read-modify-write whole, so neither loses the other's change.
        QDir().mkpath(QFileInfo(path).absolutePath());
        QLockFile lock(path + QStringLiteral(".lock"));
        if (!lock.tryLock(5000)) {
            if (error)
                *error = QStringLiteral("%1 is being changed by another program; try again.").arg(path);
            return false;
        }
        McpConfig config;
        if (!load(path, config, error))
            return false;
        change(config);
        const McpConfig result = config.normalized();
        // On Windows the replace can collide with a running server reading the
        // file before a request; a short retry covers that.
        for (int attempt = 0;; ++attempt) {
            if (result.save(path, error))
                return true;
            if (attempt == 4)
                return false;
            QThread::msleep(40);
        }
    }

    void McpConfig::keepOnly(const QStringList &existingUuids)
    {
        allowedConnections.erase(std::remove_if(allowedConnections.begin(), allowedConnections.end(),
                                                [&](const QString &uuid) {
                                                    return !existingUuids.contains(uuid);
                                                }),
                                 allowedConnections.end());
    }

    McpConfig McpConfig::normalized() const
    {
        McpConfig c = *this;
        c.maxDocuments = std::clamp(c.maxDocuments, 1, kHardMaxDocuments);
        c.maxTimeMs = std::clamp(c.maxTimeMs, 100, kHardMaxTimeMs);
        c.maxResultBytes = std::clamp(c.maxResultBytes, 4 * 1024, 16 * 1024 * 1024);
        return c;
    }
}
