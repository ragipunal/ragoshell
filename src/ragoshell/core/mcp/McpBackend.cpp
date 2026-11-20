#include "ragoshell/core/mcp/McpBackend.h"

#include <algorithm>
#include <chrono>

#include <QJsonObject>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/options/aggregate.hpp>
#include <mongocxx/options/count.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell/core/mongodb/MongoUriUtils.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/CredentialSettings.h"

namespace RagoShell
{
    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;

    namespace
    {
        QString elementString(const bsoncxx::document::view &doc, const char *key)
        {
            const auto el = doc[key];
            if (el && el.type() == bsoncxx::type::k_string)
                return QString::fromUtf8(el.get_string().value.data(),
                                         static_cast<int>(el.get_string().value.size()));
            return {};
        }
    }

    MongoMcpBackend::MongoMcpBackend(UUIDEncoding uuidEncoding, SupportedTimes timeZone)
        : _uuidEncoding(uuidEncoding), _timeZone(timeZone)
    {
    }

    MongoMcpBackend::~MongoMcpBackend() = default;

    mongocxx::client &MongoMcpBackend::client(const ConnectionSettings &connection)
    {
        auto &slot = _clients[connection.uuid()];
        if (!slot) {
            slot = std::make_unique<mongocxx::client>(mongocxx::uri{buildMongoUri(connection)},
                                                      buildMongoClientOptions(connection));
        }
        return *slot;
    }

    QJsonArray MongoMcpBackend::listDatabases(const ConnectionSettings &connection)
    {
        QJsonArray result;
        try {
            for (auto &&doc : client(connection).list_databases()) {
                QJsonObject db;
                db["name"] = elementString(doc, "name");
                if (const auto size = doc["sizeOnDisk"]) {
                    if (size.type() == bsoncxx::type::k_double)
                        db["sizeOnDisk"] = size.get_double().value;
                    else if (size.type() == bsoncxx::type::k_int64)
                        db["sizeOnDisk"] = static_cast<double>(size.get_int64().value);
                    else if (size.type() == bsoncxx::type::k_int32)
                        db["sizeOnDisk"] = size.get_int32().value;
                }
                if (const auto empty = doc["empty"]; empty && empty.type() == bsoncxx::type::k_bool)
                    db["empty"] = empty.get_bool().value;
                result.append(db);
            }
        } catch (const mongocxx::exception &) {
            // Users without the listDatabases privilege (MongoDB < 4.0.5 has no
            // authorizedDatabases) can still read the databases they were given.
            QStringList known;
            if (!connection.defaultDatabase().empty())
                known << QString::fromStdString(connection.defaultDatabase());
            if (connection.hasEnabledPrimaryCredential()) {
                const QString authDb =
                    QString::fromStdString(connection.primaryCredential()->databaseName());
                if (!authDb.isEmpty() && !known.contains(authDb))
                    known << authDb;
            }
            if (known.isEmpty())
                throw;
            for (const QString &name : known)
                result.append(QJsonObject{{"name", name}});
        }
        return result;
    }

    QJsonArray MongoMcpBackend::listCollections(const ConnectionSettings &connection,
                                                const QString &database)
    {
        std::vector<QJsonObject> collections;
        auto db = client(connection).database(database.toStdString());
        for (auto &&doc : db.list_collections()) {
            QJsonObject coll;
            coll["name"] = elementString(doc, "name");
            const QString type = elementString(doc, "type");
            coll["type"] = type.isEmpty() ? QStringLiteral("collection") : type;
            collections.push_back(coll);
        }
        std::sort(collections.begin(), collections.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a.value("name").toString() < b.value("name").toString();
        });
        QJsonArray result;
        for (const QJsonObject &coll : collections)
            result.append(coll);
        return result;
    }

    SchemaResult MongoMcpBackend::collectionSchema(const ConnectionSettings &connection,
                                                   const QString &database, const QString &collection,
                                                   int sampleSize, int maxTimeMs)
    {
        return SchemaAnalysis::analyzeCollection(connection, database, collection, sampleSize,
                                                 maxTimeMs, _uuidEncoding, _timeZone);
    }

    std::vector<bsoncxx::document::value> MongoMcpBackend::find(const ConnectionSettings &connection,
                                                                const McpFindQuery &query)
    {
        auto coll = client(connection)
                        .database(query.database.toStdString())
                        .collection(query.collection.toStdString());
        mongocxx::options::find opts;
        opts.limit(query.limit);
        if (query.skip > 0)
            opts.skip(query.skip);
        if (query.projection)
            opts.projection(query.projection->view());
        if (query.sort)
            opts.sort(query.sort->view());
        opts.max_time(std::chrono::milliseconds(query.maxTimeMs));

        std::vector<bsoncxx::document::value> docs;
        for (auto &&doc : coll.find(query.filter.view(), opts)) {
            docs.emplace_back(doc);
            if (static_cast<std::int64_t>(docs.size()) >= query.limit)
                break;
        }
        return docs;
    }

    std::vector<bsoncxx::document::value> MongoMcpBackend::aggregate(const ConnectionSettings &connection,
                                                                     const QString &database,
                                                                     const QString &collection,
                                                                     bsoncxx::array::view pipeline,
                                                                     std::int64_t limit, int maxTimeMs)
    {
        auto coll = client(connection)
                        .database(database.toStdString())
                        .collection(collection.toStdString());
        mongocxx::pipeline stages;
        for (auto &&stage : pipeline)
            stages.append_stage(stage.get_document().value);
        stages.limit(static_cast<std::int32_t>(limit));

        mongocxx::options::aggregate opts;
        opts.max_time(std::chrono::milliseconds(maxTimeMs));

        std::vector<bsoncxx::document::value> docs;
        for (auto &&doc : coll.aggregate(stages, opts)) {
            docs.emplace_back(doc);
            if (static_cast<std::int64_t>(docs.size()) >= limit)
                break;
        }
        return docs;
    }

    std::int64_t MongoMcpBackend::count(const ConnectionSettings &connection, const QString &database,
                                        const QString &collection, bsoncxx::document::view filter,
                                        int maxTimeMs)
    {
        auto coll = client(connection)
                        .database(database.toStdString())
                        .collection(collection.toStdString());
        mongocxx::options::count opts;
        opts.max_time(std::chrono::milliseconds(maxTimeMs));
        return coll.count_documents(filter, opts);
    }

    bsoncxx::document::value MongoMcpBackend::explain(const ConnectionSettings &connection,
                                                      const QString &database,
                                                      bsoncxx::document::view command)
    {
        auto db = client(connection).database(database.toStdString());
        return db.run_command(make_document(kvp("explain", command),
                                            kvp("verbosity", "executionStats")));
    }
}
