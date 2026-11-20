#pragma once

#include <QString>

#include <memory>

#include "ragoshell/core/transport/IMongoTransport.h"

class QWebSocket;

namespace RagoShell
{
    /// IMongoTransport over a WebSocket to a gateway/agent
    /// (docs/LocalAgent.md §3, roadmap 8.3). Used for Mod C
    /// ("ws://127.0.0.1:<port>" + pairing token) and later self-hosted /
    /// hosted gateways ("wss://…") — roadmap 8.4/8.5.
    ///
    /// Ops are synchronous per the interface: each request pumps a short-lived
    /// QEventLoop until the matching-id reply, socket close or the per-op
    /// timeout. The socket is created in connect() so it lives on the worker
    /// thread that drives it (MongoWorker calls transports on its own thread).
    class WebSocketGatewayTransport : public IMongoTransport
    {
    public:
        /// url: ws:// or wss:// endpoint. token: pairing token for the hello
        /// handshake (empty when the peer does not require one — self-hosted
        /// gateways may still want one).
        WebSocketGatewayTransport(const QString &url, const QString &token);
        ~WebSocketGatewayTransport() override;

        Result connect(const QString &ejsonSettings) override;
        Result runCommand(const QString &database, const QString &ejsonCommand) override;
        Result listDatabases() override;
        Result listCollections(const QString &database) override;
        Result ping() override;
        Result killOp(const QString &ejsonFilter) override;
        void disconnect() override;
        bool isLive() const override { return true; }

        /// Test hook: override the default kOpTimeoutMs per-op deadline.
        void setOpTimeoutMs(int ms) { _opTimeoutMs = ms; }

    private:
        Result call(const QString &op, const QString &paramKey, const QString &paramValue);
        Result call(const QString &op, const QString &keyA, const QString &valueA,
                    const QString &keyB, const QString &valueB);

        QString _url;
        QString _token;
        std::unique_ptr<QWebSocket> _socket;
        quint32 _nextId = 1;
        int _opTimeoutMs;
    };
}
