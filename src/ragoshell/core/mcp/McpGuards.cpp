#include "ragoshell/core/mcp/McpGuards.h"

#include <bsoncxx/types.hpp>

#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/settings/SslSettings.h"

namespace RagoShell
{
    namespace
    {
        // Deeper nesting is not a real query; also bounds the recursion.
        constexpr int kMaxDepth = 64;

        QString forbiddenOperator(const std::string &key)
        {
            if (key == "$out" || key == "$merge")
                return QStringLiteral("%1 writes data and is not allowed (read-only server).")
                    .arg(QString::fromStdString(key));
            if (key == "$where" || key == "$function" || key == "$accumulator")
                return QStringLiteral("%1 runs server-side JavaScript and is not allowed.")
                    .arg(QString::fromStdString(key));
            return {};
        }

        // Credential stores (SCRAM password hashes, cluster signing keys):
        // never readable, directly or as a join / union target.
        bool isCredentialCollection(const QString &name)
        {
            return name == QLatin1String("system.users") || name == QLatin1String("system.keys");
        }

        QString credentialCollectionError(const QString &name)
        {
            return QStringLiteral("Reading %1 (credentials) is not allowed.").arg(name);
        }

        QString stringValue(const bsoncxx::document::element &el)
        {
            if (el.type() != bsoncxx::type::k_string)
                return {};
            const auto value = el.get_string().value;
            return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
        }

        // A stage target is a collection name or, since MongoDB 7, { db, coll }.
        QString targetCollection(const bsoncxx::document::element &el)
        {
            if (el.type() == bsoncxx::type::k_document) {
                const auto coll = el.get_document().value["coll"];
                return coll ? stringValue(coll) : QString();
            }
            return stringValue(el);
        }

        // $lookup / $graphLookup "from" and $unionWith must not reach credentials.
        QString checkStageTarget(const std::string &key, const bsoncxx::document::element &el)
        {
            QString target;
            if ((key == "$lookup" || key == "$graphLookup") && el.type() == bsoncxx::type::k_document) {
                const auto from = el.get_document().value["from"];
                if (from)
                    target = targetCollection(from);
            } else if (key == "$unionWith") {
                target = targetCollection(el);
            }
            return isCredentialCollection(target) ? credentialCollectionError(target) : QString();
        }

        QString walkArray(bsoncxx::array::view arr, int depth);

        QString walkDocument(bsoncxx::document::view doc, int depth)
        {
            if (depth > kMaxDepth)
                return QStringLiteral("Query is nested too deeply.");
            for (auto &&el : doc) {
                const std::string key(el.key());
                QString error = forbiddenOperator(key);
                if (error.isEmpty())
                    error = checkStageTarget(key, el);
                if (!error.isEmpty())
                    return error;
                if (el.type() == bsoncxx::type::k_document) {
                    const QString nested = walkDocument(el.get_document().value, depth + 1);
                    if (!nested.isEmpty())
                        return nested;
                } else if (el.type() == bsoncxx::type::k_array) {
                    const QString nested = walkArray(el.get_array().value, depth + 1);
                    if (!nested.isEmpty())
                        return nested;
                } else if (el.type() == bsoncxx::type::k_code
                           || el.type() == bsoncxx::type::k_codewscope) {
                    return QStringLiteral("JavaScript code values are not allowed.");
                }
            }
            return {};
        }

        QString walkArray(bsoncxx::array::view arr, int depth)
        {
            if (depth > kMaxDepth)
                return QStringLiteral("Query is nested too deeply.");
            for (auto &&el : arr) {
                if (el.type() == bsoncxx::type::k_document) {
                    const QString nested = walkDocument(el.get_document().value, depth + 1);
                    if (!nested.isEmpty())
                        return nested;
                } else if (el.type() == bsoncxx::type::k_array) {
                    const QString nested = walkArray(el.get_array().value, depth + 1);
                    if (!nested.isEmpty())
                        return nested;
                } else if (el.type() == bsoncxx::type::k_code
                           || el.type() == bsoncxx::type::k_codewscope) {
                    return QStringLiteral("JavaScript code values are not allowed.");
                }
            }
            return {};
        }
    }

    QString McpGuards::checkDocument(bsoncxx::document::view doc)
    {
        return walkDocument(doc, 0);
    }

    QString McpGuards::checkPipeline(bsoncxx::array::view pipeline)
    {
        int index = 0;
        for (auto &&el : pipeline) {
            if (el.type() != bsoncxx::type::k_document)
                return QStringLiteral("Pipeline stage %1 is not a document.").arg(index);
            const bsoncxx::document::view stage = el.get_document().value;
            int keys = 0;
            for (auto &&field : stage) {
                ++keys;
                if (field.key().empty() || field.key()[0] != '$')
                    return QStringLiteral("Pipeline stage %1 must start with a $ operator.").arg(index);
            }
            if (keys != 1)
                return QStringLiteral("Pipeline stage %1 must have exactly one operator.").arg(index);
            const QString error = walkDocument(stage, 0);
            if (!error.isEmpty())
                return error;
            ++index;
        }
        return {};
    }

    QString McpGuards::checkDatabaseName(const QString &name)
    {
        if (name.isEmpty())
            return QStringLiteral("Database name is required.");
        if (name.size() > 64)
            return QStringLiteral("Database name is too long.");
        static const QString forbidden = QStringLiteral("/\\. \"$*<>:|?");
        for (const QChar c : name) {
            if (forbidden.contains(c) || c.unicode() == 0)
                return QStringLiteral("Invalid database name.");
        }
        return {};
    }

    QString McpGuards::checkCollectionName(const QString &name)
    {
        if (name.isEmpty())
            return QStringLiteral("Collection name is required.");
        if (name.contains(QChar('$')) || name.contains(QChar(0)))
            return QStringLiteral("Invalid collection name.");
        if (isCredentialCollection(name))
            return credentialCollectionError(name);
        return {};
    }

    QString McpGuards::unsupportedReason(const ConnectionSettings &connection)
    {
        if (connection.hasTransport()) {
            return QStringLiteral(
                "Gateway and demo transport connections are not supported by the MCP server.");
        }
        if (connection.sshSettings() && connection.sshSettings()->enabled())
            return QStringLiteral("SSH tunnel connections are not supported by the MCP server.");
        const SslSettings *ssl = connection.sslSettings();
        if (ssl && ssl->sslEnabled() && ssl->usePemFile() && ssl->askPassphrase())
            return QStringLiteral("Connections that ask for the PEM passphrase at connect time "
                                  "are not supported by the MCP server.");
        return {};
    }

    QJsonObject McpGuards::connectionSummary(const ConnectionSettings &connection)
    {
        QJsonObject obj;
        obj["uuid"] = connection.uuid();
        obj["name"] = QString::fromStdString(connection.getReadableName());
        obj["address"] = QString::fromStdString(connection.getFullAddress());
        obj["tls"] = connection.sslSettings() && connection.sslSettings()->sslEnabled();
        obj["replicaSet"] = connection.isReplicaSet();
        obj["srv"] = connection.isSrv();
        if (!connection.defaultDatabase().empty())
            obj["defaultDatabase"] = QString::fromStdString(connection.defaultDatabase());
        return obj;
    }
}
