#pragma once

#include <atomic>
#include <cstdint>
#include <QObject>
#include <QMutex>
#include <unordered_set>

#include <mongocxx/client.hpp>
#include <mongocxx/options/client.hpp>

#include "ragoshell/core/events/MongoEvents.h"

QT_BEGIN_NAMESPACE
class QThread;
class QTimer;
QT_END_NAMESPACE

namespace RagoShell
{
    class MongoClient;
    class ScriptEngine;
    class ConnectionSettings;
    class IMongoTransport;

    class MongoWorker : public QObject
    {
        Q_OBJECT

    public:        
        explicit MongoWorker(ConnectionSettings *connection, const QString &startupScript, int batchSize,
                             double mongoTimeoutSec, int shellTimeoutSec, QObject *parent = nullptr);

        ~MongoWorker();
        uint64_t beginNewOperation();
        bool isOpCancelled(uint64_t opGen) const;
        void interrupt();
        void stopAndDelete();
        void changeTimeout(int newTimeout);
        void updateLocalServerPort(int newPort);

    protected Q_SLOTS:

        void init();

        /**
         * @brief Every minute we are issuing { ping : 1 } command to every used connection
         * in order to avoid dropped connections.
         */
        void keepAlive();

        /**
         * @brief Initiate connection to MongoDB
         */
        bool handle(EstablishConnectionRequest *event);

        /**
        * @brief Try to re-connect to MongoDB replica set in order to refresh connection view.
        *        For more details, see MongoServer::tryRefreshReplicaSetFolder()
        */
        void handle(RefreshReplicaSetFolderRequest *event);

        /**
         * @brief Load list of all database names
         */
        void handle(LoadDatabaseNamesRequest *event);

        /**
         * @brief Load list of all collection names
         */
        void handle(LoadCollectionNamesRequest *event);

        /**
         * @brief Load list of all users
         */
        void handle(LoadUsersRequest *event);

        /**
        * @brief Load indexes in collection
        */
        void handle(LoadCollectionIndexesRequest *event);

        /**
        * @brief Add/edit indexes in collection
        */
        void handle(AddEditIndexRequest *event);

        /**
        * @brief delete index from collection
        */
        void handle(DropCollectionIndexRequest *event);

        /**
         * @brief Load list of all JS functions
         */
        void handle(LoadFunctionsRequest *event);

        /**
         * @brief Inserts document
         */
        void handle(InsertDocumentRequest *event);

        /**
         * @brief Remove documents
         */
        void handle(RemoveDocumentRequest *event);

        /**
         * @brief Load list of all collection names
         */
        void handle(ExecuteQueryRequest *event);

        /**
         * @brief Execute javascript
         */
        void handle(ExecuteScriptRequest *event);
        void retry(ExecuteScriptRequest *event);
        void handle(StopScriptRequest *event);

        void handle(AutocompleteRequest *event);
        void handle(CreateDatabaseRequest *event);
        void handle(DropDatabaseRequest *event);

        void handle(CreateCollectionRequest *event);
        void handle(DropCollectionRequest *event);
        void handle(RenameCollectionRequest *event);
        void handle(DuplicateCollectionRequest *event);

        void handle(CreateUserRequest *event);
        void handle(DropUserRequest *event);

        void handle(CreateFunctionRequest *event);
        void handle(DropFunctionRequest *event);

    protected:
        virtual void timerEvent(QTimerEvent *);

    private:
        // Added after Mongo 4.0 to fix connection failures seen after a first edit/add/remove doc. operation
        void restartReplicaSetConnection();

        /**
         * @brief Send event to this MongoWorker
         */
        void send(Event *event);

        std::vector<std::string> getDatabaseNamesSafe(EstablishConnectionRequest* event = nullptr);
        std::string getAuthBase() const;

        // Returns a pair of mongocxx::client* connection and error string
        std::pair<mongocxx::client*, std::string> getConnection(bool mayReturnNull = false);
        MongoClient *getClient();

        /// Install a non-owning transport (InMemoryDemoTransport on WASM,
        /// WebSocketGatewayTransport for Mod B/C). When set, getClient() wraps
        /// it and no socket is opened — docs/TransportSeam.md §4.
        /// Ownership stays with the caller; when the worker creates one from
        /// ConnectionSettings::transportKind it goes into _ownedTransport.
        void setTransport(IMongoTransport *transport);
        IMongoTransport *ensureTransportConnected(std::string *outError = nullptr);

        /**
        *@brief Builds the mongocxx connection URI from current connection settings.
        */
        std::string buildUri() const;

        /**
        *@brief Builds client options (TLS etc.) from current connection settings.
        */
        mongocxx::options::client buildClientOptions() const;

        /**
        *@brief Update Replica Set related parameters/settings.
        */
        ReplicaSet getReplicaSetInfo() const;

        std::string connectAndGetReplicaSetName() const;

        /**
         * @brief Send reply event to object 'obj'
         */
        void reply(QObject *receiver, Event *event);

        /**
        * @brief Send hear beat messages to database in order to keep connection alive
        */
        void pingDatabase(mongocxx::client *client) const;
        ScriptEngine *ensureScriptEngine();
        void applyPendingPortUpdate();

        QThread *_thread;
        QMutex _firstConnectionMutex;
        mutable QMutex _engineMutex;

        // Destroyed AFTER _scriptEngine (C++ reverse member destruction order) so
        // ShellHost::~ShellHost -> CursorState::closeRemote never touches a freed
        // _ownedTransport (W4 security review C2).
        ConnectionSettings *_connSettings;
        std::unique_ptr<mongocxx::client> _client;
        std::unique_ptr<IMongoTransport> _ownedTransport;
        IMongoTransport *_transport = nullptr;  // non-owning, may be null
        std::unique_ptr<ScriptEngine> _scriptEngine;

        std::atomic<uint64_t> _latestOpGen { 0 };
        std::atomic<uint64_t> _cancelledOpGen { 0 };

        const QString _startupScript;
        const int _batchSize;
        int _timerId;
        double _mongoTimeoutSec;
        int _shellTimeoutSec;
        QAtomicInteger<int> _isQuiting;
        int _inFlightOps = 0;
        bool _scriptRunning = false;
        int _pendingPort = 0;

        // Collection of created databases.
        // Starting from 3.0, MongoDB drops empty databases.
        // It means, we did not find a way to create "empty" database.
        // We save all created databases in this collection and merge with
        // list of real databases returned from MongoDB server.
        std::unordered_set<std::string> _createdDbs;
    };

}
