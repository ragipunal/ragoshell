#include "ragoshell-app/McpSettingsBridge.h"

#include <algorithm>

#include <QCoreApplication>

#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SettingsManager.h"

namespace RagoShell
{
    namespace
    {
        constexpr bool kWindows =
#ifdef Q_OS_WIN
            true;
#else
            false;
#endif

        // Order of clientNames().
        const McpClientSetup::Client kClients[] = {
            McpClientSetup::Client::ClaudeCode, McpClientSetup::Client::ClaudeDesktop,
            McpClientSetup::Client::Cursor, McpClientSetup::Client::Antigravity,
            McpClientSetup::Client::VsCode};
    }

    McpSettingsBridge::McpSettingsBridge(SettingsManager *settings, QObject *parent)
        : QObject(parent), _settings(settings), _path(McpConfig::defaultPath())
    {
        if (available()) {
            // APPIMAGE is set by the AppImage runtime to the image's own path.
            _launch = McpClientSetup::locate(QCoreApplication::applicationDirPath(),
                                             qEnvironmentVariable("APPIMAGE"), kWindows);
        }
        reload();
    }

    bool McpSettingsBridge::available() const
    {
#if defined(SYSTEM_MOBILE) || defined(Q_OS_WASM)
        return false;
#else
        return true;
#endif
    }

    bool McpSettingsBridge::commandLineSafe() const
    {
        return McpClientSetup::commandLineSafe(_launch, kWindows);
    }

    void McpSettingsBridge::reload()
    {
        QString error;
        _writable = McpConfig::load(_path, _config, &error);
        if (!_writable)
            _config = McpConfig();   // shown as off; the file itself is left alone
        _loadError = error;
        emit changed();
    }

    QStringList McpSettingsBridge::savedUuids() const
    {
        QStringList uuids;
        if (_settings) {
            for (const ConnectionSettings *c : _settings->connections())
                uuids << c->uuid();
        }
        return uuids;
    }

    int McpSettingsBridge::allowedCount() const
    {
        const QStringList uuids = savedUuids();
        return static_cast<int>(std::count_if(uuids.begin(), uuids.end(),
                                              [this](const QString &uuid) { return _config.isAllowed(uuid); }));
    }

    QStringList McpSettingsBridge::clientNames() const
    {
        return {QStringLiteral("Claude Code"), QStringLiteral("Claude Desktop"), QStringLiteral("Cursor"),
                QStringLiteral("Antigravity"), QStringLiteral("VS Code (Copilot)")};
    }

    bool McpSettingsBridge::isAllowed(const QString &uuid) const
    {
        return !uuid.isEmpty() && _config.isAllowed(uuid);
    }

    bool McpSettingsBridge::write(const std::function<void(McpConfig &)> &change)
    {
        QString failure;
        const bool ok = available() && McpConfig::modify(_path, change, &failure);
        if (ok)
            _writeError.clear();
        else
            _writeError = failure.isEmpty() ? _path : failure;
        reload();
        if (!ok)
            emit writeFailed(error());
        return ok;
    }

    bool McpSettingsBridge::setAllowed(const QString &uuid, bool allowed)
    {
        if (uuid.isEmpty())
            return false;
        reload();
        // Unchanged: nothing is written (a broken file shows nothing allowed).
        if (_config.isAllowed(uuid) == allowed)
            return true;
        const QStringList existing = savedUuids();
        return write([&](McpConfig &c) {
            c.allowedConnections.removeAll(uuid);
            if (allowed)
                c.allowedConnections.append(uuid);
            c.keepOnly(existing);
        });
    }

    bool McpSettingsBridge::updateServer(const QVariantMap &changes)
    {
        if (changes.isEmpty())
            return true;
        const QStringList existing = savedUuids();
        return write([&](McpConfig &c) {
            if (changes.contains(QStringLiteral("enabled")))
                c.enabled = changes.value(QStringLiteral("enabled")).toBool();
            if (changes.contains(QStringLiteral("maxDocuments")))
                c.maxDocuments = std::clamp(changes.value(QStringLiteral("maxDocuments")).toInt(), 1,
                                            hardMaxDocuments());
            if (changes.contains(QStringLiteral("maxTimeSec")))
                c.maxTimeMs = std::clamp(changes.value(QStringLiteral("maxTimeSec")).toInt(), 1,
                                         hardMaxTimeSec()) * 1000;
            c.keepOnly(existing);
        });
    }

    void McpSettingsBridge::pruneMissing()
    {
        reload();
        if (!available() || !_writable)
            return;
        const QStringList existing = savedUuids();
        const bool stale = std::any_of(_config.allowedConnections.begin(), _config.allowedConnections.end(),
                                       [&](const QString &uuid) { return !existing.contains(uuid); });
        if (stale)
            write([&](McpConfig &c) { c.keepOnly(existing); });
    }

    QString McpSettingsBridge::snippet(int client) const
    {
        if (client < 0 || client >= static_cast<int>(std::size(kClients)))
            return {};
        if (kClients[client] == McpClientSetup::Client::ClaudeCode && !commandLineSafe())
            return {};
        return McpClientSetup::snippet(kClients[client], _launch, kWindows);
    }
}
