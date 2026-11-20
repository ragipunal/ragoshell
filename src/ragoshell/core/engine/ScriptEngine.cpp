#include "ragoshell/core/engine/ScriptEngine.h"

#include <algorithm>
#include <stdexcept>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QStringList>

#include "ragoshell/core/mongodb/MongoUriUtils.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/utils/QtUtils.h"

// ScriptEngine stays a synchronous API (as MongoWorker expects). Running the scripts is the
// job of a ShellBackend (ShellBackend.h): the embedded engine of docs/Shell.md.

namespace RagoShell
{
    ScriptEngine::ScriptEngine(ConnectionSettings *connection, int timeoutSec) :
        _connection(connection),
        _backend(createShellBackend()),
        _interruptRequested(false),
        _timeoutSec(timeoutSec > 0 ? timeoutSec : 15),
        _initialized(false)
    {
        _backend->setRequestTimeoutMs(_timeoutSec * 1000);
    }

    ScriptEngine::~ScriptEngine() = default;

    void ScriptEngine::changeTimeout(int newTimeout)
    {
        _timeoutSec = newTimeout > 0 ? newTimeout : 15;
        _backend->setRequestTimeoutMs(_timeoutSec * 1000);
    }

    bool ScriptEngine::takeInterrupt()
    {
        const bool requested = _interruptRequested.exchange(false);
        _backend->clearInterrupt();
        return requested;
    }

    ShellConnectOptions ScriptEngine::connectOptions(const std::string &dbName) const
    {
        const int connectTimeoutMs = std::max(15, _timeoutSec) * 1000;

        ShellConnectOptions opts;
        opts.uri = QString::fromStdString(buildMongoUri(*_connection));
        opts.batchSize = _batchSize;
        opts.connectTimeoutMs = connectTimeoutMs;
        opts.serverSelectionTimeoutMs = std::max(5000, connectTimeoutMs - 2000);
        // directConnection is rejected by drivers for mongodb+srv URIs.
        opts.directConnection = !_connection->isReplicaSet() && !_connection->isSrv();
        opts.dbName = QString::fromStdString(!dbName.empty() ? dbName : _connection->defaultDatabase());
        opts.startupScript = _startupScript;
        opts.operationTimeoutMs = _timeoutSec * 1000;

        if (const SslSettings *ssl = _connection->sslSettings(); ssl && ssl->sslEnabled()) {
            opts.tls = true;
            if (ssl->allowInvalidCertificates()) {
                opts.tlsAllowInvalidCertificates = true;
            } else if (!ssl->caFile().empty()) {
                opts.tlsCAFile = QString::fromStdString(ssl->caFile());
            }
            opts.tlsAllowInvalidHostnames = ssl->allowInvalidHostnames();
            if (ssl->usePemFile() && !ssl->pemKeyFile().empty()) {
                opts.tlsCertificateKeyFile = QString::fromStdString(ssl->pemKeyFile());
                if (!ssl->pemPassPhrase().empty())
                    opts.tlsCertificateKeyFilePassword = QString::fromStdString(ssl->pemPassPhrase());
            }
            if (ssl->useAdvancedOptions() && !ssl->crlFile().empty())
                opts.crlFile = QString::fromStdString(ssl->crlFile());
        }
        return opts;
    }

    void ScriptEngine::init(const std::string& /*serverAddr*/, const std::string& dbName)
    {
        if (takeInterrupt()) {
            _failedScope = true;
            _initialized = false;
            _lastInitError = "Interrupted by user";
            return;
        }
        _failedScope = false;
        _initialized = false;

        const ShellReply reply = _backend->connect(connectOptions(dbName));
        if (!reply.ok()) {
            if (reply.status == ShellStatus::Interrupted || reply.status == ShellStatus::Timeout)
                takeInterrupt();
            _failedScope = true;
            _lastInitError = reply.message.toStdString();
            if (reply.status != ShellStatus::Interrupted)
                LOG_MSG("shell: connect failed: " + _lastInitError, LogSeverity::Error);
            return;
        }

        if (!reply.startupError.isEmpty())
            LOG_MSG("shell: startup script not loaded: " + reply.startupError.toStdString(),
                    LogSeverity::Warning);

        _lastInitError.clear();
        _initialized = true;
    }

    MongoShellExecResult ScriptEngine::exec(const std::string &script,
                                            const std::string &dbName /* = std::string() */,
                                            AggrInfo aggrInfo /* = AggrInfo() */)
    {
        if (takeInterrupt())
            return MongoShellExecResult(true, "Interrupted by user", false, /*interrupted=*/true);

        if (!_initialized || !_backend->isRunning()) {
            init("", dbName);
            if (!_initialized || !_backend->isRunning()) {
                _failedScope = true;
                if (_lastInitError == "Interrupted by user")
                    return MongoShellExecResult(true, "Interrupted by user", false, /*interrupted=*/true);
                const std::string detail = _lastInitError.empty()
                    ? "JavaScript shell is not initialized"
                    : ("JavaScript shell is not initialized: " + _lastInitError);
                return MongoShellExecResult(true, detail);
            }
        }

        if (takeInterrupt())
            return MongoShellExecResult(true, "Interrupted by user", false, /*interrupted=*/true);

        QElapsedTimer timer;
        timer.start();

        ShellEvalRequest request;
        request.script = QString::fromStdString(script);
        request.dbName = QString::fromStdString(dbName);
        request.batchSize = aggrInfo.isValid && aggrInfo.batchSize > 0 ? aggrInfo.batchSize : _batchSize;
        request.timeoutMs = _timeoutSec * 1000;

        ShellEvalReply reply = _backend->eval(request);
        if (reply.status == ShellStatus::Failed || reply.status == ShellStatus::Timeout
            || reply.status == ShellStatus::Interrupted) {
            // Timeout/interrupt/death: stop the backend immediately and let init() restart it.
            _backend->kill();
            takeInterrupt();
            _failedScope = true;
            _initialized = false;
            return MongoShellExecResult(true,
                (reply.message.isEmpty() ? QStringLiteral("JavaScript shell evaluation failed") : reply.message).toStdString(),
                reply.status == ShellStatus::Timeout,
                reply.status == ShellStatus::Interrupted);
        }
        if (reply.status == ShellStatus::Error)
            return MongoShellExecResult(true, reply.message.toStdString());

        const qint64 totalElapsed = reply.elapsedMs >= 0 ? reply.elapsedMs : timer.elapsed();

        std::vector<MongoShellResult> parsedResults;
        const qint64 count = static_cast<qint64>(reply.items.size());
        const qint64 perItemElapsed = count > 0 ? std::max<qint64>(0, totalElapsed / count) : 0;
        for (qint64 i = 0; i < count; ++i) {
            ShellItem &item = reply.items[static_cast<size_t>(i)];
            const qint64 share = (i == count - 1)
                ? (totalElapsed - perItemElapsed * (count - 1))
                : perItemElapsed;
            const std::string stmt = item.statement.empty() ? script : item.statement;

            AggrInfo outAggr;
            if (aggrInfo.isValid)
                outAggr = aggrInfo;
            else if (item.hasAggrInfo)
                outAggr = item.aggrInfo;

            parsedResults.push_back(MongoShellResult(item.type, item.output, item.documents,
                                                     item.queryInfo, stmt, share, outAggr));
        }

        const std::string server = _connection ? _connection->getFullAddress() : "";
        std::string activeDb = reply.activeDb;
        if (activeDb.empty() && _connection)
            activeDb = _connection->defaultDatabase();

        return MongoShellExecResult(parsedResults, server, !server.empty(),
                                    activeDb, !activeDb.empty(), false);
    }

    void ScriptEngine::interrupt()
    {
        _interruptRequested = true;
        _backend->interrupt();
    }

    void ScriptEngine::clearInterrupt()
    {
        _interruptRequested = false;
        _backend->clearInterrupt();
    }

    void ScriptEngine::use(const std::string &dbName)
    {
        _backend->use(dbName);
    }

    void ScriptEngine::setBatchSize(int batchSize)
    {
        _batchSize = batchSize;
        _backend->setBatchSize(batchSize);
    }

    void ScriptEngine::ping()
    {
        if (!_initialized)
            return;
        if (!_backend->ping()) {
            _failedScope = true;
            _initialized = false;
        }
    }

    QStringList ScriptEngine::complete(const std::string &prefix,
                                       const AutocompletionMode mode)
    {
        if (mode == AutocompleteNone)
            return QStringList();
        if (!_initialized || !_backend->isRunning()) {
            init("", _connection ? _connection->defaultDatabase() : std::string());
            if (!_initialized || !_backend->isRunning())
                return QStringList();
        }
        return _backend->complete(prefix, mode != AutocompleteNoCollectionNames);
    }
}
