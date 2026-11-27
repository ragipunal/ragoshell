#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <chrono>
#include <vector>

#include <QJSEngine>
#include <QJSValue>
#include <QObject>
#include <QString>

#include <mongocxx/client.hpp>
#include <mongocxx/cursor.hpp>

#include "ragoshell/core/Core.h"
#include "ragoshell/core/engine/BsonJsConverter.h"
#include "ragoshell/core/engine/ShellBackend.h"
#include "ragoshell/core/transport/IMongoTransport.h"

namespace RagoShell
{
    /// What JavaScript running in a QJSEngine can do with MongoDB (docs/Shell.md).
    ///
    /// The object is exposed to the engine as the hidden global `__host` (install()). The calls are
    /// synchronous: they return when the server has answered. Documents cross the boundary as BSON
    /// (BsonJsConverter), never as text. A failure becomes a JavaScript exception: an Error with the
    /// server's `code` and `codeName` when the server sent them.
    ///
    /// Reads (find, aggregate, countDocuments, distinct, listCollections, listIndexes) use the
    /// driver's own calls and so follow the read preference of the URI. Everything else, writes
    /// included, goes through runCommand: the shell runtime builds the insert / update /
    /// delete / findAndModify / createIndexes commands itself and shapes the replies.
    ///
    /// Use it on the thread of the engine.
    class ShellHost : public QObject
    {
        Q_OBJECT

    public:
        explicit ShellHost(QJSEngine *engine, QObject *parent = nullptr);
        ~ShellHost() override;

        /// Defines the global `__host` in the engine.
        void install();

        BsonJsConverter &converter() { return _bson; }

        /// A driver or conversion error as the shell reports it: name ("MongoServerError" for a
        /// server error), message without the driver's category suffix, and the server's code.
        struct ErrorInfo
        {
            QString name = QStringLiteral("Error");
            QString message;
            bool hasCode = false;
            double code = 0;
            QString codeName;
            QString text() const { return name + QStringLiteral(": ") + message; }
        };
        static ErrorInfo describeError(const std::exception &error);

        /// Connects (a ping, so that a wrong host or password fails here) with the URI and TLS
        /// files of `options`. Returns false and sets `error` when it fails.
        bool connect(const ShellConnectOptions &options, QString *error);
        /// The pieces of connect(), for a caller that opens the client on a thread of its own (so that it can
        /// stop waiting for it): openClient() touches no ShellHost and may run anywhere; adopt() then makes
        /// the client the host's. `appName` goes to the server as client metadata (see newShellAppName()).
        static bool openClient(const ShellConnectOptions &options, const std::string &appName,
                               std::unique_ptr<mongocxx::client> *opened, QString *error);
        static std::string newShellAppName();
        void adopt(std::unique_ptr<mongocxx::client> client, const std::string &appName,
                   const ShellConnectOptions &options);
        void adoptTransport(IMongoTransport *transport, const ShellConnectOptions &options);

        /// What it takes to find and kill the operations this host has running on the server.
        struct KillTarget
        {
            ShellConnectOptions options;
            std::string appName;
        };
        KillTarget killTarget() const;
        /// Opens a second connection and kills (killOp) every operation of the shell's connection, looking
        /// again every 150 ms while `stillRunning` is true (an operation may not have reached the server
        /// yet). Thread safe; touches no ShellHost. Best effort: errors are ignored.
        static void killOperations(const KillTarget &target, const std::atomic<bool> &stillRunning);
        /// Time limit of the reads the host starts (milliseconds; 0: none). maxTimeMS of the call overrides it.
        void setOperationTimeoutMs(int timeoutMs) { _operationTimeoutMs = timeoutMs; }
        void disconnect();
        bool isConnected() const { return _client != nullptr || _transport != nullptr; }
        /// A ping from C++ (no JavaScript exception on failure).
        bool ping(QString *error = nullptr);

        /// For the C++ side (the grid): up to `maxCount` documents of a cursor (0: all that are left)
        /// without a trip through JavaScript. `exhausted` tells that the cursor has no more.
        std::vector<MongoDocumentPtr> takeDocuments(int cursorId, int maxCount, bool *exhausted = nullptr);

        // ---- the JavaScript API --------------------------------------------------------------

        /// Runs a command; throws the server's error. Returns the reply document.
        Q_INVOKABLE QJSValue runCommand(const QString &db, const QJSValue &command);

        /// Opens a cursor; returns its id. `options`: projection, sort, limit, skip, batchSize, hint,
        /// collation, maxTimeMS, comment, allowDiskUse, noCursorTimeout, min, max, returnKey,
        /// showRecordId, let.
        Q_INVOKABLE int find(const QString &db, const QString &collection, const QJSValue &filter,
                             const QJSValue &options = QJSValue());
        /// `options`: allowDiskUse, batchSize, hint, collation, maxTimeMS, comment, let.
        Q_INVOKABLE int aggregate(const QString &db, const QString &collection, const QJSValue &pipeline,
                                  const QJSValue &options = QJSValue());
        /// Up to `count` documents as a JavaScript array (count <= 0: all that are left).
        Q_INVOKABLE QJSValue cursorNext(int cursorId, int count = 1);
        Q_INVOKABLE bool cursorHasNext(int cursorId);
        Q_INVOKABLE void cursorClose(int cursorId);
        Q_INVOKABLE void closeAllCursors();

        /// `options`: hint, limit, skip, maxTimeMS, collation.
        Q_INVOKABLE double countDocuments(const QString &db, const QString &collection, const QJSValue &filter,
                                          const QJSValue &options = QJSValue());
        Q_INVOKABLE double estimatedDocumentCount(const QString &db, const QString &collection);
        /// The distinct values as a JavaScript array.
        Q_INVOKABLE QJSValue distinct(const QString &db, const QString &collection, const QString &field,
                                      const QJSValue &filter = QJSValue(), const QJSValue &options = QJSValue());
        /// The documents of listCollections / listIndexes as a JavaScript array.
        Q_INVOKABLE QJSValue listCollections(const QString &db, const QJSValue &filter = QJSValue());
        Q_INVOKABLE QJSValue listIndexes(const QString &db, const QString &collection);

        /// A new ObjectId as 24 hex characters.
        Q_INVOKABLE QString newObjectId() const;
        /// Extended JSON text of any value (a document, an array, a number, ...), on one line.
        Q_INVOKABLE QString toEjson(const QJSValue &value, bool relaxed = true);
        /// The server's version ("7.0.14"), or an empty string for a user who may not run buildInfo.
        Q_INVOKABLE QString serverVersion();
        /// "host:port" of the server the shell talks to: the primary of a replica set, else the seed list of the URI.
        Q_INVOKABLE QString serverAddress();
        /// `bytes` random bytes as hex text (for UUID()).
        Q_INVOKABLE QString randomHex(int bytes) const;
        Q_INVOKABLE QString hexToBase64(const QString &hex) const;

    private:
        struct CursorState
        {
            std::optional<mongocxx::cursor> cursor;
            std::optional<mongocxx::cursor::iterator> iterator;
            bool consumed = false; ///< the current element was handed out; step before looking again

            bool isTransport = false;
            IMongoTransport *transport = nullptr;
            QString db;
            QString collection;
            std::vector<bsoncxx::document::value> batch;
            std::size_t batchIndex = 0;
            std::int64_t remoteCursorId = 0;
            int batchSize = 0;
            std::int32_t maxTimeMs = 0;

            CursorState() = default;
            explicit CursorState(mongocxx::cursor &&c) : cursor(std::move(c)) {}
            ~CursorState();
            bool hasNext();
            bsoncxx::document::value next();
            void closeRemote() noexcept;
        };

        CursorState *cursorState(int cursorId);
        int addCursor(mongocxx::cursor &&cursor);
        int openTransportCursor(const QString &db, const QString &collection,
                                bsoncxx::document::view reply, int batchSize,
                                std::int32_t maxTimeMs);
        bsoncxx::document::value runTransportCommand(const QString &db,
                                                     bsoncxx::document::view cmd);
        mongocxx::client &client();
        void throwError(const std::exception &error);

        QJSEngine *_engine;
        BsonJsConverter _bson;
        std::unique_ptr<mongocxx::client> _client;
        IMongoTransport *_transport = nullptr;
        std::map<int, std::unique_ptr<CursorState>> _cursors;
        int _nextCursorId = 1;
        std::chrono::milliseconds defaultMaxTime() const
        {
            // A little over the shell's own limit, so that the watchdog (which reports "Timeout reached") is
            // the first to fire; the server-side limit is the safety net behind it.
            return std::chrono::milliseconds(_operationTimeoutMs > 0 ? _operationTimeoutMs + 500 : 0);
        }

        std::string _appName;
        ShellConnectOptions _connectOptions;
        int _operationTimeoutMs = 0;
        QString _seedHosts;       ///< host list of the connect URI
        QString _serverAddress;   ///< serverAddress(), cached until the next connect
    };
}
