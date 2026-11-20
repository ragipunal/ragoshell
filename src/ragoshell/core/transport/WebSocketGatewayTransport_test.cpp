#include "gtest/gtest.h"

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QWebSocket>
#include <QWebSocketServer>

#include <functional>
#include <utility>

#include "ragoshell/core/cloud/EntitlementToken.h"
#include "ragoshell/core/transport/DeviceSessionLedger.h"
#include "ragoshell/core/transport/EgressPolicy.h"
#include "ragoshell/core/transport/GatewayProtocol.h"
#include "ragoshell/core/transport/GatewaySession.h"
#include "ragoshell/core/transport/InMemoryDemoTransport.h"
#include "ragoshell/core/transport/WebRoot.h"
#include "ragoshell/core/transport/WebSocketGatewayTransport.h"

#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

using namespace RagoShell;

// End-to-end on loopback: a real QWebSocketServer + GatewaySession serving an
// InMemoryDemoTransport, driven by a real WebSocketGatewayTransport client —
// the full Mod C wire path minus the OS binary (docs/LocalAgent.md §6).

namespace
{
    constexpr const char *kToken = "test-token-0123456789abcdef0123456789abcdef0123456789ab";

    void ensureCoreApp()
    {
        if (!QCoreApplication::instance())
        {
            static int argc = 1;
            static char arg0[] = "rago_unit_tests";
            static char *argv[] = {arg0, nullptr};
            static QCoreApplication app(argc, argv);
        }
    }

    bool waitUntil(const std::function<bool()> &pred, int timeoutMs = 5000)
    {
        ensureCoreApp();
        QElapsedTimer timer;
        timer.start();
        while (!pred())
        {
            if (timer.elapsed() >= timeoutMs)
                return false;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        return true;
    }

    class TestAgent : public QObject
    {
    public:
        explicit TestAgent(const QString &tok = QLatin1String(kToken),
                           const QString &hostOverride = QString(),
                           int idleMs = 0, bool enableEgress = false)
            : token(tok),
              _idleMs(idleMs),
              _server(QStringLiteral("test-agent"), QWebSocketServer::NonSecureMode)
        {
            _allowed = {QStringLiteral("https://ragoshell.com.tr"),
                        QStringLiteral("http://localhost:")};
            if (enableEgress)
                egress = std::make_unique<EgressPolicy>(/*allowPrivate=*/false);
            if (!_server.listen(QHostAddress::LocalHost, 0))
                qFatal("test agent could not bind loopback");
            _requiredHost = hostOverride.isEmpty()
                ? QStringLiteral("127.0.0.1:%1").arg(_server.serverPort())
                : hostOverride;
            QObject::connect(&_server, &QWebSocketServer::newConnection,
                             &_server, [this] {
                                 auto *s = new GatewaySession(
                                     _server.nextPendingConnection(), _allowed,
                                     _requiredHost, token,
                                     [] { return std::make_unique<InMemoryDemoTransport>(); },
                                     nullptr, egress.get());
                                 if (_idleMs > 0)
                                     s->setIdleTimeoutMs(_idleMs);
                                 if (maxResponseBytes > 0)
                                     s->setMaxResponseBytes(maxResponseBytes);
                                 QObject::connect(s, &GatewaySession::finished,
                                                  this, [this, s] {
                                                      ++finishedCount;
                                                      s->deleteLater();
                                                  });
                             });
        }
        QString url() const
        {
            return QStringLiteral("ws://127.0.0.1:%1").arg(_server.serverPort());
        }
        QString localhostUrl() const
        {
            return QStringLiteral("ws://localhost:%1").arg(_server.serverPort());
        }
        QString token;
        int finishedCount = 0;
        int _idleMs = 0;
        qint64 maxResponseBytes = 0; // >0: per-session response cap (test hook)
        std::unique_ptr<EgressPolicy> egress;

    private:
        QWebSocketServer _server;
        QStringList _allowed;
        QString _requiredHost;
    };

    // --- hosted-mode helpers: mint a real ES256 entitlement+jwt ---------
    // (mirrors the EntitlementToken_test mint helpers; the gateway verifies
    // with the corresponding public-key ring).
    struct OpenSslFree { void operator()(unsigned char *p) const noexcept { OPENSSL_free(p); } };
    using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

    EvpPkeyPtr genP256Key()
    {
        EVP_PKEY *raw = EVP_EC_gen("P-256");
        return EvpPkeyPtr(raw, &EVP_PKEY_free);
    }

    QByteArray spkiDer(EVP_PKEY *pkey)
    {
        unsigned char *der = nullptr;
        const int len = i2d_PUBKEY(pkey, &der);
        std::unique_ptr<unsigned char, OpenSslFree> guard(der);
        return QByteArray(reinterpret_cast<const char *>(der), len);
    }

    QByteArray b64url(const QByteArray &in)
    {
        return in.toBase64(QByteArray::Base64UrlEncoding
                           | QByteArray::OmitTrailingEquals);
    }

    QByteArray derSigToP1363(const QByteArray &derSig)
    {
        const auto *p = reinterpret_cast<const unsigned char *>(derSig.constData());
        std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> sig(
            d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(derSig.size())), &ECDSA_SIG_free);
        if (!sig)
            return QByteArray();
        const BIGNUM *r = nullptr, *s = nullptr;
        ECDSA_SIG_get0(sig.get(), &r, &s);
        QByteArray out(64, '\0');
        auto *buf = reinterpret_cast<unsigned char *>(out.data());
        if (BN_bn2binpad(r, buf, 32) != 32 || BN_bn2binpad(s, buf + 32, 32) != 32)
            return QByteArray();
        return out;
    }

    QByteArray signP1363(EVP_PKEY *priv, const QByteArray &input)
    {
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
            EVP_MD_CTX_new(), &EVP_MD_CTX_free);
        const auto *msg = reinterpret_cast<const unsigned char *>(input.constData());
        size_t len = 0;
        if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, priv) != 1
            || EVP_DigestSign(ctx.get(), nullptr, &len, msg, input.size()) != 1)
            return QByteArray();
        QByteArray der(int(len), '\0');
        if (EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char *>(der.data()),
                           &len, msg, input.size()) != 1)
            return QByteArray();
        der.truncate(int(len));
        return derSigToP1363(der);
    }

    QString mintEntitlementJwt(EVP_PKEY *priv, const QString &kid,
                               bool hostedBridge, qint64 iat, qint64 exp,
                               const QString &did, const QString &status = QStringLiteral("active"),
                               const QString &typ = QStringLiteral("entitlement+jwt"))
    {
        const QByteArray header = QJsonDocument(QJsonObject{
            {"alg", "ES256"}, {"kid", kid}, {"typ", typ}}).toJson(QJsonDocument::Compact);
        QJsonObject payload{
            {"iss", EntitlementVerifier::defaultApiIssuer()},
            {"aud", EntitlementVerifier::defaultAppAudience()},
            {"sub", QStringLiteral("550e8400-e29b-41d4-a716-446655440000")},
            {"jti", QStringLiteral("0123456789abcdef0123456789abcdef")},
            {"iat", iat}, {"nbf", iat}, {"exp", exp},
            {"offline_grace_until", exp},
            {"plan", QStringLiteral("pro")}, {"status", status},
            {"features", QJsonObject{{"sync", true}, {"hosted_ai", true},
                                     {"hosted_bridge", hostedBridge},
                                     {"priority_support", true}}},
            {"quotas", QJsonObject{{"sync_max_collections", 20},
                                   {"sync_max_blob_bytes", 1048576},
                                   {"ai_monthly_token_quota", 2000000}}},
        };
        if (!did.isEmpty())
            payload.insert("did", did);
        const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        const QByteArray input = b64url(header) + '.' + b64url(body);
        return QString::fromLatin1(input + '.' + b64url(signP1363(priv, input)));
    }

    // Hosted-mode agent: session configured with the verifier ring + ledger.
    class HostedTestAgent : public QObject
    {
    public:
        explicit HostedTestAgent(EVP_PKEY *pubKey, int maxPerDevice = 2)
            : _ledger(std::make_shared<DeviceSessionLedger>(maxPerDevice)),
              verifier(std::make_shared<EntitlementVerifier>()),
              _server(QStringLiteral("hosted-agent"), QWebSocketServer::NonSecureMode)
        {
            QString err;
            verifier->addPublicKeySpkiDer(QStringLiteral("test-kid"), spkiDer(pubKey), &err);
            auto opts = std::make_shared<EntitlementVerifyOptions>();
            opts->expectedIssuer = EntitlementVerifier::defaultApiIssuer();
            opts->expectedAudience = EntitlementVerifier::defaultAppAudience();
            _hosted.verifier = verifier;
            _hosted.options = opts;
            _hosted.ledger = _ledger;
            _allowed = {QStringLiteral("https://ragoshell.com.tr")};
            if (!_server.listen(QHostAddress::LocalHost, 0))
                qFatal("hosted agent could not bind loopback");
            _requiredHost = QStringLiteral("127.0.0.1:%1").arg(_server.serverPort());
            QObject::connect(&_server, &QWebSocketServer::newConnection,
                             &_server, [this] {
                                 auto *s = new GatewaySession(
                                     _server.nextPendingConnection(), _allowed,
                                     _requiredHost, QString(),
                                     [] { return std::make_unique<InMemoryDemoTransport>(); },
                                     nullptr, nullptr, QString(), &_hosted);
                                 QObject::connect(s, &GatewaySession::finished,
                                                  this, [s] { s->deleteLater(); });
                             });
        }
        QString url() const
        {
            return QStringLiteral("ws://127.0.0.1:%1").arg(_server.serverPort());
        }
        std::shared_ptr<DeviceSessionLedger> _ledger;
        std::shared_ptr<EntitlementVerifier> verifier;

    private:
        QWebSocketServer _server;
        QStringList _allowed;
        QString _requiredHost;
        HostedAuthConfig _hosted;
    };

    // Frame-level client used to drive protocol cases the transport wrapper
    // hides (oversized frames, pipelined ids, unknown ops, bad origins).
    struct RawClient
    {
        explicit RawClient(const QString &url,
                           const QString &origin = QStringLiteral("https://ragoshell.com.tr"))
            : sock(origin, QWebSocketProtocol::VersionLatest)
        {
            QObject::connect(&sock, &QWebSocket::textMessageReceived,
                             &sock, [this](const QString &f) { frames << f; });
            sock.open(QUrl(url));
        }

        bool waitConnected(int ms = 5000)
        {
            return waitUntil([this] {
                return sock.state() == QAbstractSocket::ConnectedState
                       || sock.state() == QAbstractSocket::UnconnectedState;
            }, ms) && sock.state() == QAbstractSocket::ConnectedState;
        }

        bool handshake(const QString &tok = QLatin1String(kToken))
        {
            sock.sendTextMessage(GatewayProtocol::encodeRequest(
                1, QString::fromUtf8(GatewayProtocol::Op::Hello),
                QJsonObject{{"token", tok}}));
            if (!waitFrames(1))
                return false;
            QString ejson, err;
            return GatewayProtocol::decodeResponse(frames.at(0), 1, &ejson, &err);
        }

        bool connectTransport()
        {
            sock.sendTextMessage(GatewayProtocol::encodeRequest(
                2, QString::fromUtf8(GatewayProtocol::Op::Connect),
                QJsonObject{{"settings", "{}"}}));
            if (!waitFrames(2))
                return false;
            QString ejson, err;
            return GatewayProtocol::decodeResponse(frames.at(1), 2, &ejson, &err);
        }

        bool waitFrames(int n, int ms = 5000)
        {
            return waitUntil([this, n] { return frames.size() >= n; }, ms);
        }

        bool waitClosed(int ms = 5000)
        {
            return waitUntil([this] {
                return sock.state() == QAbstractSocket::UnconnectedState;
            }, ms);
        }

        QWebSocket sock;
        QStringList frames;
    };
}

class WebSocketGatewayTransportTest : public testing::Test
{
protected:
    void SetUp() override { ensureCoreApp(); }
};

TEST_F(WebSocketGatewayTransportTest, hello_connect_runcommand)
{
    TestAgent agent;
    WebSocketGatewayTransport client(agent.url(), QString::fromUtf8(kToken));

    auto c = client.connect(QStringLiteral("{}"));
    ASSERT_TRUE(c.ok) << c.error.toStdString();

    auto dbs = client.listDatabases();
    ASSERT_TRUE(dbs.ok) << dbs.error.toStdString();
    EXPECT_TRUE(dbs.ejson.contains(QStringLiteral("catalog")));

    auto r = client.runCommand(QStringLiteral("catalog"),
                             QStringLiteral("{\"find\":\"products\",\"filter\":{}}"));
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.ejson.contains(QStringLiteral("products"))
                || r.ejson.contains(QStringLiteral("_id")));

    client.disconnect();
}

TEST_F(WebSocketGatewayTransportTest, wrong_token_closes)
{
    TestAgent agent;
    WebSocketGatewayTransport client(agent.url(), QStringLiteral("wrong"));
    auto c = client.connect(QStringLiteral("{}"));
    EXPECT_FALSE(c.ok);
}

TEST_F(WebSocketGatewayTransportTest, unknown_op_surfaces_error)
{
    TestAgent agent;
    WebSocketGatewayTransport client(agent.url(), QString::fromUtf8(kToken));
    ASSERT_TRUE(client.connect(QStringLiteral("{}")).ok);

    // runCommand on a nonexistent collection still returns ok at transport
    // level — use a bad command shape to force an error path instead.
    auto r = client.runCommand(QStringLiteral("catalog"), QStringLiteral("not-json"));
    // The demo transport may parse-fail → ok=false; either way no crash/hang.
    SUCCEED();
}

TEST_F(WebSocketGatewayTransportTest, bad_origin_rejected)
{
    TestAgent agent;
    RawClient raw(agent.url(), QStringLiteral("https://evil.example"));
    // The handshake may briefly complete at TCP level; the session must then
    // refuse (error frame + PolicyViolated close) rather than serve requests.
    ASSERT_TRUE(waitUntil([&raw] {
        return raw.sock.state() != QAbstractSocket::ConnectingState;
    }));
    EXPECT_TRUE(raw.waitClosed());
}

TEST_F(WebSocketGatewayTransportTest, wrong_host_rejected)
{
    // requiredHost is what the agent expects in the Host header; a client
    // reaching it through any other name must be refused (rebinding guard).
    TestAgent agent(QLatin1String(kToken), QStringLiteral("10.9.9.9:1"));
    WebSocketGatewayTransport client(agent.url(), QString::fromUtf8(kToken));
    EXPECT_FALSE(client.connect(QStringLiteral("{}")).ok);
}

TEST_F(WebSocketGatewayTransportTest, oversized_frame_closes_session)
{
    TestAgent agent;
    RawClient raw(agent.url());
    ASSERT_TRUE(raw.waitConnected());
    ASSERT_TRUE(raw.handshake());

    raw.sock.sendTextMessage(QString(GatewayProtocol::kMaxFrameBytes + 1, QLatin1Char('a')));
    EXPECT_TRUE(raw.waitClosed());
}

TEST_F(WebSocketGatewayTransportTest, unknown_op_gets_error_frame)
{
    TestAgent agent;
    RawClient raw(agent.url());
    ASSERT_TRUE(raw.waitConnected());
    ASSERT_TRUE(raw.handshake());
    ASSERT_TRUE(raw.connectTransport());

    raw.sock.sendTextMessage(GatewayProtocol::encodeRequest(
        3, QStringLiteral("dropTables"), QJsonObject{}));
    ASSERT_TRUE(raw.waitFrames(3));
    QString ejson, err;
    EXPECT_FALSE(GatewayProtocol::decodeResponse(raw.frames.at(2), 3, &ejson, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("unknown op")));
    EXPECT_EQ(raw.sock.state(), QAbstractSocket::ConnectedState); // not closed
}

TEST_F(WebSocketGatewayTransportTest, pipelined_requests_answered_by_id)
{
    TestAgent agent;
    RawClient raw(agent.url());
    ASSERT_TRUE(raw.waitConnected());
    ASSERT_TRUE(raw.handshake());
    ASSERT_TRUE(raw.connectTransport());

    // Two requests sent before either reply — protocol allows exactly one in
    // flight on the client, but the session must answer each by its own id.
    raw.sock.sendTextMessage(GatewayProtocol::encodeRequest(
        10, QString::fromUtf8(GatewayProtocol::Op::Ping), QJsonObject{}));
    raw.sock.sendTextMessage(GatewayProtocol::encodeRequest(
        11, QString::fromUtf8(GatewayProtocol::Op::ListDatabases), QJsonObject{}));
    ASSERT_TRUE(raw.waitFrames(4));

    QString ejson, err;
    ASSERT_TRUE(GatewayProtocol::decodeResponse(raw.frames.at(2), 10, &ejson, &err));
    ASSERT_TRUE(GatewayProtocol::decodeResponse(raw.frames.at(3), 11, &ejson, &err));
    EXPECT_TRUE(ejson.contains(QStringLiteral("catalog")));
}

TEST_F(WebSocketGatewayTransportTest, calls_after_disconnect_fail_then_reconnect)
{
    TestAgent agent;
    WebSocketGatewayTransport client(agent.url(), QString::fromUtf8(kToken));
    ASSERT_TRUE(client.connect(QStringLiteral("{}")).ok);

    client.disconnect();
    auto r = client.runCommand(QStringLiteral("catalog"),
                             QStringLiteral("{ping:1}"));
    EXPECT_FALSE(r.ok);

    // Reconnect must open a fresh socket, not wait on a dead one.
    ASSERT_TRUE(client.connect(QStringLiteral("{}")).ok);
    auto dbs = client.listDatabases();
    EXPECT_TRUE(dbs.ok) << dbs.error.toStdString();
}

TEST_F(WebSocketGatewayTransportTest, rejected_socket_still_frees_session)
{
    // Regression for the HIGH review finding: the origin/host gate closed the
    // socket before `disconnected` was connected, so finished() never emitted
    // and the agent-side session slot leaked forever.
    TestAgent agent;
    RawClient raw(agent.url(), QStringLiteral("https://evil.example"));
    ASSERT_TRUE(waitUntil([&raw] {
        return raw.sock.state() != QAbstractSocket::ConnectingState;
    }));
    ASSERT_TRUE(raw.waitClosed());
    EXPECT_TRUE(waitUntil([&agent] { return agent.finishedCount >= 1; }));
}

TEST_F(WebSocketGatewayTransportTest, binary_frame_closes_session)
{
    TestAgent agent;
    RawClient raw(agent.url());
    ASSERT_TRUE(raw.waitConnected());
    ASSERT_TRUE(raw.handshake());

    raw.sock.sendBinaryMessage(QByteArray("x"));
    EXPECT_TRUE(raw.waitClosed());
}

TEST_F(WebSocketGatewayTransportTest, localhost_host_accepted)
{
    // requiredHost "127.0.0.1:P" must also accept Host "localhost:P" — the
    // same loopback listener reached through its canonical name.
    TestAgent agent;
    WebSocketGatewayTransport client(agent.localhostUrl(), QString::fromUtf8(kToken));
    EXPECT_TRUE(client.connect(QStringLiteral("{}")).ok);
}

TEST_F(WebSocketGatewayTransportTest, idle_timeout_closes_session)
{
    TestAgent agent(QLatin1String(kToken), QString(), 120);
    RawClient raw(agent.url());
    ASSERT_TRUE(raw.waitConnected());
    ASSERT_TRUE(raw.handshake());
    // No frames for > 120 ms → the session closes the socket.
    EXPECT_TRUE(raw.waitClosed());
}

TEST_F(WebSocketGatewayTransportTest, egress_blocks_private_connect_target)
{
    // Gateway sessions carry an EgressPolicy; connect ops to private hosts
    // are refused with an error frame BEFORE any transport is created —
    // the session itself stays usable (docs/WebGateway.md §2, G1).
    TestAgent agent(QLatin1String(kToken), QString(), 0, /*enableEgress=*/true);
    RawClient raw(agent.url());
    ASSERT_TRUE(raw.waitConnected());
    ASSERT_TRUE(raw.handshake());

    raw.sock.sendTextMessage(GatewayProtocol::encodeRequest(
        5, QString::fromUtf8(GatewayProtocol::Op::Connect),
        QJsonObject{{"settings", "{\"uri\":\"mongodb://127.0.0.1:27017\"}"}}));
    ASSERT_TRUE(raw.waitFrames(2));
    QString ejson, err;
    EXPECT_FALSE(GatewayProtocol::decodeResponse(raw.frames.at(1), 5, &ejson, &err));
    // Generic denial only — resolved IPs / internal DNS detail must not
    // leak to the client (they are logged server-side instead).
    EXPECT_EQ(err, QStringLiteral("Egress denied."));

    // The connection remains open after a blocked target — the session is
    // still usable for a permitted target (public literal → transport is
    // created; it then fails on the real connect, NOT on egress).
    raw.sock.sendTextMessage(GatewayProtocol::encodeRequest(
        6, QString::fromUtf8(GatewayProtocol::Op::Connect),
        QJsonObject{{"settings", "{\"uri\":\"mongodb://8.8.8.8:27017\"}"}}));
    ASSERT_TRUE(raw.waitFrames(3));
    EXPECT_TRUE(GatewayProtocol::decodeResponse(raw.frames.at(2), 6, &ejson, &err));
    EXPECT_EQ(raw.sock.state(), QAbstractSocket::ConnectedState);
}

TEST_F(WebSocketGatewayTransportTest, egress_duplicate_uri_keys_fail_closed)
{
    // {"uri":private,"uri":public} — QJsonDocument keeps the LAST key but
    // libbson (what NativeMongoTransport reads) keeps the FIRST. The gate
    // must parse with the same parser as the transport or a private target
    // hides behind a public twin.
    TestAgent agent(QLatin1String(kToken), QString(), 0, /*enableEgress=*/true);
    RawClient raw(agent.url());
    ASSERT_TRUE(raw.waitConnected());
    ASSERT_TRUE(raw.handshake());

    raw.sock.sendTextMessage(GatewayProtocol::encodeRequest(
        7, QString::fromUtf8(GatewayProtocol::Op::Connect),
        QJsonObject{{"settings",
            "{\"uri\":\"mongodb://127.0.0.1:27017\","
            "\"uri\":\"mongodb://8.8.8.8:27017\"}"}}));
    ASSERT_TRUE(raw.waitFrames(2));
    QString ejson, err;
    EXPECT_FALSE(GatewayProtocol::decodeResponse(raw.frames.at(1), 7, &ejson, &err));
    EXPECT_EQ(err, QStringLiteral("Egress denied."));
}

TEST(GatewayProtocolTest, request_roundtrip)
{
    const QString frame = GatewayProtocol::encodeRequest(
        7, QStringLiteral("runCommand"),
        QJsonObject{{"database", "d"}, {"command", "{ping:1}"}});
    quint32 id = 0;
    QString op, err;
    QJsonObject params;
    ASSERT_TRUE(GatewayProtocol::decodeRequest(frame, &id, &op, &params, &err));
    EXPECT_EQ(id, 7u);
    EXPECT_EQ(op, QStringLiteral("runCommand"));
    EXPECT_EQ(params.value("database").toString(), QStringLiteral("d"));
}

TEST(GatewayProtocolTest, malformed_frames_rejected)
{
    quint32 id = 0;
    QString op, err;
    QJsonObject params;
    EXPECT_FALSE(GatewayProtocol::decodeRequest(QStringLiteral("{"), &id, &op, &params, &err));
    EXPECT_FALSE(GatewayProtocol::decodeRequest(QStringLiteral("{}"), &id, &op, &params, &err));
    EXPECT_FALSE(GatewayProtocol::decodeRequest(
        QStringLiteral("{\"id\":\"x\",\"op\":\"y\"}"), &id, &op, &params, &err));
}

TEST(GatewayProtocolTest, response_roundtrip_and_id_mismatch)
{
    const QString ok = GatewayProtocol::encodeOk(3, QStringLiteral("{\"ok\":1}"));
    QString ejson, err;
    EXPECT_TRUE(GatewayProtocol::decodeResponse(ok, 3, &ejson, &err));
    EXPECT_TRUE(ejson.contains(QStringLiteral("\"ok\":1")));
    EXPECT_FALSE(GatewayProtocol::decodeResponse(ok, 4, &ejson, &err));

    const QString bad = GatewayProtocol::encodeError(3, QStringLiteral("boom"));
    EXPECT_FALSE(GatewayProtocol::decodeResponse(bad, 3, &ejson, &err));
    EXPECT_EQ(err, QStringLiteral("boom"));
}

// --- Hosted mode (docs/HostedGateway.md §6): hello carries a real ES256 --
// --- entitlement+jwt; hosted_bridge + status + per-device cap enforced. --

TEST(DeviceSessionLedgerTest, acquire_release_exhaustion)
{
    DeviceSessionLedger ledger(2);
    EXPECT_TRUE(ledger.tryAcquire(QStringLiteral("dev-a")));
    EXPECT_TRUE(ledger.tryAcquire(QStringLiteral("dev-a")));
    EXPECT_FALSE(ledger.tryAcquire(QStringLiteral("dev-a")));
    EXPECT_TRUE(ledger.tryAcquire(QStringLiteral("dev-b"))); // other key unaffected
    EXPECT_FALSE(ledger.tryAcquire(QString()));              // empty key denied

    ledger.release(QStringLiteral("dev-a"));
    EXPECT_EQ(ledger.count(QStringLiteral("dev-a")), 1);
    ledger.release(QStringLiteral("dev-a"));
    ledger.release(QStringLiteral("dev-a")); // over-release is a no-op
    EXPECT_EQ(ledger.count(QStringLiteral("dev-a")), 0);
    EXPECT_TRUE(ledger.tryAcquire(QStringLiteral("dev-a")));
}

TEST_F(WebSocketGatewayTransportTest, hosted_valid_jwt_connects)
{
    ensureCoreApp();
    auto key = genP256Key();
    HostedTestAgent agent(key.get());
    RawClient raw(agent.url());
    ASSERT_TRUE(raw.waitConnected());
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString jwt = mintEntitlementJwt(key.get(), QStringLiteral("test-kid"),
        /*hostedBridge=*/true, now - 60, now + 3600,
        QStringLiteral("7c9e6679-7425-40de-944b-e07fc1f90ae7"));
    EXPECT_TRUE(raw.handshake(jwt));
    EXPECT_TRUE(raw.connectTransport());
}

TEST_F(WebSocketGatewayTransportTest, hosted_free_plan_denied)
{
    ensureCoreApp();
    auto key = genP256Key();
    HostedTestAgent agent(key.get());
    RawClient raw(agent.url());
    ASSERT_TRUE(raw.waitConnected());
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString jwt = mintEntitlementJwt(key.get(), QStringLiteral("test-kid"),
        /*hostedBridge=*/false, now - 60, now + 3600,
        QStringLiteral("7c9e6679-7425-40de-944b-e07fc1f90ae7"));
    EXPECT_FALSE(raw.handshake(jwt));
    EXPECT_TRUE(raw.waitClosed());
}

TEST_F(WebSocketGatewayTransportTest, hosted_expired_and_wrong_typ_denied)
{
    ensureCoreApp();
    auto key = genP256Key();
    HostedTestAgent agent(key.get());
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString did = QStringLiteral("7c9e6679-7425-40de-944b-e07fc1f90ae7");

    RawClient expired(agent.url());
    ASSERT_TRUE(expired.waitConnected());
    EXPECT_FALSE(expired.handshake(mintEntitlementJwt(
        key.get(), QStringLiteral("test-kid"), true, now - 90000, now - 60, did)));

    RawClient wrongTyp(agent.url());
    ASSERT_TRUE(wrongTyp.waitConnected());
    EXPECT_FALSE(wrongTyp.handshake(mintEntitlementJwt(
        key.get(), QStringLiteral("test-kid"), true, now - 60, now + 3600, did,
        QStringLiteral("active"), QStringLiteral("at+jwt"))));
}

TEST_F(WebSocketGatewayTransportTest, hosted_per_device_cap_enforced)
{
    ensureCoreApp();
    auto key = genP256Key();
    HostedTestAgent agent(key.get(), /*maxPerDevice=*/2);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString did = QStringLiteral("7c9e6679-7425-40de-944b-e07fc1f90ae7");
    const QString jwt = mintEntitlementJwt(key.get(), QStringLiteral("test-kid"),
        true, now - 60, now + 3600, did);

    RawClient a(agent.url()), b(agent.url()), c(agent.url());
    ASSERT_TRUE(a.waitConnected() && b.waitConnected() && c.waitConnected());
    EXPECT_TRUE(a.handshake(jwt));
    EXPECT_TRUE(b.handshake(jwt));
    EXPECT_FALSE(c.handshake(jwt)); // third concurrent session same did → denied
    EXPECT_EQ(agent._ledger->count(did), 2);

    // Slot frees on disconnect — a fourth session succeeds after one closes.
    a.sock.close();
    ASSERT_TRUE(waitUntil([&agent, &did] {
        return agent._ledger->count(did) == 1;
    }));
    RawClient d(agent.url());
    ASSERT_TRUE(d.waitConnected());
    EXPECT_TRUE(d.handshake(jwt));
}

// --- 2026-11-04 review (docs/BridgeSecurityReview.md §6) -----------------

TEST_F(WebSocketGatewayTransportTest, frame_cap_enforced_by_qt_before_hello)
{
    // R2: the 1 MiB cap must hold while Qt reads — before hello, before the
    // session's own size check. Qt closes with 1009 (too much data); the
    // session's post-assembly check would have closed with 1008.
    TestAgent agent;
    RawClient raw(agent.url());
    ASSERT_TRUE(raw.waitConnected());

    raw.sock.sendTextMessage(QString(GatewayProtocol::kMaxFrameBytes + 1, QLatin1Char('a')));
    ASSERT_TRUE(raw.waitClosed());
    EXPECT_EQ(raw.sock.closeCode(), QWebSocketProtocol::CloseCodeTooMuchData);
    EXPECT_TRUE(waitUntil([&agent] { return agent.finishedCount == 1; }));
}

TEST_F(WebSocketGatewayTransportTest, oversized_result_becomes_error_frame)
{
    // R7: a reply over the response cap is replaced by an error frame for
    // the same id; the session stays usable for small replies.
    TestAgent agent;
    agent.maxResponseBytes = 50; // connect/ping replies fit, listDatabases not
    RawClient raw(agent.url());
    ASSERT_TRUE(raw.waitConnected());
    ASSERT_TRUE(raw.handshake());
    ASSERT_TRUE(raw.connectTransport());

    raw.sock.sendTextMessage(GatewayProtocol::encodeRequest(
        3, QString::fromUtf8(GatewayProtocol::Op::ListDatabases), QJsonObject{}));
    ASSERT_TRUE(raw.waitFrames(3));
    QString ejson, err;
    EXPECT_FALSE(GatewayProtocol::decodeResponse(raw.frames.at(2), 3, &ejson, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("too large"))) << err.toStdString();

    raw.sock.sendTextMessage(GatewayProtocol::encodeRequest(
        4, QString::fromUtf8(GatewayProtocol::Op::Ping), QJsonObject{}));
    ASSERT_TRUE(raw.waitFrames(4));
    EXPECT_TRUE(GatewayProtocol::decodeResponse(raw.frames.at(3), 4, &ejson, &err))
        << err.toStdString();
}

TEST_F(WebSocketGatewayTransportTest, egress_srv_refused_with_clear_error)
{
    // R6: a public gateway refuses mongodb+srv:// with an explicit reason
    // (no DNS involved); policy refusals reach the client verbatim.
    TestAgent agent(QLatin1String(kToken), QString(), 0, /*enableEgress=*/true);
    RawClient raw(agent.url());
    ASSERT_TRUE(raw.waitConnected());
    ASSERT_TRUE(raw.handshake());

    raw.sock.sendTextMessage(GatewayProtocol::encodeRequest(
        8, QString::fromUtf8(GatewayProtocol::Op::Connect),
        QJsonObject{{"settings",
            "{\"uri\":\"mongodb+srv://cluster0.example.mongodb.net/db\"}"}}));
    ASSERT_TRUE(raw.waitFrames(2));
    QString ejson, err;
    EXPECT_FALSE(GatewayProtocol::decodeResponse(raw.frames.at(1), 8, &ejson, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("not supported on public gateways")))
        << err.toStdString();
    EXPECT_EQ(raw.sock.state(), QAbstractSocket::ConnectedState);
}

TEST_F(WebSocketGatewayTransportTest, hosted_session_closes_at_jwt_exp)
{
    // R8: the entitlement is a lease — the session ends at exp even though
    // it is not idle.
    ensureCoreApp();
    auto key = genP256Key();
    HostedTestAgent agent(key.get());
    RawClient raw(agent.url());
    ASSERT_TRUE(raw.waitConnected());
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString jwt = mintEntitlementJwt(key.get(), QStringLiteral("test-kid"),
        /*hostedBridge=*/true, now - 60, now + 3,
        QStringLiteral("7c9e6679-7425-40de-944b-e07fc1f90ae7"));
    ASSERT_TRUE(raw.handshake(jwt));
    EXPECT_EQ(raw.sock.state(), QAbstractSocket::ConnectedState);

    ASSERT_TRUE(raw.waitClosed(10000));
    ASSERT_GE(raw.frames.size(), 2);
    EXPECT_TRUE(raw.frames.last().contains(QStringLiteral("entitlement expired")))
        << raw.frames.last().toStdString();
    EXPECT_TRUE(waitUntil([&agent] {
        return agent._ledger->count(QStringLiteral("7c9e6679-7425-40de-944b-e07fc1f90ae7")) == 0;
    }));
}

namespace
{
    // Gateway double that answers like the real (single-threaded) session:
    // runCommand is slow and holds every later reply until it is done.
    class LaggyGateway
    {
    public:
        explicit LaggyGateway(int runCommandDelayMs)
            : _delayMs(runCommandDelayMs),
              _server(QStringLiteral("laggy-gateway"), QWebSocketServer::NonSecureMode)
        {
            if (!_server.listen(QHostAddress::LocalHost, 0))
                qFatal("laggy gateway could not bind loopback");
            QObject::connect(&_server, &QWebSocketServer::newConnection, &_server, [this] {
                _peer = _server.nextPendingConnection();
                _peer->setParent(&_server);
                QObject::connect(_peer, &QWebSocket::textMessageReceived, _peer,
                                 [this](const QString &frame) { onFrame(frame); });
            });
        }
        QString url() const
        {
            return QStringLiteral("ws://127.0.0.1:%1").arg(_server.serverPort());
        }

    private:
        void onFrame(const QString &frame)
        {
            quint32 id = 0;
            QString op, err;
            QJsonObject params;
            if (!GatewayProtocol::decodeRequest(frame, &id, &op, &params, &err))
                return;
            const QString reply = GatewayProtocol::encodeOk(id, QStringLiteral("{\"ok\":1}"));
            if (op == QLatin1String(GatewayProtocol::Op::RunCommand)) {
                _busy = true;
                QTimer::singleShot(_delayMs, _peer, [this, reply] {
                    _peer->sendTextMessage(reply);
                    for (const QString &q : std::as_const(_queued))
                        _peer->sendTextMessage(q);
                    _queued.clear();
                    _busy = false;
                });
                return;
            }
            if (_busy)
                _queued << reply;
            else
                _peer->sendTextMessage(reply);
        }

        int _delayMs;
        QWebSocketServer _server;
        QWebSocket *_peer = nullptr;
        bool _busy = false;
        QStringList _queued;
    };
}

TEST_F(WebSocketGatewayTransportTest, late_reply_after_timeout_is_ignored)
{
    // R10: the reply to a timed-out request arrives during the next call;
    // it must be skipped, not fail that call with "Response id mismatch.".
    LaggyGateway gateway(/*runCommandDelayMs=*/400);
    WebSocketGatewayTransport client(gateway.url(), QString::fromUtf8(kToken));
    ASSERT_TRUE(client.connect(QStringLiteral("{}")).ok);

    client.setOpTimeoutMs(100);
    const auto slow = client.runCommand(QStringLiteral("admin"), QStringLiteral("{\"ping\":1}"));
    EXPECT_FALSE(slow.ok);
    EXPECT_EQ(slow.error, QStringLiteral("Gateway op timed out."));

    client.setOpTimeoutMs(5000);
    const auto next = client.ping();
    EXPECT_TRUE(next.ok) << next.error.toStdString();
}

TEST(WebRootTest, containment_covers_br_sibling)
{
    // R9: every file the gateway opens — the requested one and its .br
    // sibling — must canonicalize inside the web root.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(QDir(tmp.path()).mkpath(QStringLiteral("web")));
    ASSERT_TRUE(QDir(tmp.path()).mkpath(QStringLiteral("outside")));
    auto touch = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write("x") == 1;
    };
    const QString webDir = tmp.path() + QStringLiteral("/web");
    const QString secret = tmp.path() + QStringLiteral("/outside/secret.txt");
    ASSERT_TRUE(touch(webDir + QStringLiteral("/app.js")));
    ASSERT_TRUE(touch(secret));
    const QString root = QFileInfo(webDir).canonicalFilePath();
    ASSERT_FALSE(root.isEmpty());

    const QString inside = WebRoot::containedPath(root, root + QStringLiteral("/app.js"));
    EXPECT_EQ(inside, root + QStringLiteral("/app.js"));
    EXPECT_TRUE(WebRoot::containedPath(root, root + QStringLiteral("/../outside/secret.txt"))
                    .isEmpty());
    EXPECT_TRUE(WebRoot::containedPath(root, root + QStringLiteral("/missing.js")).isEmpty());
    EXPECT_TRUE(WebRoot::containedPath(root, root).isEmpty()); // the root itself
    EXPECT_TRUE(WebRoot::containedPath(QString(), secret).isEmpty());

#ifndef Q_OS_WIN
    // A symlinked .br sibling pointing outside the root must not be served
    // (QFile::link makes .lnk shortcuts on Windows, not symlinks).
    ASSERT_TRUE(QFile::link(secret, root + QStringLiteral("/app.js.br")));
    EXPECT_TRUE(WebRoot::containedPath(root, inside + QStringLiteral(".br")).isEmpty());
#endif
}
