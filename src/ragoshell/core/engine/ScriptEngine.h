#pragma once

#include <atomic>
#include <memory>

#include <QObject>

#include "ragoshell/core/domain/MongoShellResult.h"
#include "ragoshell/core/engine/ShellBackend.h"
#include "ragoshell/core/Enums.h"

class QString;

namespace RagoShell
{
    class ConnectionSettings;
    class IMongoTransport;

    /// The synchronous shell API MongoWorker uses. The work is done by a ShellBackend (the
    /// embedded JavaScript engine, see ShellBackend.h).
    class ScriptEngine : public QObject
    {
        Q_OBJECT

    public:
        ScriptEngine(ConnectionSettings *connection, int timeoutSec);
        ~ScriptEngine();

        void setTransport(IMongoTransport *transport, const std::string &initError = std::string());

        void init(const std::string& serverAddr = "", const std::string& dbName = "");
        MongoShellExecResult exec(const std::string &script, const std::string &dbName = std::string(),
                                  AggrInfo aggrInfo = AggrInfo());
        void interrupt();
        void clearInterrupt();

        void use(const std::string &dbName);
        void setBatchSize(int batchSize);
        /// JavaScript the shell runs before every query (sent on (re)connect).
        void setStartupScript(const QString &script) { _startupScript = script; }
        void ping();
        QStringList complete(const std::string &prefix, const AutocompletionMode mode);

        bool failedScope() const { return _failedScope; }
        const std::string &lastInitError() const { return _lastInitError; }
        const char *backendName() const { return _backend->name(); }

        void changeTimeout(int newTimeout);

    private:
        ShellConnectOptions connectOptions(const std::string &dbName) const;
        /// Takes a pending interrupt request (true if there was one) from the engine and the backend.
        bool takeInterrupt();

        ConnectionSettings *_connection;
        IMongoTransport *_transport = nullptr;
        std::string _transportInitError;
        std::unique_ptr<ShellBackend> _backend;
        std::atomic<bool> _interruptRequested;
        int _batchSize = 50;
        int _timeoutSec;
        QString _startupScript;
        bool _failedScope = false;
        bool _initialized;
        std::string _lastInitError;
    };
}
