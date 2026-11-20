#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <QJsonArray>
#include <QString>

#include <bsoncxx/array/view.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <mongocxx/client-fwd.hpp>

#include "ragoshell/core/Enums.h"
#include "ragoshell/core/utils/SchemaAnalysis.h"

namespace RagoShell
{
    class ConnectionSettings;

    struct McpFindQuery
    {
        QString database;
        QString collection;
        bsoncxx::document::value filter;
        std::optional<bsoncxx::document::value> projection;
        std::optional<bsoncxx::document::value> sort;
        std::int64_t skip = 0;
        std::int64_t limit = 0;   ///< > 0
        int maxTimeMs = 0;
    };

    /**
     * @brief MongoDB access used by McpServer. Every argument has already
     * passed McpGuards; implementations only read. All methods block and
     * throw std::exception on failure.
     */
    class McpBackend
    {
    public:
        virtual ~McpBackend() = default;

        /// [{ name, sizeOnDisk, empty }]
        virtual QJsonArray listDatabases(const ConnectionSettings &connection) = 0;
        /// [{ name, type }], sorted by name
        virtual QJsonArray listCollections(const ConnectionSettings &connection,
                                           const QString &database) = 0;
        virtual SchemaResult collectionSchema(const ConnectionSettings &connection,
                                              const QString &database, const QString &collection,
                                              int sampleSize, int maxTimeMs) = 0;
        virtual std::vector<bsoncxx::document::value> find(const ConnectionSettings &connection,
                                                           const McpFindQuery &query) = 0;
        /// 'pipeline' never contains a $limit added by the caller; the
        /// backend appends { $limit: limit }.
        virtual std::vector<bsoncxx::document::value> aggregate(const ConnectionSettings &connection,
                                                                const QString &database,
                                                                const QString &collection,
                                                                bsoncxx::array::view pipeline,
                                                                std::int64_t limit, int maxTimeMs) = 0;
        virtual std::int64_t count(const ConnectionSettings &connection, const QString &database,
                                   const QString &collection, bsoncxx::document::view filter,
                                   int maxTimeMs) = 0;
        /// Runs { explain: command, verbosity: "executionStats" } on 'database'.
        virtual bsoncxx::document::value explain(const ConnectionSettings &connection,
                                                 const QString &database,
                                                 bsoncxx::document::view command) = 0;
    };

    /**
     * @brief McpBackend over mongocxx, one cached client per connection.
     * The URI and TLS options come from buildMongoUri / buildMongoClientOptions,
     * so TLS profiles (CA, client PEM, allowInvalid*) behave as in the app.
     */
    class MongoMcpBackend : public McpBackend
    {
    public:
        MongoMcpBackend(UUIDEncoding uuidEncoding, SupportedTimes timeZone);
        ~MongoMcpBackend() override;

        QJsonArray listDatabases(const ConnectionSettings &connection) override;
        QJsonArray listCollections(const ConnectionSettings &connection,
                                   const QString &database) override;
        SchemaResult collectionSchema(const ConnectionSettings &connection,
                                      const QString &database, const QString &collection,
                                      int sampleSize, int maxTimeMs) override;
        std::vector<bsoncxx::document::value> find(const ConnectionSettings &connection,
                                                   const McpFindQuery &query) override;
        std::vector<bsoncxx::document::value> aggregate(const ConnectionSettings &connection,
                                                        const QString &database,
                                                        const QString &collection,
                                                        bsoncxx::array::view pipeline,
                                                        std::int64_t limit, int maxTimeMs) override;
        std::int64_t count(const ConnectionSettings &connection, const QString &database,
                           const QString &collection, bsoncxx::document::view filter,
                           int maxTimeMs) override;
        bsoncxx::document::value explain(const ConnectionSettings &connection,
                                         const QString &database,
                                         bsoncxx::document::view command) override;

    private:
        mongocxx::client &client(const ConnectionSettings &connection);

        UUIDEncoding _uuidEncoding;
        SupportedTimes _timeZone;
        std::map<QString, std::unique_ptr<mongocxx::client>> _clients;
    };
}
