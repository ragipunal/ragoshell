#pragma once

#include <memory>
#include <string>
#include <vector>

#include <QString>
#include <QStringList>

#include "ragoshell/core/domain/MongoShellResult.h"

// The shell runs behind ScriptEngine in a ShellBackend: the embedded engine, JsShellBackend (the shell
// runtime of docs/Shell.md on Qt's QJSEngine, the same on every platform). ScriptEngine keeps the
// public API the rest of the application uses; the interface is the seam it talks to.

namespace RagoShell
{
    class IMongoTransport;

    /// What a backend needs to connect: filled by ScriptEngine from ConnectionSettings.
    struct ShellConnectOptions
    {
        QString uri;
        QString dbName;
        int batchSize = 50;
        int connectTimeoutMs = 15000;
        int serverSelectionTimeoutMs = 13000;
        /// The shell's time limit for a script: bounds socket reads and the server-side time of reads.
        int operationTimeoutMs = 0;
        bool directConnection = false;
        QString startupScript;

        /// Non-owning transport seam (WASM demo or WebSocket gateway) bound to the backend thread.
        /// When non-null, ShellHost executes commands over this transport instead of mongocxx::client.
        IMongoTransport *transport = nullptr;
        QString transportAddress;

        bool tls = false;
        bool tlsAllowInvalidCertificates = false;
        bool tlsAllowInvalidHostnames = false;
        QString tlsCAFile;
        QString tlsCertificateKeyFile;
        QString tlsCertificateKeyFilePassword;
        QString crlFile;
    };

    struct ShellEvalRequest
    {
        QString script;
        QString dbName;
        int batchSize = 50;
        int timeoutMs = 15000;
    };

    /// Outcome of a backend call. `Error` is an answer from the shell (script or server error:
    /// the backend is still usable); `Failed`, `Timeout` and `Interrupted` mean the backend
    /// itself did not answer and ScriptEngine restarts it.
    enum class ShellStatus
    {
        Ok,
        Error,
        Failed,
        Timeout,
        Interrupted
    };

    struct ShellReply
    {
        ShellStatus status = ShellStatus::Ok;
        QString message;
        /// connect only: the Preferences startup script did not load (the connection still works).
        QString startupError;

        bool ok() const { return status == ShellStatus::Ok; }
    };

    /// One statement's result, as the grid and the output pane need it.
    struct ShellItem
    {
        std::string type;
        std::string output;
        std::string statement; ///< empty: the whole script
        std::vector<MongoDocumentPtr> documents;
        MongoQueryInfo queryInfo;
        AggrInfo aggrInfo; ///< valid only when the statement was an aggregate
        bool hasAggrInfo = false;
    };

    struct ShellEvalReply : ShellReply
    {
        std::vector<ShellItem> items;
        std::string activeDb;
        /// Time the call took, without converting the answer; -1: not measured.
        long long elapsedMs = -1;
    };

    class ShellBackend
    {
    public:
        virtual ~ShellBackend() = default;

        virtual const char *name() const = 0;
        virtual bool isRunning() const = 0;

        /// Starts the backend if needed and connects. A running backend is restarted.
        virtual ShellReply connect(const ShellConnectOptions &options) = 0;
        /// Stops the backend gracefully.
        virtual void disconnect() = 0;
        /// Stops it at once (after a timeout, an interrupt or a lost answer).
        virtual void kill() = 0;

        virtual ShellEvalReply eval(const ShellEvalRequest &request) = 0;
        virtual void use(const std::string &dbName) = 0;
        virtual void setBatchSize(int batchSize) = 0;
        virtual bool ping() = 0;
        virtual QStringList complete(const std::string &prefix, bool includeCollections = true) = 0;

        /// Timeout of the calls that carry none themselves (use, ping, complete).
        virtual void setRequestTimeoutMs(int timeoutMs) = 0;
        /// May be called from any thread; makes the running call return Interrupted.
        virtual void interrupt() = 0;
        virtual void clearInterrupt() = 0;
    };

    std::unique_ptr<ShellBackend> createShellBackend();
}
