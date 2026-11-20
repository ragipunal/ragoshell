#include "ragoshell/core/transport/GatewaySession.h"

#include <QJsonDocument>
#include <QNetworkRequest>
#include <QTimer>
#include <QWebSocket>

#include "ragoshell/core/cloud/EntitlementToken.h"
#include "ragoshell/core/transport/DeviceSessionLedger.h"
#include "ragoshell/core/transport/EgressPolicy.h"
#include "ragoshell/core/transport/GatewayProtocol.h"
#include "ragoshell/core/transport/IMongoTransport.h"

#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

#include <QDateTime>

#include <limits>

namespace RagoShell
{
    GatewaySession::GatewaySession(QWebSocket *socket,
                                   const QStringList &allowedOrigins,
                                   const QString &requiredHost,
                                   const QString &expectedToken,
                                   TransportFactory factory,
                                   QObject *parent,
                                   const EgressPolicy *egress,
                                   const QString &originOverride,
                                   const HostedAuthConfig *hosted)
        : QObject(parent), _socket(socket), _expectedToken(expectedToken),
          _factory(std::move(factory)), _egress(egress),
          _originOverride(originOverride),
          _hosted(hosted ? *hosted : HostedAuthConfig{}),
          _maxResponseBytes(GatewayProtocol::kMaxResponseBytes),
          _idleMs(GatewayProtocol::kIdleTimeoutMs)
    {
        _socket->setParent(this);

        // Enforce the frame cap while Qt reads, not after: Qt's default is
        // ~2 GiB per message, and the size check in onTextMessage only runs
        // once the whole message is buffered — 8 pre-auth sockets could
        // otherwise pin gigabytes (2026-11-04 review R2). Qt closes with
        // 1009 (too much data) when a peer exceeds it.
        _socket->setMaxAllowedIncomingMessageSize(
            static_cast<quint64>(GatewayProtocol::kMaxFrameBytes));
        _socket->setMaxAllowedIncomingFrameSize(
            static_cast<quint64>(GatewayProtocol::kMaxFrameBytes));

        // Connect lifecycle signals and arm timers BEFORE the origin/Host
        // gate below: a rejected socket must still emit finished() when its
        // close lands, otherwise the agent's session slot leaks permanently
        // (any web page can burn all kMaxSessions with 8 probes).
        connect(_socket, &QWebSocket::textMessageReceived,
                this, &GatewaySession::onTextMessage);
        connect(_socket, &QWebSocket::binaryMessageReceived,
                this, [this](const QByteArray &) {
                    failAndClose(QStringLiteral("binary frames not supported"));
                });
        connect(_socket, &QWebSocket::disconnected,
                this, &GatewaySession::onDisconnected);
        // Every close — ours, the peer's or Qt's own 1009 — must end in
        // disconnected() so finished() frees the slot (review R7).
        connect(_socket, &QWebSocket::aboutToClose, this, &GatewaySession::armAbort);
        _expiry.setSingleShot(true);
        connect(&_expiry, &QTimer::timeout,
                this, [this] { failAndClose(QStringLiteral("entitlement expired")); });

        QTimer::singleShot(GatewayProtocol::kHelloTimeoutMs, _socket, [this] {
            if (!_authed)
                failAndClose(QStringLiteral("hello timeout"));
        });
        _idle.setSingleShot(true);
        connect(&_idle, &QTimer::timeout,
                this, [this] { failAndClose(QStringLiteral("idle timeout")); });
        _idle.start(_idleMs);

        // Pre-auth gate (docs/LocalAgent.md §5 T1): Origin must be allowlisted
        // and Host must point at the loopback listener — a rebinding attack
        // can fake one but not both. requiredHost=="" disables the Host side
        // (public web gateways cannot enumerate their own names).
        const QString origin = _originOverride.isEmpty()
            ? _socket->origin() : _originOverride;
        const QString host = QString::fromUtf8(
            _socket->request().rawHeader("Host"));
        // Entries ending in ':' are prefixes (e.g. "http://localhost:" matches
        // a dev server on any port); everything else is an exact origin match.
        // "*" is the documented pass-through for deployments whose HTTP
        // upgrade verifier already enforced the allowlist (web gateway).
        bool originOk = false;
        for (const QString &allowed : allowedOrigins) {
            if (allowed == QLatin1String("*")
                || (allowed.endsWith(QLatin1Char(':'))
                        ? origin.startsWith(allowed)
                        : origin == allowed)) {
                originOk = true;
                break;
            }
        }
        bool hostOk = true;
        if (!requiredHost.isEmpty()) {
            const int colon = requiredHost.lastIndexOf(QLatin1Char(':'));
            const QString port = colon >= 0 ? requiredHost.mid(colon) : QString();
            const QStringList hosts{requiredHost, QStringLiteral("localhost") + port};
            hostOk = hosts.contains(host);
        }
        if (!originOk || !hostOk) {
            failAndClose(QStringLiteral("forbidden"));
            return;
        }
    }

    GatewaySession::~GatewaySession()
    {
        // The ledger slot is released on disconnect; this covers sessions
        // destroyed before disconnected() fired.
        if (!_authKey.isEmpty() && _hosted.ledger)
            _hosted.ledger->release(_authKey);
    }

    bool GatewaySession::tokenEquals(const QString &a, const QString &b)
    {
        const QByteArray ba = a.toUtf8(), bb = b.toUtf8();
        if (ba.size() != bb.size())
            return false;
        char diff = 0;
        for (int i = 0; i < ba.size(); ++i)
            diff |= ba.at(i) ^ bb.at(i);
        return diff == 0;
    }

    void GatewaySession::failAndClose(const QString &reason)
    {
        if (_closing)
            return; // one close per session; later timers (idle, exp) are moot
        _closing = true;
        _socket->sendTextMessage(GatewayProtocol::encodeError(0, reason));
        _socket->close(QWebSocketProtocol::CloseCodePolicyViolated, reason);
        armAbort();
    }

    void GatewaySession::armAbort()
    {
        if (_abortArmed)
            return;
        _abortArmed = true;
        QTimer::singleShot(GatewayProtocol::kCloseGraceMs, this, [this] {
            if (_socket->state() != QAbstractSocket::UnconnectedState)
                _socket->abort();
        });
    }

    void GatewaySession::sendResult(quint32 id, bool ok, const QString &ejson,
                                    const QString &error)
    {
        if (!ok) {
            _socket->sendTextMessage(GatewayProtocol::encodeError(id, error));
            return;
        }
        const QString tooLarge =
            QStringLiteral("Result too large for the gateway (over %1 bytes); "
                           "narrow the query or add a limit.").arg(_maxResponseBytes);
        // Every UTF-16 unit is at least one UTF-8 byte, so an oversize EJSON
        // string is refused before it is parsed into the envelope.
        if (ejson.size() > _maxResponseBytes) {
            _socket->sendTextMessage(GatewayProtocol::encodeError(id, tooLarge));
            return;
        }
        const QString frame = GatewayProtocol::encodeOk(id, ejson);
        qint64 utf8Bytes = frame.size();
        if (utf8Bytes * 3 > _maxResponseBytes) {
            // Exact UTF-8 length only near the cap (surrogate pairs: 2 units
            // → 4 bytes, so 2 per unit).
            utf8Bytes = 0;
            for (const QChar ch : frame) {
                const ushort u = ch.unicode();
                utf8Bytes += u < 0x80 ? 1 : u < 0x800 ? 2 : ch.isSurrogate() ? 2 : 3;
            }
        }
        if (utf8Bytes > _maxResponseBytes) {
            _socket->sendTextMessage(GatewayProtocol::encodeError(id, tooLarge));
            return;
        }
        _socket->sendTextMessage(frame);
    }

    void GatewaySession::onTextMessage(const QString &frame)
    {
        if (_closing)
            return; // close handshake is already in flight (8.6 review L8)
        // A peer that keeps sending requests but does not read the replies
        // would grow the write queue without bound (review R7). The real
        // client reads each whole reply before its next request, so it never
        // trips this. Aborting (queued — we are inside the socket's own
        // signal) emits disconnected() at once, freeing the slot.
        if (_socket->bytesToWrite() > GatewayProtocol::kMaxPendingWriteBytes) {
            qWarning() << "ragoshell-gateway: peer is not reading replies; aborting";
            _closing = true;
            QTimer::singleShot(0, _socket, &QWebSocket::abort);
            return;
        }
        // Hosted: the entitlement ends at exp — refuse further work even if
        // the expiry timer has not fired yet (review R8).
        if (_authed && _expiresAt > 0
            && QDateTime::currentSecsSinceEpoch() >= _expiresAt) {
            failAndClose(QStringLiteral("entitlement expired"));
            return;
        }
        _idle.start(_idleMs); // any frame renews the idle lease
        if (frame.toUtf8().size() > GatewayProtocol::kMaxFrameBytes) {
            failAndClose(QStringLiteral("frame too large"));
            return;
        }

        quint32 id = 0;
        QString op, error;
        QJsonObject params;
        if (!GatewayProtocol::decodeRequest(frame, &id, &op, &params, &error)) {
            failAndClose(error);
            return;
        }

        if (!_authed) {
            if (op != QLatin1String(GatewayProtocol::Op::Hello)) {
                failAndClose(QStringLiteral("unauthorized"));
                return;
            }
            const QString token =
                params.value(QStringLiteral("token")).toString();
            if (_hosted.verifier) {
                // Hosted mode (docs/HostedGateway.md §2): the token must be
                // a ValidOnline entitlement+jwt — offline grace exists for
                // offline client features, not a hosted network service.
                EntitlementVerifyOptions opts;
                if (_hosted.options)
                    opts = *_hosted.options;
                const EntitlementVerifyResult res = _hosted.verifier->verify(
                    token, QDateTime::currentSecsSinceEpoch(), opts);
                QString key;
                if (res.state == EntitlementState::ValidOnline
                    && res.claims.features.hostedBridge
                    && (res.claims.status == EntitlementStatus::Active
                        || res.claims.status == EntitlementStatus::Trialing))
                    key = !res.claims.deviceId.isEmpty()
                        ? res.claims.deviceId : res.claims.subject;
                if (key.isEmpty()
                    || (_hosted.ledger && !_hosted.ledger->tryAcquire(key))) {
                    failAndClose(QStringLiteral("unauthorized"));
                    return;
                }
                _authKey = key;
                // The entitlement is a lease, not a login: the session ends
                // at exp even while frames keep it from going idle
                // (2026-11-04 review R8). exp - now is at most the 7-day
                // token lifetime plus skew, well inside int milliseconds.
                _expiresAt = res.claims.expiresAt;
                const qint64 leftMs =
                    (_expiresAt - QDateTime::currentSecsSinceEpoch()) * 1000;
                _expiry.start(static_cast<int>(
                    qBound(qint64(0), leftMs,
                           qint64(std::numeric_limits<int>::max()))));
            } else if (!tokenEquals(token, _expectedToken)) {
                failAndClose(QStringLiteral("unauthorized"));
                return;
            }
            _authed = true;
            _socket->sendTextMessage(GatewayProtocol::encodeOk(id, QStringLiteral("{}")));
            return;
        }

        if (op == QLatin1String(GatewayProtocol::Op::Connect)) {
            // SSRF gate (docs/WebGateway.md §2, G1): every host in the
            // connect URI must resolve publicly before a transport exists.
            // The URI is extracted with bsoncxx — the SAME parser
            // NativeMongoTransport runs on this payload (libbson keeps the
            // FIRST duplicate key, QJsonDocument the last) — so the checked
            // URI can never disagree with the dialed one.
            QString settings = params.value(QStringLiteral("settings")).toString();
            if (_egress) {
                QString uri;
                try {
                    const auto doc = bsoncxx::from_json(settings.toStdString());
                    const auto el = doc[std::string("uri")];
                    if (el && el.type() == bsoncxx::type::k_string)
                        uri = QString::fromStdString(
                            std::string(el.get_string().value));
                } catch (const std::exception &) {
                    // uri stays empty → the check below fails closed.
                }
                QString reason, clientReason, dialUri;
                if (!_egress->prepareMongoUri(uri, &dialUri, &reason, &clientReason)) {
                    // Policy refusals (TLS, +srv, options, host count) reach
                    // the client verbatim; resolved IPs and internal DNS
                    // detail stay in the server log — they are a
                    // private-namespace enumeration oracle otherwise.
                    qWarning() << "ragoshell-gateway: egress denied:"
                               << qPrintable(reason);
                    _socket->sendTextMessage(GatewayProtocol::encodeError(
                        id, clientReason.isEmpty() ? QStringLiteral("Egress denied.")
                                                   : clientReason));
                    return;
                }
                // The driver dials the checked, rewritten URI (forced
                // directConnection, clamped timeouts — review R1/R3), never
                // the client's original payload.
                settings = QString::fromUtf8(
                    QJsonDocument(QJsonObject{{QStringLiteral("uri"), dialUri}})
                        .toJson(QJsonDocument::Compact));
            }
            if (!_transport && _factory)
                _transport = _factory();
            if (!_transport) {
                _socket->sendTextMessage(
                    GatewayProtocol::encodeError(id, QStringLiteral("no transport")));
                return;
            }
            const IMongoTransport::Result r = _transport->connect(settings);
            sendResult(id, r.ok, r.ejson, r.error);
            return;
        }

        if (!_transport) {
            _socket->sendTextMessage(
                GatewayProtocol::encodeError(id, QStringLiteral("not connected")));
            return;
        }

        IMongoTransport::Result r;
        bool have = true;
        const QString db = params.value(QStringLiteral("database")).toString();
        if (op == QLatin1String(GatewayProtocol::Op::RunCommand))
            r = _transport->runCommand(db, params.value(QStringLiteral("command")).toString());
        else if (op == QLatin1String(GatewayProtocol::Op::ListDatabases))
            r = _transport->listDatabases();
        else if (op == QLatin1String(GatewayProtocol::Op::ListCollections))
            r = _transport->listCollections(db);
        else if (op == QLatin1String(GatewayProtocol::Op::Ping))
            r = _transport->ping();
        else if (op == QLatin1String(GatewayProtocol::Op::KillOp))
            r = _transport->killOp(params.value(QStringLiteral("filter")).toString());
        else if (op == QLatin1String(GatewayProtocol::Op::Disconnect)) {
            _transport->disconnect();
            r.ok = true;
            r.ejson = QStringLiteral("{}");
        } else if (op == QLatin1String(GatewayProtocol::Op::IsLive)) {
            r.ok = _transport->isLive();
            r.ejson = QStringLiteral("{\"live\":%1}").arg(r.ok ? QStringLiteral("true")
                                                              : QStringLiteral("false"));
        } else {
            have = false;
        }

        if (!have)
            _socket->sendTextMessage(
                GatewayProtocol::encodeError(id, QStringLiteral("unknown op")));
        else
            sendResult(id, r.ok, r.ejson, r.error);
    }

    void GatewaySession::onDisconnected()
    {
        if (!_authKey.isEmpty() && _hosted.ledger) {
            _hosted.ledger->release(_authKey);
            _authKey.clear();
        }
        if (_transport)
            _transport->disconnect();
        emit finished();
    }
}
