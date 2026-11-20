#pragma once

#include <QJsonObject>
#include <QString>

#include <bsoncxx/array/view.hpp>
#include <bsoncxx/document/view.hpp>

namespace RagoShell
{
    class ConnectionSettings;

    /**
     * @brief Read-only checks for everything an MCP client sends to MongoDB.
     * Every check returns an empty string when the input is acceptable,
     * otherwise a message that is safe to show to the client.
     */
    namespace McpGuards
    {
        /// Query filter / projection / sort: no server-side JavaScript
        /// ($where, $function, $accumulator) and no write stages.
        QString checkDocument(bsoncxx::document::view doc);

        /// Aggregation pipeline: an array of one-operator stage documents,
        /// with no $out / $merge (also inside $facet, $lookup, $unionWith),
        /// no server-side JavaScript and no $lookup / $graphLookup /
        /// $unionWith into system.users or system.keys.
        QString checkPipeline(bsoncxx::array::view pipeline);

        /// MongoDB naming rules for user-supplied names; the credential
        /// collections system.users and system.keys are refused.
        QString checkDatabaseName(const QString &name);
        QString checkCollectionName(const QString &name);

        /// Why this saved connection cannot be served headless (SSH tunnel,
        /// interactive password prompts); empty when it can.
        QString unsupportedReason(const ConnectionSettings &connection);

        /// What an MCP client may learn about a connection: name, uuid,
        /// address and flags. Never user names, passwords, key paths or
        /// passphrases.
        QJsonObject connectionSummary(const ConnectionSettings &connection);
    }
}
