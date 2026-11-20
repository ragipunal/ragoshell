#pragma once

#include <mongocxx/client.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view_or_value.hpp>

#include "ragoshell/core/Core.h"
#include "ragoshell/core/domain/MongoQueryInfo.h"
#include "ragoshell/core/domain/MongoUser.h"
#include "ragoshell/core/domain/MongoFunction.h"
#include "ragoshell/core/events/MongoEventsInfo.h"

namespace RagoShell
{
    class IMongoTransport;

    class MongoClient
    {
    public:
        explicit MongoClient(mongocxx::client *const connection);
        /// Transport backend (WASM/gateway/demo): commands travel as canonical
        /// EJSON instead of the driver's socket path — docs/TransportSeam.md §2.
        explicit MongoClient(IMongoTransport *transport);

        std::vector<std::string> getCollectionNamesWithDbname(const std::string &dbname) const;
        std::vector<MongoCollectionInfo> listCollectionInfos(const std::string &dbname) const;
        std::vector<std::string> getDatabaseNames() const;
        float getVersion() const;
        std::string dbVersionStr() const;
        std::string getStorageEngineType() const;

        std::vector<MongoUser> getUsers(const std::string &dbName);
        void createUser(const std::string &dbName, const MongoUser &user);
        void dropUser(const std::string &dbName, const std::string &user);

        std::vector<MongoFunction> getFunctions(const std::string &dbName) const;
        std::vector<IndexInfo> getIndexes(const MongoCollectionInfo &collection) const;
        void dropIndexFromCollection(const MongoCollectionInfo &collection, const std::string &indexName) const;
        void addEditIndex(const IndexInfo &oldInfo, const IndexInfo &newInfo) const;

        void renameIndexFromCollection(const MongoCollectionInfo &collection, const std::string &oldIndexName,
                                       const std::string &newIndexName) const;

        void createFunction(const std::string &dbName, const MongoFunction &fun,
                            const std::string &existingFunctionName = std::string());

        void dropFunction(const std::string &dbName, const std::string &name);
        void createDatabase(const std::string &dbName);
        void dropDatabase(const std::string &dbName);

        void createCollection(const std::string &ns, long long size, bool capped, int max,
                              bsoncxx::document::view_or_value extraOptions,
                              bsoncxx::document::value *info = nullptr);
        void renameCollection(const MongoNamespace &ns, const std::string &newCollectionName);
        void duplicateCollection(const MongoNamespace &ns, const std::string &newCollectionName);
        void dropCollection(const MongoNamespace &ns);

        void insertDocument(bsoncxx::document::view_or_value obj, const MongoNamespace &ns);
        void saveDocument(bsoncxx::document::view_or_value obj, const MongoNamespace &ns);
        void removeDocuments(const MongoNamespace &ns, bsoncxx::document::view_or_value query,
                             bool justOne = true);
        std::vector<MongoDocumentPtr> query(const MongoQueryInfo &info);

        MongoCollectionInfo runCollStatsCommand(const std::string &ns);
        std::vector<MongoCollectionInfo> runCollStatsCommand(const std::vector<std::string> &namespaces);

        void done();

    private:
        mongocxx::client *const _client = nullptr;
        IMongoTransport *_transport = nullptr;
        bsoncxx::document::value runCommandChecked(const std::string &db,
                                                   bsoncxx::document::view_or_value cmd) const;
        /// Documents of a cursor-shaped command result (cursor.firstBatch).
        std::vector<bsoncxx::document::value> execCursor(
            const std::string &db, bsoncxx::document::view_or_value cmd) const;
        /// listCollections docs, whichever backend is active.
        std::vector<bsoncxx::document::value> listCollectionDocs(
            const std::string &db, bool nameOnly = false) const;
    };
}
