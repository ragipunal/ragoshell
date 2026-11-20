#pragma once

#include <functional>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "ragoshell-app/QmlSingleton.h"
#include "ragoshell/core/mcp/McpClientSetup.h"
#include "ragoshell/core/mcp/McpConfig.h"

namespace RagoShell
{
    class SettingsManager;

    /**
     * @brief QML-facing view of the read-only MCP server settings (mcp.json,
     * docs/McpSettingsUi.md): the server switch and limits in Preferences, the
     * per-connection allow-list in the connection dialog, and ready client
     * configurations. Every write is a locked read-modify-write of the file on
     * disk (McpConfig::modify), so changes made with ragoshell-mcp are kept.
     */
    class McpSettingsBridge : public QObject
    {
        Q_OBJECT
        QML_NAMED_ELEMENT(McpSettings)
        QML_SINGLETON

        /// False on phone, tablet and browser builds (they ship no ragoshell-mcp).
        Q_PROPERTY(bool available READ available CONSTANT FINAL)
        Q_PROPERTY(bool serverFound READ serverFound CONSTANT FINAL)
        Q_PROPERTY(QString serverPath READ serverPath CONSTANT FINAL)
        /// False when the server path holds characters a shell would expand
        /// ($, `, % on Windows): no command line is offered for it then.
        Q_PROPERTY(bool commandLineSafe READ commandLineSafe CONSTANT FINAL)
        Q_PROPERTY(bool enabled READ enabled NOTIFY changed FINAL)
        Q_PROPERTY(int maxDocuments READ maxDocuments NOTIFY changed FINAL)
        Q_PROPERTY(int maxTimeSec READ maxTimeSec NOTIFY changed FINAL)
        Q_PROPERTY(int hardMaxDocuments READ hardMaxDocuments CONSTANT FINAL)
        Q_PROPERTY(int hardMaxTimeSec READ hardMaxTimeSec CONSTANT FINAL)
        /// Saved connections that are on the allow-list.
        Q_PROPERTY(int allowedCount READ allowedCount NOTIFY changed FINAL)
        /// Why mcp.json cannot be read, or why the last write failed; empty when fine.
        Q_PROPERTY(QString error READ error NOTIFY changed FINAL)
        /// False while mcp.json exists but does not parse: nothing may be written.
        Q_PROPERTY(bool writable READ writable NOTIFY changed FINAL)
        Q_PROPERTY(QString configPath READ configPath CONSTANT FINAL)
        /// Client names for the configuration picker (product names, not translated).
        Q_PROPERTY(QStringList clientNames READ clientNames CONSTANT FINAL)

    public:
        explicit McpSettingsBridge(SettingsManager *settings, QObject *parent = nullptr);
        static McpSettingsBridge *create(QQmlEngine *, QJSEngine *)
        {
            return QmlSingletonInstance<McpSettingsBridge>::get();
        }

        bool available() const;
        bool serverFound() const { return _launch.found; }
        QString serverPath() const { return _launch.command; }
        bool commandLineSafe() const;
        bool enabled() const { return _config.enabled; }
        int maxDocuments() const { return _config.maxDocuments; }
        int maxTimeSec() const { return (_config.maxTimeMs + 999) / 1000; }
        int hardMaxDocuments() const { return McpConfig::kHardMaxDocuments; }
        int hardMaxTimeSec() const { return McpConfig::kHardMaxTimeMs / 1000; }
        int allowedCount() const;
        QString error() const { return _loadError.isEmpty() ? _writeError : _loadError; }
        bool writable() const { return _writable; }
        QString configPath() const { return _path; }
        QStringList clientNames() const;

        /// Re-read mcp.json (it may have been changed with ragoshell-mcp).
        Q_INVOKABLE void reload();
        Q_INVOKABLE bool isAllowed(const QString &uuid) const;
        /// Adds or removes one connection; writes only when that changes the file.
        Q_INVOKABLE bool setAllowed(const QString &uuid, bool allowed);
        /// Applies only the keys present ("enabled", "maxDocuments",
        /// "maxTimeSec"): the fields the user changed, so a value changed
        /// meanwhile with ragoshell-mcp is not overwritten by a stale one.
        Q_INVOKABLE bool updateServer(const QVariantMap &changes);
        /// Configuration text for clientNames()[client]; empty for the command
        /// line when !commandLineSafe.
        Q_INVOKABLE QString snippet(int client) const;

    public Q_SLOTS:
        /// Takes connections that no longer exist (deleted here or by sync)
        /// off the allow-list; writes only when there is one.
        void pruneMissing();

    Q_SIGNALS:
        void changed();
        /// A change could not be saved: AI tools may keep their access.
        void writeFailed(const QString &error);

    private:
        bool write(const std::function<void(McpConfig &)> &change);
        QStringList savedUuids() const;

        SettingsManager *_settings;
        QString _path;
        McpConfig _config;
        QString _loadError;
        QString _writeError;
        bool _writable = true;
        McpLaunch _launch;
    };
}
