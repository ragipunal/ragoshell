#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include <QJSEngine>
#include <QJSValue>

#include "ragoshell/core/engine/ShellBackend.h"
#include "ragoshell/core/engine/ShellHost.h"

namespace RagoShell
{
    /// The embedded shell (docs/Shell.md): the shell API of
    /// core/engine/js/shell-runtime.js running in a QJSEngine, talking to MongoDB through ShellHost.
    /// The engine lives on the thread that connects; use the backend from that thread (interrupt()
    /// excepted: it may be called from any thread).
    ///
    /// Stopping a script. interrupt() and the time limit of a script (the watchdog of eval()) do two
    /// things: QJSEngine::setInterrupted() ends JavaScript that loops, and a second connection kills
    /// (killOp) the operations the shell has running on the server, which ends a call the driver is
    /// blocked in. connect() opens the client on a thread of its own, so a Stop does not wait for a
    /// server that does not answer.
    class JsShellBackend : public ShellBackend
    {
    public:
        JsShellBackend();
        ~JsShellBackend() override;

        const char *name() const override { return "js"; }
        bool isRunning() const override;

        ShellReply connect(const ShellConnectOptions &options) override;
        void disconnect() override;
        void kill() override;

        ShellEvalReply eval(const ShellEvalRequest &request) override;
        void use(const std::string &dbName) override;
        void setBatchSize(int batchSize) override;
        bool ping() override;
        QStringList complete(const std::string &prefix, bool includeCollections = true) override;

        void setRequestTimeoutMs(int timeoutMs) override { _timeoutMs = timeoutMs; }
        void interrupt() override;
        void clearInterrupt() override;

    private:
        /// Creates the engine, the host and loads the runtime. Returns an error text, or empty.
        QString ensureEngine();
        ShellItem toItem(const QJSValue &item, int batchSize);
        /// Stops what the running script is doing: the engine, and the operations on the server.
        void stopRunningScript();

        std::unique_ptr<QJSEngine> _engine;
        std::unique_ptr<ShellHost> _host;
        QJSValue _shell; ///< the object the runtime script evaluates to
        std::atomic<QJSEngine *> _interruptible{ nullptr };
        std::atomic<bool> _interrupted{ false };
        std::atomic<bool> _timedOut{ false };
        int _timeoutMs = 15000;

        // What interrupt() (any thread) needs while a script runs.
        std::mutex _runMutex;
        std::shared_ptr<std::atomic<bool>> _running; ///< true while eval() runs; one per eval
        ShellHost::KillTarget _killTarget;
        bool _killStarted = false;
    };
}
