#include "ragoshell/core/mcp/McpServer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/mcp/McpBackend.h"
#include "ragoshell/core/mcp/McpGuards.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/utils/ExplainPlan.h"
#include "ragoshell/core/utils/MongoJsonUtils.h"

namespace RagoShell
{
    using bsoncxx::builder::basic::kvp;

    namespace
    {
        // JSON-RPC 2.0 error codes
        constexpr int kParseError = -32700;
        constexpr int kInvalidRequest = -32600;
        constexpr int kMethodNotFound = -32601;
        constexpr int kInvalidParams = -32602;

        /// Invalid tool input or a refused query; reported to the client as
        /// a tool result with isError so the model can correct itself.
        struct ToolError : std::runtime_error
        {
            explicit ToolError(const QString &message) : std::runtime_error(message.toStdString()) {}
        };

        QJsonObject rpcResult(const QJsonValue &id, const QJsonObject &result)
        {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
        }

        QJsonObject rpcError(const QJsonValue &id, int code, const QString &message)
        {
            return {{"jsonrpc", "2.0"}, {"id", id},
                    {"error", QJsonObject{{"code", code}, {"message", message}}}};
        }

        QJsonObject textResult(const QString &text, bool isError = false)
        {
            QJsonObject result{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}};
            if (isError)
                result["isError"] = true;
            return result;
        }

        QString compact(const QJsonObject &obj)
        {
            return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        }

        QString relaxedJson(bsoncxx::document::view doc)
        {
            return QString::fromStdString(bsoncxx::to_json(doc, bsoncxx::ExtendedJsonMode::k_relaxed));
        }

        // ---- tool argument helpers ------------------------------------------

        QString requiredString(const QJsonObject &args, const char *key)
        {
            const QString value = args.value(key).toString().trimmed();
            if (value.isEmpty())
                throw ToolError(QStringLiteral("'%1' is required.").arg(key));
            return value;
        }

        std::int64_t integerArg(const QJsonObject &args, const char *key, std::int64_t fallback)
        {
            const QJsonValue v = args.value(key);
            if (v.isUndefined() || v.isNull())
                return fallback;
            // Clamped before the cast: callers clamp again to their own range.
            if (v.isDouble() && std::floor(v.toDouble()) == v.toDouble())
                return static_cast<std::int64_t>(std::clamp(v.toDouble(), -1e15, 1e15));
            if (v.isString()) {
                bool ok = false;
                const qlonglong n = v.toString().toLongLong(&ok);
                if (ok)
                    return n;
            }
            throw ToolError(QStringLiteral("'%1' must be an integer.").arg(key));
        }

        /// Object (Extended JSON) or string (Extended JSON or Mongo Shell
        /// syntax such as { _id: ObjectId("...") }); nullopt when absent.
        std::optional<bsoncxx::document::value> documentArg(const QJsonObject &args, const char *key)
        {
            const QJsonValue v = args.value(key);
            if (v.isUndefined() || v.isNull())
                return std::nullopt;
            QString text;
            if (v.isObject())
                text = compact(v.toObject());
            else if (v.isString() && !v.toString().trimmed().isEmpty())
                text = v.toString();
            else if (v.isString())
                return std::nullopt;
            else
                throw ToolError(QStringLiteral("'%1' must be a JSON object.").arg(key));

            bsoncxx::document::value doc = bsoncxx::from_json("{}");
            QString error;
            if (!MongoJsonUtils::parseSingleDocument(text, doc, error))
                throw ToolError(QStringLiteral("Invalid '%1': %2").arg(key, error));
            const QString guard = McpGuards::checkDocument(doc.view());
            if (!guard.isEmpty())
                throw ToolError(guard);
            return doc;
        }

        bsoncxx::array::value pipelineArg(const QJsonObject &args)
        {
            const QJsonValue v = args.value("pipeline");
            QString text;
            if (v.isArray())
                text = QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
            else if (v.isString())
                text = v.toString().trimmed();
            else
                throw ToolError(QStringLiteral("'pipeline' must be an array of stage documents."));

            bsoncxx::builder::basic::array stages;
            const QString inner = QString(text).remove(QRegularExpression(QStringLiteral("\\s")));
            if (inner != QLatin1String("[]")) {
                std::vector<bsoncxx::document::value> docs;
                QString error;
                if (!MongoJsonUtils::parseDocuments(text, docs, error))
                    throw ToolError(QStringLiteral("Invalid 'pipeline': %1").arg(error));
                for (const auto &doc : docs)
                    stages.append(bsoncxx::types::b_document{doc.view()});
            }
            bsoncxx::array::value pipeline = stages.extract();
            const QString guard = McpGuards::checkPipeline(pipeline.view());
            if (!guard.isEmpty())
                throw ToolError(guard);
            return pipeline;
        }

        void requireNames(const QString &database, const QString *collection)
        {
            QString error = McpGuards::checkDatabaseName(database);
            if (error.isEmpty() && collection)
                error = McpGuards::checkCollectionName(*collection);
            if (!error.isEmpty())
                throw ToolError(error);
        }

        /// Drivers may echo connection details in errors; never let a secret through.
        QString redact(QString message, const ConnectionSettings &connection)
        {
            std::vector<std::string> secrets;
            for (CredentialSettings *cred : connection.credentials())
                secrets.push_back(cred->userPassword());
            if (connection.sslSettings())
                secrets.push_back(connection.sslSettings()->pemPassPhrase());
            for (const std::string &secret : secrets) {
                if (secret.size() >= 3)
                    message.replace(QString::fromStdString(secret), QStringLiteral("*****"));
            }
            return message;
        }

        // ---- tool schemas -----------------------------------------------------

        QJsonObject prop(const char *type, const char *description)
        {
            return {{"type", type}, {"description", description}};
        }

        QJsonObject documentProp(const char *description)
        {
            return {{"type", QJsonArray{"object", "string"}}, {"description", description}};
        }

        QJsonObject tool(const char *name, const char *title, const QString &description,
                         const QJsonObject &properties, const QStringList &required)
        {
            QJsonObject props = properties;
            if (QString::fromLatin1(name) != QLatin1String("list_connections")) {
                props["connection"] = prop("string",
                    "Connection name or uuid from list_connections. Optional when only one "
                    "connection is exposed.");
            }
            QJsonObject schema{{"type", "object"}, {"properties", props}};
            if (!required.isEmpty())
                schema["required"] = QJsonArray::fromStringList(required);
            return {{"name", name},
                    {"title", title},
                    {"description", description},
                    {"inputSchema", schema},
                    {"annotations", QJsonObject{{"title", title},
                                                {"readOnlyHint", true},
                                                {"destructiveHint", false},
                                                {"idempotentHint", true},
                                                {"openWorldHint", false}}}};
        }

        const QStringList &toolNames()
        {
            static const QStringList names = {"list_connections", "list_databases", "list_collections",
                                              "collection_schema", "find", "aggregate", "count", "explain"};
            return names;
        }

        // ---- result formatting ------------------------------------------------

        /// meta + "documents": [...] as text; relaxed Extended JSON keeps
        /// ObjectId/Date/Decimal/Int64 values exact, which QJsonDocument would not.
        QString documentsText(QJsonObject meta, const std::vector<bsoncxx::document::value> &docs,
                              std::int64_t limit, int maxResultBytes)
        {
            QStringList parts;
            qint64 bytes = 0;
            bool sizeCapped = false;
            for (std::size_t i = 0; i < docs.size() && static_cast<std::int64_t>(i) < limit; ++i) {
                const QString json = relaxedJson(docs[i].view());
                if (bytes + json.size() > maxResultBytes) {
                    sizeCapped = true;
                    break;
                }
                bytes += json.size();
                parts << json;
            }
            meta["returned"] = static_cast<int>(parts.size());
            meta["hasMore"] = static_cast<std::int64_t>(docs.size()) > limit || sizeCapped;
            if (sizeCapped) {
                meta["note"] = QStringLiteral("Result truncated at %1 bytes; use a projection or a "
                                              "smaller limit.").arg(maxResultBytes);
            }
            QString text = compact(meta);
            text.chop(1);   // closing brace
            text += QStringLiteral(",\"documents\":[") + parts.join(QChar(',')) + QStringLiteral("]}");
            return text;
        }

        QJsonArray countsToJson(const std::vector<std::pair<QString, qint64>> &counts)
        {
            QJsonArray arr;
            for (const auto &entry : counts)
                arr.append(QJsonObject{{"value", entry.first}, {"count", entry.second}});
            return arr;
        }

        QJsonObject typesToJson(const std::vector<std::pair<QString, qint64>> &counts)
        {
            QJsonObject obj;
            for (const auto &entry : counts)
                obj[entry.first] = entry.second;
            return obj;
        }

        QJsonObject schemaToJson(const SchemaResult &schema, bool includeValues)
        {
            QJsonArray fields;
            for (const SchemaField &f : schema.fields) {
                QJsonObject field{{"path", f.path},
                                  {"presence", std::round(f.presence * 1000.0) / 1000.0},
                                  {"types", typesToJson(f.types)}};
                if (!f.itemTypes.empty())
                    field["arrayItemTypes"] = typesToJson(f.itemTypes);
                if (f.onlyRecent)
                    field["onlyInNewestDocuments"] = true;
                if (f.categorical)
                    field["categorical"] = true;
                if (includeValues) {
                    if (!f.topValues.empty())
                        field["topValues"] = countsToJson(f.topValues);
                    if (!f.minValue.isEmpty())
                        field["min"] = f.minValue;
                    if (!f.maxValue.isEmpty())
                        field["max"] = f.maxValue;
                    if (!f.letterCase.isEmpty())
                        field["letterCase"] = f.letterCase;
                }
                fields.append(field);
            }
            QJsonObject result{{"sampledDocuments", schema.sampledDocuments}, {"fields", fields}};
            if (schema.totalDocuments >= 0)
                result["estimatedDocuments"] = schema.totalDocuments;
            if (schema.fullScan)
                result["allDocumentsAnalyzed"] = true;
            if (schema.recentDocuments > 0)
                result["newestDocumentsSampled"] = schema.recentDocuments;
            if (!schema.indexes.isEmpty())
                result["indexes"] = QJsonArray::fromStringList(schema.indexes);
            return result;
        }

        QJsonObject explainToJson(const ExplainPlan &plan)
        {
            QJsonObject obj{{"namespace", plan.ns},
                            {"hasExecutionStats", plan.hasExecutionStats},
                            {"indexesUsed", QJsonArray::fromStringList(plan.indexesUsed)},
                            {"warnings", QJsonArray::fromStringList(plan.warnings)}};
            if (plan.nReturned >= 0) obj["nReturned"] = plan.nReturned;
            if (plan.totalDocsExamined >= 0) obj["totalDocsExamined"] = plan.totalDocsExamined;
            if (plan.totalKeysExamined >= 0) obj["totalKeysExamined"] = plan.totalKeysExamined;
            if (plan.executionTimeMs >= 0) obj["executionTimeMs"] = plan.executionTimeMs;
            QJsonArray stages;
            for (const ExplainStage &s : plan.stages) {
                QJsonObject stage{{"depth", s.depth}, {"stage", s.stage}};
                if (!s.detail.isEmpty()) stage["detail"] = s.detail;
                if (s.nReturned >= 0) stage["nReturned"] = s.nReturned;
                if (s.docsExamined >= 0) stage["docsExamined"] = s.docsExamined;
                if (s.keysExamined >= 0) stage["keysExamined"] = s.keysExamined;
                if (s.executionTimeMs >= 0) stage["executionTimeMs"] = s.executionTimeMs;
                if (s.warning) stage["warning"] = true;
                stages.append(stage);
            }
            obj["stages"] = stages;
            return obj;
        }
    }

    QStringList McpServer::supportedProtocolVersions()
    {
        return {"2025-06-18", "2025-03-26", "2024-11-05"};
    }

    McpServer::McpServer(const McpConfig &config,
                         const std::vector<const ConnectionSettings *> &connections,
                         McpBackend &backend)
        : _backend(backend)
    {
        for (const ConnectionSettings *c : connections) {
            if (c)
                _offered.push_back(c);
        }
        applyConfig(config);
    }

    void McpServer::applyConfig(const McpConfig &config)
    {
        _config = config.normalized();
        // Defense in depth: the host already filtered, but nothing outside
        // the allow-list may ever be served.
        _connections.clear();
        if (!_config.enabled)
            return;
        for (const ConnectionSettings *c : _offered) {
            if (_config.isAllowed(c->uuid()))
                _connections.push_back(c);
        }
    }

    QByteArray McpServer::handleLine(const QByteArray &line)
    {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError)
            return QJsonDocument(rpcError(QJsonValue::Null, kParseError, parseError.errorString()))
                .toJson(QJsonDocument::Compact);

        if (doc.isArray()) {
            QJsonArray responses;
            for (const QJsonValue &item : doc.array()) {
                if (!item.isObject()) {
                    responses.append(rpcError(QJsonValue::Null, kInvalidRequest, "Invalid request"));
                    continue;
                }
                if (auto response = handle(item.toObject()))
                    responses.append(*response);
            }
            return responses.isEmpty() ? QByteArray() : QJsonDocument(responses).toJson(QJsonDocument::Compact);
        }
        if (!doc.isObject())
            return QJsonDocument(rpcError(QJsonValue::Null, kInvalidRequest, "Invalid request"))
                .toJson(QJsonDocument::Compact);

        const auto response = handle(doc.object());
        return response ? QJsonDocument(*response).toJson(QJsonDocument::Compact) : QByteArray();
    }

    std::optional<QJsonObject> McpServer::handle(const QJsonObject &message)
    {
        const bool isRequest = message.contains("id");
        const QJsonValue id = message.value("id");
        const QString method = message.value("method").toString();

        if (method.isEmpty()) {
            // Responses to requests we never send, or garbage.
            if (!isRequest || message.contains("result") || message.contains("error"))
                return std::nullopt;
            return rpcError(id, kInvalidRequest, "Invalid request");
        }
        if (!isRequest)
            return std::nullopt;   // notifications/initialized, notifications/cancelled, ...

        const QJsonObject params = message.value("params").toObject();
        if (method == QLatin1String("initialize"))
            return rpcResult(id, initializeResult(params));
        if (method == QLatin1String("ping"))
            return rpcResult(id, QJsonObject());
        if (method == QLatin1String("tools/list"))
            return rpcResult(id, QJsonObject{{"tools", toolDefinitions()}});
        if (method == QLatin1String("tools/call")) {
            const QString name = params.value("name").toString();
            if (!toolNames().contains(name))
                return rpcError(id, kInvalidParams, QStringLiteral("Unknown tool: %1").arg(name));
            return rpcResult(id, callTool(name, params.value("arguments").toObject()));
        }
        return rpcError(id, kMethodNotFound, QStringLiteral("Method not found: %1").arg(method));
    }

    QJsonObject McpServer::initializeResult(const QJsonObject &params) const
    {
        const QString requested = params.value("protocolVersion").toString();
        const QStringList supported = supportedProtocolVersions();
        return {{"protocolVersion", supported.contains(requested) ? requested : supported.first()},
                {"capabilities", QJsonObject{{"tools", QJsonObject{{"listChanged", false}}}}},
                {"serverInfo", QJsonObject{{"name", QLatin1String(BrandInfo::productId)},
                                           {"title", QStringLiteral("%1 (read-only)")
                                                         .arg(QLatin1String(BrandInfo::productName))},
                                           {"version", PROJECT_VERSION}}},
                {"instructions",
                 QStringLiteral("Read-only MongoDB access through the user's %1 saved connections. "
                                "Start with list_connections, then list_databases / list_collections and "
                                "collection_schema before querying. find and aggregate return at most %2 "
                                "documents and every operation is limited to %3 ms. Writes, $out, $merge "
                                "and server-side JavaScript are rejected.")
                     .arg(QLatin1String(BrandInfo::productName))
                     .arg(_config.maxDocuments)
                     .arg(_config.maxTimeMs)}};
    }

    QJsonArray McpServer::toolDefinitions()
    {
        const QJsonObject database = prop("string", "Database name.");
        const QJsonObject collection = prop("string", "Collection name.");
        const QJsonObject filter = documentProp(
            "Query filter as a JSON object (Extended JSON, e.g. {\"_id\": {\"$oid\": \"...\"}}) or a "
            "Mongo Shell string (e.g. \"{ _id: ObjectId('...') }\"). Defaults to {}.");
        const QJsonObject limit = prop("integer",
            "Maximum documents to return; capped by the server's document limit.");

        QJsonArray tools;
        tools.append(tool("list_connections", "List connections",
            QStringLiteral("Saved %1 connections exposed to this server (name, uuid, address, TLS). "
                           "Never includes credentials.")
                .arg(QLatin1String(BrandInfo::productName)),
            QJsonObject(), {}));
        tools.append(tool("list_databases", "List databases",
            "Databases on the connection with their size on disk.",
            QJsonObject(), {}));
        tools.append(tool("list_collections", "List collections",
            "Collections and views in a database.",
            QJsonObject{{"database", database}}, {"database"}));
        tools.append(tool("collection_schema", "Collection schema",
            "Infers the schema from a random $sample: field paths, BSON types and presence ratio. "
            "Sample values are only included with includeValues=true.",
            QJsonObject{{"database", database},
                        {"collection", collection},
                        {"sampleSize", prop("integer", "Documents to sample (default 100, max 1000).")},
                        {"includeValues", prop("boolean",
                            "Also return the most frequent values and min/max per field (default false).")}},
            {"database", "collection"}));
        tools.append(tool("find", "Find documents",
            "Runs a find query and returns documents as relaxed Extended JSON. hasMore=true means "
            "more documents matched than were returned.",
            QJsonObject{{"database", database},
                        {"collection", collection},
                        {"filter", filter},
                        {"projection", documentProp("Projection document, e.g. {\"name\": 1, \"_id\": 0}.")},
                        {"sort", documentProp("Sort document, e.g. {\"createdAt\": -1}.")},
                        {"skip", prop("integer", "Documents to skip (default 0).")},
                        {"limit", limit}},
            {"database", "collection"}));
        tools.append(tool("aggregate", "Aggregate",
            "Runs a read-only aggregation pipeline. $out, $merge and JavaScript operators are "
            "rejected; the server appends a $limit.",
            QJsonObject{{"database", database},
                        {"collection", collection},
                        {"pipeline", QJsonObject{{"type", QJsonArray{"array", "string"}},
                                                 {"items", QJsonObject{{"type", "object"}}},
                                                 {"description", "Array of stage documents, or the same "
                                                                 "in Mongo Shell syntax as a string."}}},
                        {"limit", limit}},
            {"database", "collection", "pipeline"}));
        tools.append(tool("count", "Count documents",
            "Counts documents matching a filter.",
            QJsonObject{{"database", database}, {"collection", collection}, {"filter", filter}},
            {"database", "collection"}));
        tools.append(tool("explain", "Explain query",
            "Explains a find (filter/sort/projection) or, when pipeline is given, an aggregation, "
            "with executionStats: stages, indexes used, documents and keys examined, warnings such "
            "as COLLSCAN.",
            QJsonObject{{"database", database},
                        {"collection", collection},
                        {"filter", filter},
                        {"projection", documentProp("Projection document.")},
                        {"sort", documentProp("Sort document.")},
                        {"pipeline", QJsonObject{{"type", QJsonArray{"array", "string"}},
                                                 {"items", QJsonObject{{"type", "object"}}},
                                                 {"description", "Aggregation pipeline to explain "
                                                                 "instead of a find."}}}},
            {"database", "collection"}));
        return tools;
    }

    QJsonObject McpServer::callTool(const QString &name, const QJsonObject &args)
    {
        if (name == QLatin1String("list_connections")) {
            QJsonArray list;
            for (const ConnectionSettings *c : _connections) {
                QJsonObject summary = McpGuards::connectionSummary(*c);
                const QString unsupported = McpGuards::unsupportedReason(*c);
                if (!unsupported.isEmpty())
                    summary["unavailable"] = unsupported;
                list.append(summary);
            }
            return textResult(compact(QJsonObject{{"connections", list}}));
        }

        const ConnectionSettings *connection = nullptr;
        try {
            const QString ref = args.value("connection").toString().trimmed();
            if (_connections.empty())
                throw ToolError(_config.enabled
                    ? QStringLiteral("No connections are exposed. Tick \"Let AI tools read this connection\" "
                                     "in the connection's settings in %1 (or run "
                                     "'%2-mcp --allow <connection>'), then restart this MCP server.")
                          .arg(QLatin1String(BrandInfo::productName), QLatin1String(BrandInfo::productId))
                    : QStringLiteral("The MCP server was turned off in %1 (Preferences › AI tools).")
                          .arg(QLatin1String(BrandInfo::productName)));
            if (ref.isEmpty()) {
                if (_connections.size() != 1)
                    throw ToolError(QStringLiteral("Several connections are exposed; pass 'connection' "
                                                   "(name or uuid from list_connections)."));
                connection = _connections.front();
            } else {
                for (const ConnectionSettings *c : _connections) {
                    if (c->uuid() == ref) {
                        connection = c;
                        break;
                    }
                }
                if (!connection) {
                    for (const ConnectionSettings *c : _connections) {
                        if (QString::fromStdString(c->getReadableName()).compare(ref, Qt::CaseInsensitive) != 0)
                            continue;
                        if (connection)
                            throw ToolError(QStringLiteral("Connection name '%1' is ambiguous; use its uuid.")
                                                .arg(ref));
                        connection = c;
                    }
                }
                if (!connection)
                    throw ToolError(QStringLiteral("Unknown connection '%1'. Call list_connections to see "
                                                   "the exposed connections.").arg(ref));
            }
            const QString unsupported = McpGuards::unsupportedReason(*connection);
            if (!unsupported.isEmpty())
                throw ToolError(unsupported);

            const int maxTimeMs = _config.maxTimeMs;
            const std::int64_t maxDocs = _config.maxDocuments;

            if (name == QLatin1String("list_databases")) {
                return textResult(compact(QJsonObject{{"databases", _backend.listDatabases(*connection)}}));
            }

            const QString database = requiredString(args, "database");
            if (name == QLatin1String("list_collections")) {
                requireNames(database, nullptr);
                return textResult(compact(QJsonObject{
                    {"database", database},
                    {"collections", _backend.listCollections(*connection, database)}}));
            }

            const QString collection = requiredString(args, "collection");
            requireNames(database, &collection);
            const QJsonObject meta{{"database", database}, {"collection", collection}};

            if (name == QLatin1String("collection_schema")) {
                const int sampleSize = static_cast<int>(std::clamp<std::int64_t>(
                    integerArg(args, "sampleSize", McpConfig::kDefaultSampleSize), 1,
                    McpConfig::kHardMaxSampleSize));
                const SchemaResult schema =
                    _backend.collectionSchema(*connection, database, collection, sampleSize, maxTimeMs);
                if (!schema.ok)
                    throw std::runtime_error(schema.error.toStdString());
                QJsonObject result = schemaToJson(schema, args.value("includeValues").toBool(false));
                result["database"] = database;
                result["collection"] = collection;
                return textResult(compact(result));
            }

            const std::int64_t limit = std::clamp<std::int64_t>(integerArg(args, "limit", maxDocs), 1, maxDocs);

            if (name == QLatin1String("find")) {
                McpFindQuery query{database, collection,
                                   documentArg(args, "filter").value_or(bsoncxx::from_json("{}"))};
                query.projection = documentArg(args, "projection");
                query.sort = documentArg(args, "sort");
                query.skip = std::max<std::int64_t>(0, integerArg(args, "skip", 0));
                query.limit = limit + 1;   // one extra tells whether more matched
                query.maxTimeMs = maxTimeMs;
                QJsonObject findMeta = meta;
                findMeta["limit"] = static_cast<qint64>(limit);
                return textResult(documentsText(findMeta, _backend.find(*connection, query), limit,
                                                _config.maxResultBytes));
            }

            if (name == QLatin1String("aggregate")) {
                const bsoncxx::array::value pipeline = pipelineArg(args);
                QJsonObject aggMeta = meta;
                aggMeta["limit"] = static_cast<qint64>(limit);
                return textResult(documentsText(
                    aggMeta,
                    _backend.aggregate(*connection, database, collection, pipeline.view(), limit + 1, maxTimeMs),
                    limit, _config.maxResultBytes));
            }

            if (name == QLatin1String("count")) {
                const auto filter = documentArg(args, "filter").value_or(bsoncxx::from_json("{}"));
                QJsonObject result = meta;
                result["count"] = static_cast<qint64>(
                    _backend.count(*connection, database, collection, filter.view(), maxTimeMs));
                return textResult(compact(result));
            }

            if (name == QLatin1String("explain")) {
                bsoncxx::builder::basic::document command;
                if (args.contains("pipeline")) {
                    const bsoncxx::array::value pipeline = pipelineArg(args);
                    bsoncxx::builder::basic::array stages;
                    for (auto &&stage : pipeline.view())
                        stages.append(bsoncxx::types::b_document{stage.get_document().value});
                    stages.append(bsoncxx::builder::basic::make_document(kvp("$limit", limit)));
                    command.append(kvp("aggregate", collection.toStdString()),
                                   kvp("pipeline", stages.extract()),
                                   kvp("cursor", bsoncxx::builder::basic::make_document()));
                } else {
                    const auto filter = documentArg(args, "filter").value_or(bsoncxx::from_json("{}"));
                    command.append(kvp("find", collection.toStdString()),
                                   kvp("filter", bsoncxx::types::b_document{filter.view()}));
                    if (const auto projection = documentArg(args, "projection"))
                        command.append(kvp("projection", bsoncxx::types::b_document{projection->view()}));
                    if (const auto sort = documentArg(args, "sort"))
                        command.append(kvp("sort", bsoncxx::types::b_document{sort->view()}));
                    command.append(kvp("limit", limit));
                }
                command.append(kvp("maxTimeMS", maxTimeMs));
                const bsoncxx::document::value raw =
                    _backend.explain(*connection, database, command.view());
                const ExplainPlan plan = ExplainPlanParser::parse(raw.view());
                if (!plan.valid) {
                    QString text = relaxedJson(raw.view());
                    if (text.size() > _config.maxResultBytes)
                        text = text.left(_config.maxResultBytes) + QStringLiteral("...");
                    return textResult(text);
                }
                QJsonObject result = explainToJson(plan);
                result["database"] = database;
                result["collection"] = collection;
                return textResult(compact(result));
            }
        } catch (const ToolError &ex) {
            return textResult(QString::fromStdString(ex.what()), true);
        } catch (const std::exception &ex) {
            QString message = QString::fromUtf8(ex.what());
            if (connection)
                message = redact(message, *connection);
            return textResult(QStringLiteral("MongoDB error: %1").arg(message), true);
        }
        return textResult(QStringLiteral("Unknown tool: %1").arg(name), true);
    }
}
