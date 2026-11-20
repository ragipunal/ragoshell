#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

namespace RagoShell
{
    /**
     * @brief Transport seam between the shell/UI layer and a MongoDB path
     * (docs/WasmBuildReport.md §3, roadmap 8.1).
     *
     * The desktop app uses NativeMongoTransport (mongocxx, TCP+TLS); the WASM
     * build cannot open sockets, so the same UI talks to
     * InMemoryDemoTransport (Mod A playground) or WebSocketGatewayTransport
     * (Mod B/C). Commands and results travel as EJSON strings so the wire
     * shape is identical across transports and serializes to WebSocket frames
     * without a second codec.
     *
     * Threading: implementations run on the caller thread; MongoWorker owns
     * async sequencing as today. All methods are synchronous here because the
     * gateway hop hides latency inside the worker's existing job model.
     */
    class IMongoTransport
    {
    public:
        virtual ~IMongoTransport() = default;

        /// EJSON `{ ok, errmsg, code, codeName }` — mirrors mongoc's error triplet.
        struct Result
        {
            bool ok = false;
            QString ejson;      /// result document(s) as EJSON; array for cursors
            QString error;      /// errmsg when ok=false
            int code = 0;       /// server/command error code
            QString codeName;
        };

        /// Connect using the serialized connection settings (URI + options as
        /// the dialog stores them). Called once per session.
        virtual Result connect(const QString &ejsonSettings) = 0;
        /// Run an EJSON command document against a database (admin commands,
        /// find/aggregate, CRUD, serverStatus for the monitor).
        virtual Result runCommand(const QString &database, const QString &ejsonCommand) = 0;
        /// listDatabases / listCollections convenience (EJSON result arrays).
        virtual Result listDatabases() = 0;
        virtual Result listCollections(const QString &database) = 0;
        /// Cheap liveness probe for Server Monitor (e.g. `{ping: 1}`).
        virtual Result ping() = 0;
        /// Terminate a running operation (killOp / killSessions shape).
        virtual Result killOp(const QString &ejsonFilter) = 0;
        virtual void disconnect() = 0;

        /// True when this transport can reach real servers (native, gateway);
        /// false for in-memory demos so the UI can hide account-only affordances.
        virtual bool isLive() const = 0;
    };
}
