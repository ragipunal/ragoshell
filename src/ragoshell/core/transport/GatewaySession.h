#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>

class QWebSocket;

namespace RagoShell
{
    class DeviceSessionLedger;
    class EgressPolicy;
    class EntitlementVerifier;
    class IMongoTransport;
    struct EntitlementVerifyOptions;

    /// Optional hosted-mode authentication for GatewaySession
    /// (docs/HostedGateway.md §2): when set, the hello `token` is verified
    /// as an ES256 entitlement+jwt instead of the pairing token —
    /// signature + typ/iss/aud + exp + hosted_bridge + plan status, then a
    /// per-device slot from the ledger.
    ///
    /// verifier/ledger are shared_ptrs: sessions copy this config, and the
    /// pointers must stay valid through session destruction even if the
    /// listener tears its own references down first (8.6 review L4).
    struct HostedAuthConfig
    {
        std::shared_ptr<const EntitlementVerifier> verifier; // required in hosted mode
        std::shared_ptr<const EntitlementVerifyOptions> options;
        std::shared_ptr<DeviceSessionLedger> ledger;         // per-did cap, optional
    };

    /// Server-side handler for one agent WebSocket (docs/LocalAgent.md §2).
    ///
    /// Lifecycle: origin+Host are checked at construction; the first frame
    /// must be {"op":"hello","params":{"token":"…"}} within
    /// kHelloTimeoutMs — constant-time compare, socket closes otherwise.
    /// Then each frame is a GatewayProtocol request routed to this session's
    /// own IMongoTransport (made by the injected factory on `connect`).
    ///
    /// The session owns its socket lifetime (parented here) and emits
    /// finished() when it closes so the agent can free it.
    class GatewaySession : public QObject
    {
        Q_OBJECT
    public:
        using TransportFactory = std::function<std::unique_ptr<IMongoTransport>()>;

        /// socket must be an already-accepted QWebSocket; allowedOrigins are
        /// exact origins or "*" entries; requiredHost is e.g. "127.0.0.1:17371"
        /// (also accepts the same port on "localhost"; EMPTY disables the
        /// Host check — public gateways can't enumerate their own names).
        /// expectedToken is the pairing token compared in constant time.
        /// egress, when non-null, SSRF-filters the `connect` op's URI and the
        /// transport is handed EgressPolicy's rewritten URI, never the
        /// client's original (web gateway only). originOverride supplies the Origin when the
        /// socket was produced by QAbstractHttpServer upgrade and Qt did not
        /// populate origin() (verified empty → fall back to the header the
        /// upgrade verifier already inspected).
        GatewaySession(QWebSocket *socket,
                       const QStringList &allowedOrigins,
                       const QString &requiredHost,
                       const QString &expectedToken,
                       TransportFactory factory,
                       QObject *parent = nullptr,
                       const EgressPolicy *egress = nullptr,
                       const QString &originOverride = QString(),
                       const HostedAuthConfig *hosted = nullptr);
        ~GatewaySession() override; // out-of-line: unique_ptr<IMongoTransport>

        /// Test hook: override the default kIdleTimeoutMs session-idle limit.
        void setIdleTimeoutMs(int ms) { _idleMs = ms; }
        /// Test hook: override the GatewayProtocol::kMaxResponseBytes cap.
        void setMaxResponseBytes(qint64 bytes) { _maxResponseBytes = bytes; }

    signals:
        void finished();

    private slots:
        void onTextMessage(const QString &frame);
        void onDisconnected();

    private:
        void failAndClose(const QString &reason);
        /// Once a close has started, abort a peer that has not gone away
        /// within kCloseGraceMs — a graceful close never completes while
        /// replies sit unread in the write queue (2026-11-04 review R7).
        void armAbort();
        /// Sends the reply for `id`; a result over the response cap becomes
        /// an error frame instead (review R7).
        void sendResult(quint32 id, bool ok, const QString &ejson, const QString &error);
        static bool tokenEquals(const QString &a, const QString &b);

        QWebSocket *_socket;
        QString _expectedToken;
        TransportFactory _factory;
        const EgressPolicy *_egress;
        QString _originOverride;
        HostedAuthConfig _hosted;
        QString _authKey; // device/sub key acquired in the ledger (hosted mode)
        std::unique_ptr<IMongoTransport> _transport;
        QTimer _idle;
        QTimer _expiry;       // hosted: closes the session at the JWT's exp
        qint64 _expiresAt = 0; // hosted: JWT exp (unix s), 0 = no expiry
        qint64 _maxResponseBytes;
        int _idleMs;
        bool _authed = false;
        bool _closing = false; // set by failAndClose — drop later frames
        bool _abortArmed = false;
    };
}
