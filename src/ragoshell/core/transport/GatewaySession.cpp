#include "ragoshell/core/transport/GatewaySession.h"

#include <QJsonDocument>
#include <QNetworkRequest>
#include <QTimer>
#include <QWebSocket>

#include "ragoshell/core/cloud/EntitlementToken.h"
#include "ragoshell/core/transport/BridgeQuota.h"
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
    namespace
    {
        // Client-facing close reasons of the free tier limits (§9).
        const QString kTransferLimit = QStringLiteral("monthly transfer limit reached");
        const QString kSessionLimit =
            QStringLiteral("session limit reached: one cloud bridge session per account");
        const QString kSessionTime = QStringLiteral("session time limit reached");
        const QString kAccountDisabled =
            QStringLiteral("cloud bridge is disabled for this account");
        const QString kUnavailable =
            QStringLiteral("cloud bridge is temporarily unavailable; try again later");

        // UTF-8 length without encoding a copy: surrogate pairs are 2 UTF-16
        // units → 4 bytes, so 2 per unit.
        qint64 utf8Length(const QString &s)
        {
            qint64 n = 0;
            for (const QChar ch : s) {
                const ushort u = ch.unicode();
                n += u < 0x80 ? 1 : u < 0x800 ? 2 : ch.isSurrogate() ? 2 : 3;
            }
            return n;
        }
    }

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
        if (_hosted.idleMs > 0)
            _idleMs = _hosted.idleMs;

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
        // A coarse timer may fire up to 5% early (6 minutes of a 2 h lease).
        _expiry.setTimerType(Qt::PreciseTimer);
        connect(&_expiry, &QTimer::timeout, this, [this] {
            if (!leaseEnded()) // fired early (timer coarseness): re-check later
                _expiry.start(1000);
        });

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

    bool GatewaySession::sendFrame(const QString &frame)
    {
        if (_authed && _hosted.quota
            && !_hosted.quota->consume(_account, utf8Length(frame),
                                       QDateTime::currentSecsSinceEpoch())) {
            failAndClose(kTransferLimit);
            return false;
        }
        _socket->sendTextMessage(frame);
        return true;
    }

    bool GatewaySession::leaseEnded()
    {
        if (!_authed)
            return false;
        if (_expiresAt > 0 && QDateTime::currentSecsSinceEpoch() >= _expiresAt) {
            failAndClose(QStringLiteral("entitlement expired"));
            return true;
        }
        if (_sessionDeadline.hasExpired()) {
            failAndClose(kSessionTime);
            return true;
        }
        return false;
    }

    bool GatewaySession::chargeDiscarded(qint64 bytes)
    {
        if (!_authed || !_hosted.quota)
            return true;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (_hosted.quota->consume(_account, bytes, now))
            return true;
        // Use up what is left, then stop: the next hello is refused anyway.
        _hosted.quota->consume(_account,
                               _hosted.quota->monthlyLimit() - _hosted.quota->used(_account, now),
                               now);
        failAndClose(kTransferLimit);
        return false;
    }

    void GatewaySession::sendResult(quint32 id, bool ok, const QString &ejson,
                                    const QString &error)
    {
        if (!ok) {
            sendFrame(GatewayProtocol::encodeError(id, error));
            return;
        }
        const QString tooLarge =
            QStringLiteral("Result too large for the gateway (over %1 bytes); "
                           "narrow the query or add a limit.").arg(_maxResponseBytes);
        // Every UTF-16 unit is at least one UTF-8 byte, so an oversize EJSON
        // string is refused before it is parsed into the envelope.
        // A discarded reply still counts with its size (UTF-16 units, a
        // lower bound of its UTF-8 bytes) — otherwise oversized queries would
        // pull unlimited data from MongoDB for the price of an error frame.
        if (ejson.size() > _maxResponseBytes) {
            if (chargeDiscarded(ejson.size()))
                sendFrame(GatewayProtocol::encodeError(id, tooLarge));
            return;
        }
        const QString frame = GatewayProtocol::encodeOk(id, ejson);
        const qint64 frameBytes = utf8Length(frame);
        if (frameBytes > _maxResponseBytes) {
            if (chargeDiscarded(frameBytes))
                sendFrame(GatewayProtocol::encodeError(id, tooLarge));
            return;
        }
        sendFrame(frame);
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
        // Hosted: the lease ends at exp or at the session time limit —
        // refuse further work even if the timer has not fired yet (review R8).
        if (leaseEnded())
            return;
        _idle.start(_idleMs); // any frame renews the idle lease
        const qint64 frameBytes = utf8Length(frame);
        if (frameBytes > GatewayProtocol::kMaxFrameBytes) {
            failAndClose(QStringLiteral("frame too large"));
            return;
        }
        if (_authed && _hosted.verifier) {
            // Free tier (§9.2/§9.4): a switched-off account stops at its
            // next frame; every request counts against the monthly quota.
            if (_hosted.deny && _hosted.deny->contains(_account)) {
                failAndClose(kAccountDisabled);
                return;
            }
            if (_hosted.quota
                && !_hosted.quota->consume(_account, frameBytes,
                                           QDateTime::currentSecsSinceEpoch())) {
                failAndClose(kTransferLimit);
                return;
            }
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
                // The account (sub, a verified canonical UUID) is the key of
                // every free tier limit (§9): one session per account, not
                // per device, so extra devices cannot multiply the quota.
                QString account;
                if (res.state == EntitlementState::ValidOnline
                    && res.claims.features.hostedBridge
                    && (res.claims.status == EntitlementStatus::Active
                        || res.claims.status == EntitlementStatus::Trialing))
                    account = res.claims.subject;
                if (account.isEmpty()) {
                    failAndClose(QStringLiteral("unauthorized"));
                    return;
                }
                const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
                if (_hosted.deny && _hosted.deny->contains(account)) {
                    failAndClose(kAccountDisabled);
                    return;
                }
                if (_hosted.quota && !_hosted.quota->healthy()) {
                    // The counter file keeps failing to save: a restart would
                    // lose this month's counts, so no new sessions (§9.3).
                    failAndClose(kUnavailable);
                    return;
                }
                if (_hosted.quota && _hosted.quota->exhausted(account, nowSec)) {
                    failAndClose(kTransferLimit);
                    return;
                }
                if (_hosted.ledger && !_hosted.ledger->tryAcquire(account)) {
                    failAndClose(kSessionLimit);
                    return;
                }
                _authKey = account;
                _account = account;
                // The entitlement is a lease, not a login: the session ends
                // at exp, or at the session time limit, even while frames
                // keep it from going idle (2026-11-04 review R8, §9.1).
                // exp - now is at most the 7-day token lifetime plus skew,
                // well inside int milliseconds.
                _expiresAt = res.claims.expiresAt;
                qint64 leftMs = (_expiresAt - nowSec) * 1000;
                if (_hosted.maxSessionMs > 0) {
                    _sessionDeadline = QDeadlineTimer(_hosted.maxSessionMs, Qt::PreciseTimer);
                    leftMs = qMin(leftMs, qint64(_hosted.maxSessionMs));
                }
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
                    sendFrame(GatewayProtocol::encodeError(
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
                sendFrame(GatewayProtocol::encodeError(id, QStringLiteral("no transport")));
                return;
            }
            const IMongoTransport::Result r = _transport->connect(settings);
            sendResult(id, r.ok, r.ejson, r.error);
            return;
        }

        if (!_transport) {
            sendFrame(GatewayProtocol::encodeError(id, QStringLiteral("not connected")));
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
            sendFrame(GatewayProtocol::encodeError(id, QStringLiteral("unknown op")));
        else
            sendResult(id, r.ok, r.ejson, r.error);
    }

    void GatewaySession::onDisconnected()
    {
        if (!_authKey.isEmpty() && _hosted.ledger) {
            _hosted.ledger->release(_authKey);
            _authKey.clear();
        }
        // Persist this session's transfer now rather than at the next
        // periodic flush (§9.3); a failed write is retried by the flush.
        if (_hosted.quota && _hosted.quota->isDirty()) {
            QString err;
            if (!_hosted.quota->save(&err))
                qWarning() << "ragoshell-gateway:" << qPrintable(err);
        }
        if (_transport)
            _transport->disconnect();
        emit finished();
    }
}
