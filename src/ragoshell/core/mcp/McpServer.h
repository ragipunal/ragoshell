#pragma once

#include <optional>
#include <vector>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "ragoshell/core/mcp/McpConfig.h"

namespace RagoShell
{
    class ConnectionSettings;
    class McpBackend;

    /**
     * @brief Read-only Model Context Protocol server (JSON-RPC 2.0 over
     * newline-delimited stdio messages). Transport-free: the host feeds it
     * one line per message and writes back what handleLine() returns.
     *
     * Only connections that are both passed in and allowed by the config are
     * served. Tools never return credentials, and every query is bounded by
     * the config limits (document cap, maxTimeMS, result size).
     */
    class McpServer
    {
    public:
        static QStringList supportedProtocolVersions();

        McpServer(const McpConfig &config, const std::vector<const ConnectionSettings *> &connections,
                  McpBackend &backend);

        /// One JSON-RPC message (or batch) in, the response line out; empty
        /// for notifications.
        QByteArray handleLine(const QByteArray &line);

        /// Response for one request; nullopt for notifications.
        std::optional<QJsonObject> handle(const QJsonObject &message);

        /// The tools/list payload.
        static QJsonArray toolDefinitions();

        const std::vector<const ConnectionSettings *> &connections() const { return _connections; }

        /// A changed mcp.json while serving: limits are replaced and the served
        /// connections become those offered at start that are still allowed.
        /// Disabling or revoking takes effect at the next request; a connection
        /// allowed after start is not added (the client restarts the server).
        void applyConfig(const McpConfig &config);

    private:
        QJsonObject callTool(const QString &name, const QJsonObject &args);
        QJsonObject initializeResult(const QJsonObject &params) const;

        McpConfig _config;
        std::vector<const ConnectionSettings *> _offered;
        std::vector<const ConnectionSettings *> _connections;
        McpBackend &_backend;
    };
}
