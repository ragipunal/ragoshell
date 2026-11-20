#pragma once

#include <QJsonObject>
#include <QString>

namespace RagoShell
{
    /// Wire envelope for the gateway/agent WebSocket protocol
    /// (docs/LocalAgent.md §1, roadmap 8.3).
    ///
    /// One JSON text frame per message. Request:
    ///   {"id":<u32>,"op":"<name>","params":{<EJSON args>}}
    /// Response:
    ///   {"id":<u32>,"ok":true,"result":<IMongoTransport::Result shape>}
    ///   {"id":<u32>,"ok":false,"error":"<message>"}
    namespace GatewayProtocol
    {
        /// Loopback port the agent binds (127.0.0.1 only). RAGOSHELL_AGENT_PORT
        /// overrides in dev.
        constexpr int kDefaultAgentPort = 17371;
        /// First frame ("hello") deadline, ms.
        constexpr int kHelloTimeoutMs = 30 * 1000;
        /// One IMongoTransport op deadline inside a session, ms.
        constexpr int kOpTimeoutMs = 60 * 1000;
        /// Max accepted text-frame bytes on the agent (an oversized command
        /// belongs to a bulk-export path, not a WS frame).
        constexpr qint64 kMaxFrameBytes = 1024 * 1024;
        /// Max UTF-8 bytes of one response frame. The session turns a larger
        /// result into an error frame and the client refuses anything above
        /// it while reading (2026-11-04 review R2/R7) — Qt's own default is
        /// ~2 GiB per message.
        constexpr qint64 kMaxResponseBytes = 48LL * 1024 * 1024;
        /// Unsent reply bytes a session tolerates before it treats the peer
        /// as not reading and aborts (R7). The client sends one request at a
        /// time and reads the whole reply first, so it never gets near this.
        constexpr qint64 kMaxPendingWriteBytes = 8LL * 1024 * 1024;
        /// After a close starts, a peer that has not gone away within this
        /// grace is aborted — a graceful close waits for the write queue
        /// to drain, which never happens for a stalled reader (R7).
        constexpr int kCloseGraceMs = 5 * 1000;
        /// Concurrent sockets per agent.
        constexpr int kMaxSessions = 8;
        /// Idle session lifetime, ms.
        constexpr int kIdleTimeoutMs = 30 * 60 * 1000;

        /// IMongoTransport op names on the wire.
        namespace Op
        {
            constexpr const char *Hello = "hello";
            constexpr const char *Connect = "connect";
            constexpr const char *RunCommand = "runCommand";
            constexpr const char *ListDatabases = "listDatabases";
            constexpr const char *ListCollections = "listCollections";
            constexpr const char *Ping = "ping";
            constexpr const char *KillOp = "killOp";
            constexpr const char *Disconnect = "disconnect";
            constexpr const char *IsLive = "isLive";
        }

        QString encodeRequest(quint32 id, const QString &op, const QJsonObject &params);
        /// Server-side decode: fills id/op/params; false on malformed JSON or
        /// a missing/invalid shape (empty op, id not a number).
        bool decodeRequest(const QString &frame, quint32 *id, QString *op,
                           QJsonObject *params, QString *error);

        QString encodeOk(quint32 id, const QString &resultEjson);
        QString encodeError(quint32 id, const QString &message);
        /// Client-side decode of a response for `id`; false (with *error) on a
        /// malformed frame or an id mismatch. gotId (optional) receives the
        /// frame's id whenever it is a valid u32, so a caller can tell a
        /// late reply to an earlier request from a real failure.
        bool decodeResponse(const QString &frame, quint32 expectedId,
                            QString *resultEjson, QString *error,
                            qint64 *gotId = nullptr);
    }
}
