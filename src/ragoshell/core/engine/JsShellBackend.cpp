#include "ragoshell/core/engine/JsShellBackend.h"

#include <chrono>
#include <condition_variable>
#include <thread>

#include <QCoreApplication>
#include <QElapsedTimer>

#include <bsoncxx/builder/basic/document.hpp>

#include "ragoshell/core/domain/MongoDocument.h"
#include "ragoshell/core/utils/Logger.h"

namespace RagoShell
{
    // core/engine/js/shell-runtime.js, compiled in by src/ragoshell/CMakeLists.txt
    extern const unsigned char kShellRuntimeScript[];
    extern const std::size_t kShellRuntimeScriptSize;

    namespace
    {
        QString interruptedText() { return QCoreApplication::translate("ScriptEngine", "Interrupted by user"); }
        QString timeoutText() { return QCoreApplication::translate("ScriptEngine", "Timeout reached"); }

        std::string utf8(const QJSValue &v) { return v.toString().toStdString(); }

        // The client being opened on a thread of its own (see connect()).
        struct PendingConnection
        {
            std::mutex mutex;
            std::condition_variable changed;
            bool done = false;
            bool ok = false;
            QString error;
            std::unique_ptr<mongocxx::client> client;
        };

        // Ends the script after its time limit.
        struct Watchdog
        {
            std::mutex mutex;
            std::condition_variable changed;
            bool finished = false;
        };
    }

    JsShellBackend::JsShellBackend() = default;

    JsShellBackend::~JsShellBackend()
    {
        _interruptible = nullptr;
        _shell = QJSValue();   // the QJSValue must not outlive its engine
        _host.reset();
        _engine.reset();
    }

    bool JsShellBackend::isRunning() const
    {
        return _host && _host->isConnected();
    }

    QString JsShellBackend::ensureEngine()
    {
        if (_engine)
            return QString();

        auto engine = std::make_unique<QJSEngine>();
        auto host = std::make_unique<ShellHost>(engine.get());
        host->install();

        const QString script = QString::fromUtf8(reinterpret_cast<const char *>(kShellRuntimeScript),
                                                 static_cast<qsizetype>(kShellRuntimeScriptSize));
        QJSValue shell = engine->evaluate(script, QStringLiteral("shell-runtime.js"));
        if (shell.isError() || !shell.isObject()) {
            const QString message = QStringLiteral("shell-runtime.js: %1 (line %2)")
                                        .arg(shell.toString(), shell.property(QStringLiteral("lineNumber")).toString());
            LOG_MSG("shell: " + message.toStdString(), LogSeverity::Error);
            return message;
        }

        _engine = std::move(engine);
        _host = std::move(host);
        _shell = shell;
        _interruptible = _engine.get();
        return QString();
    }

    ShellReply JsShellBackend::connect(const ShellConnectOptions &options)
    {
        ShellReply reply;
        if (_interrupted.exchange(false)) {
            reply.status = ShellStatus::Interrupted;
            reply.message = interruptedText();
            return reply;
        }

        const QString engineError = ensureEngine();
        if (!engineError.isEmpty()) {
            reply.status = ShellStatus::Failed;
            reply.message = engineError;
            return reply;
        }

        // The driver blocks until the server answers or the selection time is up. It runs on a thread of
        // its own so that a Stop is answered at once; a thread that is given up lets its client go when it
        // is done.
        _host->disconnect();
        const std::string appName = ShellHost::newShellAppName();
        auto pending = std::make_shared<PendingConnection>();
        std::thread([pending, options, appName]() {
            QString error;
            std::unique_ptr<mongocxx::client> client;
            const bool ok = ShellHost::openClient(options, appName, &client, &error);
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->ok = ok;
            pending->error = error;
            pending->client = std::move(client);
            pending->done = true;
            pending->changed.notify_all();
        }).detach();

        {
            std::unique_lock<std::mutex> lock(pending->mutex);
            while (!pending->done) {
                pending->changed.wait_for(lock, std::chrono::milliseconds(25));
                if (_interrupted.load()) {
                    reply.status = ShellStatus::Interrupted;
                    reply.message = interruptedText();
                    return reply;
                }
            }
        }
        if (!pending->ok) {
            reply.status = ShellStatus::Failed;
            reply.message = pending->error;
            return reply;
        }
        _host->adopt(std::move(pending->client), appName, options);

        QJSValue config = _engine->newObject();
        config.setProperty(QStringLiteral("dbName"), options.dbName);
        config.setProperty(QStringLiteral("batchSize"), options.batchSize);
        config.setProperty(QStringLiteral("startupScript"), options.startupScript);
        const QJSValue startupError = _shell.property(QStringLiteral("configure")).call({ config });
        if (startupError.isError()) {
            reply.status = ShellStatus::Failed;
            reply.message = startupError.toString();
            _host->disconnect();
            return reply;
        }
        reply.startupError = startupError.toString();
        return reply;
    }

    void JsShellBackend::disconnect()
    {
        if (_host)
            _host->disconnect();
    }

    void JsShellBackend::kill()
    {
        disconnect();
    }

    void JsShellBackend::stopRunningScript()
    {
        if (QJSEngine *engine = _interruptible.load())
            engine->setInterrupted(true);

        // Kill what the script has running on the server: a call the driver is blocked in only ends
        // that way. Once per script, on a thread of its own with a connection of its own.
        std::shared_ptr<std::atomic<bool>> running;
        ShellHost::KillTarget target;
        {
            std::lock_guard<std::mutex> lock(_runMutex);
            if (!_running || !_running->load() || _killStarted)
                return;
            _killStarted = true;
            running = _running;
            target = _killTarget;
        }
        std::thread([running, target]() { ShellHost::killOperations(target, *running); }).detach();
    }

    void JsShellBackend::interrupt()
    {
        _interrupted = true;
        stopRunningScript();
    }

    void JsShellBackend::clearInterrupt()
    {
        _interrupted = false;
        _timedOut = false;
        if (QJSEngine *engine = _interruptible.load())
            engine->setInterrupted(false);
    }

    // One statement's result item (see materializeValue in shell-runtime.js) as the grid needs it.
    ShellItem JsShellBackend::toItem(const QJSValue &item, int batchSize)
    {
        ShellItem out;
        out.type = utf8(item.property(QStringLiteral("type")));
        out.output = utf8(item.property(QStringLiteral("output")));
        out.statement = utf8(item.property(QStringLiteral("statement")));
        BsonJsConverter &bson = _host->converter();

        try {
            // A cursor: the first page goes straight from BSON to MongoDocument.
            const QJSValue cursorId = item.property(QStringLiteral("cursorId"));
            if (cursorId.isNumber()) {
                const int id = cursorId.toInt();
                const int pageSize = item.property(QStringLiteral("pageSize")).toInt();
                try {
                    out.documents = _host->takeDocuments(id, pageSize > 0 ? pageSize : batchSize);
                } catch (...) {
                    _host->cursorClose(id);
                    throw;
                }
                _host->cursorClose(id);
            } else {
                const QJSValue documents = item.property(QStringLiteral("documents"));
                const quint32 length = documents.property(QStringLiteral("length")).toUInt();
                for (quint32 i = 0; i < length; ++i)
                    out.documents.push_back(MongoDocumentPtr(new MongoDocument(bson.toDocument(documents.property(i)))));
            }

            const QJSValue queryInfo = item.property(QStringLiteral("queryInfo"));
            if (queryInfo.isObject()) {
                const QJSValue fields = queryInfo.property(QStringLiteral("fields"));
                out.queryInfo = MongoQueryInfo(
                    CollectionInfo(utf8(item.property(QStringLiteral("server"))),
                                   utf8(queryInfo.property(QStringLiteral("db"))),
                                   utf8(queryInfo.property(QStringLiteral("coll")))),
                    bson.toDocument(queryInfo.property(QStringLiteral("query"))),
                    fields.isObject() ? bson.toDocument(fields) : bsoncxx::builder::basic::make_document(),
                    queryInfo.property(QStringLiteral("limit")).toInt(),
                    queryInfo.property(QStringLiteral("skip")).toInt(),
                    queryInfo.property(QStringLiteral("batchSize")).toInt(),
                    0,
                    false);
            }

            const QJSValue aggrInfo = item.property(QStringLiteral("aggrInfo"));
            if (aggrInfo.isObject()) {
                out.aggrInfo = AggrInfo(
                    utf8(aggrInfo.property(QStringLiteral("coll"))),
                    0, batchSize,
                    bson.toDocument(aggrInfo.property(QStringLiteral("pipeline"))),
                    bson.toDocument(aggrInfo.property(QStringLiteral("options"))),
                    0);
                out.hasAggrInfo = true;
            }
        } catch (const std::exception &e) {
            // A value the grid cannot hold, or a server error met while the first page was read: show the
            // problem as the statement's result.
            out.type = "error";
            out.output = (dynamic_cast<const BsonConversionError *>(&e) ? QStringLiteral("Error: ") + QString::fromUtf8(e.what())
                                                                         : ShellHost::describeError(e).text()).toStdString();
            out.documents.clear();
            out.queryInfo = MongoQueryInfo();
            out.hasAggrInfo = false;
        }
        return out;
    }

    ShellEvalReply JsShellBackend::eval(const ShellEvalRequest &request)
    {
        ShellEvalReply reply;
        if (!isRunning()) {
            reply.status = ShellStatus::Failed;
            reply.message = QStringLiteral("not connected to a MongoDB server");
            return reply;
        }
        if (_interrupted.load()) {
            reply.status = ShellStatus::Interrupted;
            reply.message = interruptedText();
            return reply;
        }

        // While the script runs, interrupt() and the watchdog can find and kill what it has on the server.
        auto running = std::make_shared<std::atomic<bool>>(true);
        {
            std::lock_guard<std::mutex> lock(_runMutex);
            _running = running;
            _killTarget = _host->killTarget();
            _killStarted = false;
        }
        _timedOut = false;
        _host->setOperationTimeoutMs(request.timeoutMs);

        // The time limit of the script (the shell timeout): a timer that stops it.
        auto watch = std::make_shared<Watchdog>();
        std::thread watchdog([this, watch, limit = request.timeoutMs]() {
            std::unique_lock<std::mutex> lock(watch->mutex);
            if (watch->changed.wait_for(lock, std::chrono::milliseconds(limit), [&] { return watch->finished; }))
                return;
            lock.unlock();
            _timedOut = true;
            stopRunningScript();
        });

        QElapsedTimer timer;
        timer.start();
        const QJSValue result = _shell.property(QStringLiteral("evalScript"))
                                    .call({ QJSValue(request.script), QJSValue(request.dbName),
                                            QJSValue(request.batchSize) });
        reply.elapsedMs = timer.elapsed();

        {
            std::lock_guard<std::mutex> lock(watch->mutex);
            watch->finished = true;
        }
        watch->changed.notify_all();
        watchdog.join();
        running->store(false);

        if (_timedOut.load()) {
            reply.status = ShellStatus::Timeout;
            reply.message = timeoutText();
            return reply;
        }
        if (_interrupted.load()) {
            reply.status = ShellStatus::Interrupted;
            reply.message = interruptedText();
            return reply;
        }
        if (result.isError()) {
            // A bug in the runtime itself (script errors come back as result items).
            reply.status = ShellStatus::Error;
            reply.message = result.toString();
            return reply;
        }

        const QJSValue results = result.property(QStringLiteral("results"));
        const quint32 length = results.property(QStringLiteral("length")).toUInt();
        for (quint32 i = 0; i < length; ++i)
            reply.items.push_back(toItem(results.property(i), request.batchSize));
        reply.activeDb = utf8(result.property(QStringLiteral("dbName")));
        return reply;
    }

    void JsShellBackend::use(const std::string &dbName)
    {
        if (_engine)
            _shell.property(QStringLiteral("use")).call({ QJSValue(QString::fromStdString(dbName)) });
    }

    void JsShellBackend::setBatchSize(int batchSize)
    {
        if (_engine)
            _shell.property(QStringLiteral("setBatchSize")).call({ QJSValue(batchSize) });
    }

    bool JsShellBackend::ping()
    {
        return _host && _host->ping();
    }

    QStringList JsShellBackend::complete(const std::string &prefix, bool includeCollections)
    {
        QStringList list;
        if (!isRunning())
            return list;
        const QJSValue items = _shell.property(QStringLiteral("complete"))
                                   .call({ QJSValue(QString::fromStdString(prefix)),
                                           QJSValue(includeCollections) });
        if (items.isError())
            return list;
        const quint32 length = items.property(QStringLiteral("length")).toUInt();
        for (quint32 i = 0; i < length; ++i)
            list << items.property(i).toString();
        return list;
    }
}
