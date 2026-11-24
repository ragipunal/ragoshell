#include "gtest/gtest.h"

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/cloud/CloudClient.h"
#include "ragoshell/core/cloud/EntitlementToken.h"
#include "ragoshell/core/settings/SettingsManager.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <functional>
#include <memory>

using namespace RagoShell;

namespace
{
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

struct OpenSslFree
{
    void operator()(unsigned char *p) const noexcept
    {
        if (p)
            OPENSSL_free(p);
    }
};

using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

EvpPkeyPtr makeP256Key()
{
    return EvpPkeyPtr(EVP_EC_gen("P-256"), &EVP_PKEY_free);
}

QByteArray exportSpkiDer(EVP_PKEY *pkey)
{
    unsigned char *der = nullptr;
    const int len = i2d_PUBKEY(pkey, &der);
    std::unique_ptr<unsigned char, OpenSslFree> guard(der);
    return (len > 0 && der) ? QByteArray(reinterpret_cast<const char *>(der), len) : QByteArray();
}

QByteArray b64Url(const QByteArray &raw)
{
    return raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QString signEntitlementJwt(EVP_PKEY *privKey,
                           const QString &kid,
                           const QString &issuer,
                           const QString &userId,
                           const QString &deviceId,
                           qint64 iat,
                           const QString &plan = QStringLiteral("pro"),
                           const QString &status = QStringLiteral("active"))
{
    const QJsonObject header{
        {QStringLiteral("alg"), QStringLiteral("ES256")},
        {QStringLiteral("kid"), kid},
        {QStringLiteral("typ"), QStringLiteral("entitlement+jwt")},
    };
    const QJsonObject features{
        {QStringLiteral("sync"), plan != QLatin1String("free")},
        {QStringLiteral("hosted_ai"), plan != QLatin1String("free")},
        {QStringLiteral("hosted_bridge"), plan != QLatin1String("free")},
        {QStringLiteral("priority_support"), plan != QLatin1String("free")},
    };
    const QJsonObject quotas{
        {QStringLiteral("sync_max_collections"), 25},
        {QStringLiteral("sync_max_blob_bytes"), 1048576},
        {QStringLiteral("ai_monthly_token_quota"), plan != QLatin1String("free") ? 2000000 : 0},
    };
    const QJsonObject payload{
        {QStringLiteral("iss"), issuer},
        {QStringLiteral("aud"), EntitlementVerifier::defaultAppAudience()},
        {QStringLiteral("sub"), userId},
        {QStringLiteral("did"), deviceId},
        {QStringLiteral("jti"), QStringLiteral("abcdef0123456789abcdef0123456789")},
        {QStringLiteral("iat"), iat},
        {QStringLiteral("nbf"), iat},
        {QStringLiteral("exp"), iat + EntitlementVerifier::kOnlineValiditySec},
        {QStringLiteral("offline_grace_until"), iat + EntitlementVerifier::kMaxOfflineGraceSec},
        {QStringLiteral("plan"), plan},
        {QStringLiteral("status"), status},
        {QStringLiteral("features"), features},
        {QStringLiteral("quotas"), quotas},
    };

    const QByteArray signingInput =
        b64Url(QJsonDocument(header).toJson(QJsonDocument::Compact)) + '.'
        + b64Url(QJsonDocument(payload).toJson(QJsonDocument::Compact));

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, privKey);
    std::size_t sigLen = 0;
    EVP_DigestSign(ctx.get(),
                   nullptr,
                   &sigLen,
                   reinterpret_cast<const unsigned char *>(signingInput.constData()),
                   static_cast<std::size_t>(signingInput.size()));
    QByteArray derSig(static_cast<int>(sigLen), '\0');
    EVP_DigestSign(ctx.get(),
                   reinterpret_cast<unsigned char *>(derSig.data()),
                   &sigLen,
                   reinterpret_cast<const unsigned char *>(signingInput.constData()),
                   static_cast<std::size_t>(signingInput.size()));
    derSig.resize(static_cast<int>(sigLen));

    const auto *p = reinterpret_cast<const unsigned char *>(derSig.constData());
    std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> ecSig(
        d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(derSig.size())), &ECDSA_SIG_free);
    const BIGNUM *r = nullptr;
    const BIGNUM *s = nullptr;
    ECDSA_SIG_get0(ecSig.get(), &r, &s);
    QByteArray rawSig(64, '\0');
    BN_bn2binpad(r, reinterpret_cast<unsigned char *>(rawSig.data()), 32);
    BN_bn2binpad(s, reinterpret_cast<unsigned char *>(rawSig.data()) + 32, 32);

    return QString::fromLatin1(signingInput + '.' + b64Url(rawSig));
}

struct RecordedRequest
{
    QString method;
    QString path;
    QHash<QString, QString> headers;
    QByteArray body;
};

struct FakeHttpResponse
{
    int status = 200;
    QByteArray body = "{}";
    QByteArray extraHeaders;
};

class FakeCloudServer : public QObject
{
public:
    using HandlerFn = std::function<FakeHttpResponse(const RecordedRequest &)>;

    explicit FakeCloudServer(HandlerFn handler) : m_handler(std::move(handler))
    {
        ensureCoreApp();
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this]() {
            while (QTcpSocket *sock = m_server.nextPendingConnection())
                attachSocket(sock);
        });
        EXPECT_TRUE(m_server.listen(QHostAddress::LocalHost, 0));
    }

    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }

    int totalRequests() const { return static_cast<int>(m_requests.size()); }
    const std::vector<RecordedRequest> &requests() const { return m_requests; }

    void setHandler(HandlerFn fn) { m_handler = std::move(fn); }

private:
    void attachSocket(QTcpSocket *sock)
    {
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buffer]() {
            buffer->append(sock->readAll());
            const int headerEnd = buffer->indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return;

            const QByteArray headerBlock = buffer->left(headerEnd);
            const QList<QByteArray> lines = headerBlock.split('\n');
            if (lines.isEmpty())
                return;

            const QList<QByteArray> reqLine = lines[0].trimmed().split(' ');
            if (reqLine.size() < 2)
                return;

            QHash<QString, QString> headers;
            int contentLen = 0;
            for (int i = 1; i < lines.size(); ++i)
            {
                const QByteArray line = lines[i].trimmed();
                const int colon = line.indexOf(':');
                if (colon > 0)
                {
                    const QString k = QString::fromLatin1(line.left(colon)).trimmed().toLower();
                    const QString v = QString::fromLatin1(line.mid(colon + 1)).trimmed();
                    headers.insert(k, v);
                    if (k == QLatin1String("content-length"))
                        contentLen = v.toInt();
                }
            }

            if (buffer->size() < headerEnd + 4 + contentLen)
                return;

            RecordedRequest req;
            req.method = QString::fromLatin1(reqLine[0]);
            req.path = QString::fromLatin1(reqLine[1]);
            req.headers = headers;
            req.body = buffer->mid(headerEnd + 4, contentLen);
            buffer->clear();

            m_requests.push_back(req);
            const FakeHttpResponse resp = m_handler ? m_handler(req) : FakeHttpResponse{500, "{}"};

            QByteArray wire;
            wire.append("HTTP/1.1 ");
            wire.append(QByteArray::number(resp.status));
            wire.append(" OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ");
            wire.append(QByteArray::number(resp.body.size()));
            wire.append("\r\n");
            if (!resp.extraHeaders.isEmpty())
                wire.append(resp.extraHeaders);
            wire.append("\r\n");
            wire.append(resp.body);

            sock->write(wire);
            sock->flush();
            sock->disconnectFromHost();
        });
    }

    QTcpServer m_server;
    HandlerFn m_handler;
    std::vector<RecordedRequest> m_requests;
};
} // namespace

TEST(CloudClientTest, NeverPhonesHomeWhenUnsignedInAndValidatesUrlsStrictly)
{
    FakeCloudServer server([](const RecordedRequest &) { return FakeHttpResponse{200, "{}"}; });

    auto creds = std::make_shared<InMemoryCloudCredentialStore>();
    CloudClient client(creds);
    client.setApiUrlOverride(server.baseUrl());
    client.restoreOfflineSession();
    QCoreApplication::processEvents();

    EXPECT_FALSE(client.isSignedIn());
    EXPECT_EQ(client.plan(), EntitlementPlan::Free);
    EXPECT_EQ(server.totalRequests(), 0);

    // API URL validation
    EXPECT_TRUE(CloudClient::isAllowedApiBaseUrl(QStringLiteral("https://api.ragoshell.com.tr")));
    EXPECT_TRUE(CloudClient::isAllowedApiBaseUrl(QStringLiteral("http://127.0.0.1:8080")));
    EXPECT_TRUE(CloudClient::isAllowedApiBaseUrl(QStringLiteral("http://localhost:8080")));
    EXPECT_FALSE(CloudClient::isAllowedApiBaseUrl(QStringLiteral("http://api.ragoshell.com.tr")));
    EXPECT_FALSE(CloudClient::isAllowedApiBaseUrl(QStringLiteral("https://user:pw@api.ragoshell.com.tr")));
    EXPECT_FALSE(CloudClient::isAllowedApiBaseUrl(QStringLiteral("javascript:alert(1)")));

    // Verification URI validation (anti-phishing)
    const QString prodApi = QStringLiteral("https://api.ragoshell.com.tr");
    const QString prodPortal = QStringLiteral("https://app.ragoshell.com.tr");
    EXPECT_TRUE(CloudClient::isAllowedVerificationUrl(
        QStringLiteral("https://app.ragoshell.com.tr/device?code=ABCD-2345"), prodApi, prodPortal));
    EXPECT_FALSE(CloudClient::isAllowedVerificationUrl(
        QStringLiteral("https://evil.example/device?code=ABCD-2345"), prodApi, prodPortal));
    EXPECT_FALSE(CloudClient::isAllowedVerificationUrl(
        QStringLiteral("http://app.ragoshell.com.tr/device"), prodApi, prodPortal));
    EXPECT_FALSE(CloudClient::isAllowedVerificationUrl(
        QStringLiteral("javascript:alert(1)"), prodApi, prodPortal));
    EXPECT_FALSE(CloudClient::isAllowedVerificationUrl(
        QStringLiteral("https://app.ragoshell.com.tr/../admin"), prodApi, prodPortal));
    EXPECT_FALSE(CloudClient::isAllowedVerificationUrl(
        QStringLiteral("http://127.0.0.1:8080/device"), prodApi, prodPortal));
    EXPECT_TRUE(CloudClient::isAllowedVerificationUrl(
        QStringLiteral("http://127.0.0.1:8080/device"),
        QStringLiteral("http://127.0.0.1:8080"),
        prodPortal));
    EXPECT_FALSE(CloudClient::isAllowedVerificationUrl(
        QStringLiteral("http://127.0.0.1:9999/device"),
        QStringLiteral("http://127.0.0.1:8080"),
        prodPortal));

    // Device name sanitization: strips Unicode Cf/Bidi/C1 controls and preserves surrogate pairs
    const QString dirtyName = QStringLiteral("Mac\u200BBook\u202E\u0085 Pro");
    EXPECT_EQ(CloudClient::sanitizeDeviceName(dirtyName), QStringLiteral("MacBook Pro"));
    const QString longEmoji = QString(127, QLatin1Char('A')) + QStringLiteral("🚀EXTRA");
    const QString cleanEmoji = CloudClient::sanitizeDeviceName(longEmoji);
    EXPECT_EQ(cleanEmoji.toUcs4().size(), 128);
    EXPECT_TRUE(cleanEmoji.endsWith(QStringLiteral("🚀")));
}

TEST(CloudClientTest, RejectsPhishingVerificationUriFromDeviceCodeEndpoint)
{
    FakeCloudServer server([](const RecordedRequest &req) -> FakeHttpResponse {
        if (req.path == QLatin1String("/v1/auth/device/code"))
        {
            const QJsonObject obj{
                {QStringLiteral("device_code"), QStringLiteral("dev-secret-1")},
                {QStringLiteral("user_code"), QStringLiteral("ABCD-2345")},
                {QStringLiteral("verification_uri"), QStringLiteral("https://phishing.example/device")},
                {QStringLiteral("verification_uri_complete"), QStringLiteral("https://phishing.example/device?code=ABCD-2345")},
                {QStringLiteral("expires_in"), 600},
                {QStringLiteral("interval"), 0},
            };
            return {200, QJsonDocument(obj).toJson(QJsonDocument::Compact)};
        }
        return {500, "{}"};
    });

    auto creds = std::make_shared<InMemoryCloudCredentialStore>();
    CloudClient client(creds);
    client.setApiUrlOverride(server.baseUrl());
    client.startDeviceSignIn(QStringLiteral("Mac Studio"));

    ASSERT_TRUE(waitUntil([&]() { return client.authState() == CloudAuthState::SignedOut && !client.isBusy(); }));
    EXPECT_EQ(client.lastErrorCode(), QStringLiteral("unsafe_verification_uri"));
    EXPECT_TRUE(client.userCode().isEmpty());
    EXPECT_EQ(creds->size(), 0);
}

TEST(CloudClientTest, CompletesDeviceFlowHandlesSlowDownVerifiesEs256EntitlementAndNeverStoresTokensInSettingsJson)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    const QByteArray prevCfg = qgetenv("RAGOSHELL_CONFIG_DIR");
    const bool hadCfg = qEnvironmentVariableIsSet("RAGOSHELL_CONFIG_DIR");
    qputenv("RAGOSHELL_CONFIG_DIR", tmpDir.path().toUtf8());

    EvpPkeyPtr es256Key = makeP256Key();
    EntitlementVerifier verifier;
    ASSERT_TRUE(verifier.addPublicKeySpkiDer(QStringLiteral("k-2026"), exportSpkiDer(es256Key.get())));

    const qint64 fixedNow = 1793707200LL;
    const QString userId = QStringLiteral("550e8400-e29b-41d4-a716-446655440000");
    const QString deviceId = QStringLiteral("7c9e6679-7425-40de-944b-e07fc1f90ae7");
    int tokenPollCount = 0;

    QString serverUrl;
    FakeCloudServer server([&](const RecordedRequest &req) -> FakeHttpResponse {
        if (req.path == QLatin1String("/v1/auth/device/code"))
        {
            const QJsonObject reqJson = QJsonDocument::fromJson(req.body).object();
            EXPECT_EQ(reqJson.value(QStringLiteral("device_name")).toString(), QStringLiteral("Dev Laptop"));
            EXPECT_EQ(reqJson.value(QStringLiteral("platform")).toString(), QStringLiteral("macos"));
            const QJsonObject resp{
                {QStringLiteral("device_code"), QStringLiteral("SecretDeviceCode_XYZ")},
                {QStringLiteral("user_code"), QStringLiteral("HJKL-7890")},
                {QStringLiteral("verification_uri"), serverUrl + QStringLiteral("/device")},
                {QStringLiteral("verification_uri_complete"), serverUrl + QStringLiteral("/device?code=HJKL-7890")},
                {QStringLiteral("expires_in"), 600},
                {QStringLiteral("interval"), 0},
            };
            return {200, QJsonDocument(resp).toJson(QJsonDocument::Compact)};
        }
        if (req.path == QLatin1String("/v1/auth/device/token"))
        {
            ++tokenPollCount;
            if (tokenPollCount == 1)
            {
                return {400, R"({"error":{"code":"authorization_pending","message":"Pending approval."}})"};
            }
            if (tokenPollCount == 2)
            {
                return {400, R"({"error":{"code":"slow_down","message":"Polling too fast."}})"};
            }
            const QJsonObject tok{
                {QStringLiteral("access_token"), QStringLiteral("AccessTok_Secret_111")},
                {QStringLiteral("refresh_token"), QStringLiteral("RefreshTok_Secret_222")},
                {QStringLiteral("token_type"), QStringLiteral("Bearer")},
                {QStringLiteral("expires_in"), 900},
                {QStringLiteral("device_id"), deviceId},
            };
            return {200, QJsonDocument(tok).toJson(QJsonDocument::Compact)};
        }
        if (req.path == QLatin1String("/v1/me"))
        {
            EXPECT_EQ(req.headers.value(QStringLiteral("authorization")),
                      QStringLiteral("Bearer AccessTok_Secret_111"));
            const QJsonObject me{
                {QStringLiteral("id"), userId},
                {QStringLiteral("email"), QStringLiteral("dba@ragoshell.com.tr")},
                {QStringLiteral("locale"), QStringLiteral("tr")},
                {QStringLiteral("emailVerified"), true},
                {QStringLiteral("totpEnabled"), true},
                {QStringLiteral("plan"), QStringLiteral("pro")},
                {QStringLiteral("createdAt"), QStringLiteral("2026-10-01T00:00:00Z")},
            };
            return {200, QJsonDocument(me).toJson(QJsonDocument::Compact)};
        }
        if (req.path == QLatin1String("/v1/me/entitlements"))
        {
            const QString jwt = signEntitlementJwt(
                es256Key.get(), QStringLiteral("k-2026"), serverUrl, userId, deviceId, fixedNow);
            const QJsonObject ent{
                {QStringLiteral("token"), jwt},
                {QStringLiteral("kid"), QStringLiteral("k-2026")},
                {QStringLiteral("plan"), QStringLiteral("pro")},
                {QStringLiteral("status"), QStringLiteral("active")},
            };
            return {200, QJsonDocument(ent).toJson(QJsonDocument::Compact)};
        }
        return {404, R"({"error":{"code":"not_found","message":"Not found"}})"};
    });
    serverUrl = server.baseUrl();

    SettingsManager settings;
    auto creds = std::make_shared<InMemoryCloudCredentialStore>();
    CloudClient client(creds, &settings);
    client.setApiUrlOverride(serverUrl);
    client.setPlatformOverride(QStringLiteral("macos"), QStringLiteral("arm64"), QStringLiteral("2.1.0"));
    client.setEntitlementVerifier(verifier);
    client.setClockForTest([fixedNow]() { return fixedNow; });
    client.setMinPollIntervalMsForTest(20);

    client.startDeviceSignIn(QStringLiteral("Dev Laptop"));
    ASSERT_TRUE(waitUntil([&]() { return client.authState() == CloudAuthState::AwaitingBrowserApproval; }));
    EXPECT_EQ(client.userCode(), QStringLiteral("HJKL-7890"));

    // Wait for authorization_pending -> slow_down (+5000ms per RFC 8628 §3.5 -> 5020ms):
    ASSERT_TRUE(waitUntil([&]() { return client.pollIntervalMs() == 5020; }, 2000));
    EXPECT_EQ(tokenPollCount, 2);

    // Wait for the post-slow_down poll + /v1/me + /v1/me/entitlements to complete.
    ASSERT_TRUE(waitUntil(
        [&]() { return client.isSignedIn() && client.plan() == EntitlementPlan::Pro && !client.isBusy(); },
        8000));

    EXPECT_EQ(tokenPollCount, 3);
    EXPECT_EQ(client.userEmail(), QStringLiteral("dba@ragoshell.com.tr"));
    EXPECT_EQ(client.deviceId(), deviceId);
    EXPECT_TRUE(client.features().sync);
    EXPECT_TRUE(client.features().hostedAi);
    EXPECT_EQ(client.quotas().aiMonthlyTokenQuota, 2000000);

    // Secrets are stored ONLY in credential store, NEVER in ragoshell.json!
    EXPECT_EQ(creds->peek(QLatin1String(ICloudCredentialStore::kRefreshTokenSlot)),
              QStringLiteral("RefreshTok_Secret_222"));
    EXPECT_FALSE(creds->peek(QLatin1String(ICloudCredentialStore::kEntitlementJwtSlot)).isEmpty());

    QFile cfgFile{configFilePath()};
    ASSERT_TRUE(cfgFile.open(QIODevice::ReadOnly));
    const QString cfgJson = QString::fromUtf8(cfgFile.readAll());
    EXPECT_FALSE(cfgJson.contains(QStringLiteral("AccessTok_Secret_111")));
    EXPECT_FALSE(cfgJson.contains(QStringLiteral("RefreshTok_Secret_222")));
    EXPECT_FALSE(cfgJson.contains(QStringLiteral("SecretDeviceCode_XYZ")));
    EXPECT_FALSE(cfgJson.contains(QStringLiteral("eyJhbGci")));

    // Offline restart in day 9 (inside 14-day offline grace window): 0 network calls, Pro active!
    const int reqBeforeRestart = server.totalRequests();
    CloudClient offlineClient(creds, &settings);
    offlineClient.setApiUrlOverride(serverUrl);
    offlineClient.setEntitlementVerifier(verifier);
    offlineClient.setClockForTest([fixedNow]() { return fixedNow + 9LL * 24 * 3600; });
    offlineClient.restoreOfflineSession();

    EXPECT_EQ(server.totalRequests(), reqBeforeRestart);
    EXPECT_TRUE(offlineClient.isSignedIn());
    EXPECT_TRUE(offlineClient.isOfflineGraceActive());
    EXPECT_EQ(offlineClient.plan(), EntitlementPlan::Pro);
    EXPECT_TRUE(offlineClient.features().sync);

    if (hadCfg)
        qputenv("RAGOSHELL_CONFIG_DIR", prevCfg);
    else
        qunsetenv("RAGOSHELL_CONFIG_DIR");
}

TEST(CloudClientTest, SingleFlightRefreshRotatesOnceRevokesOn401AndSignsOutCleanlyEvenOn500)
{
    EvpPkeyPtr es256Key = makeP256Key();
    EntitlementVerifier verifier;
    ASSERT_TRUE(verifier.addPublicKeySpkiDer(QStringLiteral("k1"), exportSpkiDer(es256Key.get())));

    const qint64 fixedNow = 1793707200LL;
    const QString userId = QStringLiteral("550e8400-e29b-41d4-a716-446655440000");
    const QString deviceId = QStringLiteral("7c9e6679-7425-40de-944b-e07fc1f90ae7");

    int refreshCount = 0;
    bool revokeNextRefresh = false;
    bool failLogout500 = false;
    RecordedRequest lastLogoutReq;

    QString serverUrl;
    FakeCloudServer server([&](const RecordedRequest &req) -> FakeHttpResponse {
        if (req.path == QLatin1String("/v1/auth/refresh"))
        {
            ++refreshCount;
            if (revokeNextRefresh)
            {
                return {401, R"({"error":{"code":"invalid_grant","message":"Refresh token revoked."}})"};
            }
            const QJsonObject reqObj = QJsonDocument::fromJson(req.body).object();
            EXPECT_EQ(reqObj.value(QStringLiteral("refresh_token")).toString(), QStringLiteral("Refresh_Initial"));
            const QJsonObject tok{
                {QStringLiteral("access_token"), QStringLiteral("Access_Rotated_1")},
                {QStringLiteral("refresh_token"), QStringLiteral("Refresh_Rotated_1")},
                {QStringLiteral("token_type"), QStringLiteral("Bearer")},
                {QStringLiteral("expires_in"), 900},
                {QStringLiteral("device_id"), deviceId},
            };
            return {200, QJsonDocument(tok).toJson(QJsonDocument::Compact)};
        }
        if (req.path == QLatin1String("/v1/auth/logout"))
        {
            lastLogoutReq = req;
            return failLogout500 ? FakeHttpResponse{500, R"({"error":{"code":"internal_error","message":"boom"}})"}
                                 : FakeHttpResponse{204, ""};
        }
        return {404, "{}"};
    });
    serverUrl = server.baseUrl();

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    const QByteArray prevCfg = qgetenv("RAGOSHELL_CONFIG_DIR");
    const bool hadCfg = qEnvironmentVariableIsSet("RAGOSHELL_CONFIG_DIR");
    qputenv("RAGOSHELL_CONFIG_DIR", tmpDir.path().toUtf8());

    SettingsManager settings;
    settings.addCacheData(QStringLiteral("cloudSignedIn"), true);
    settings.addCacheData(QStringLiteral("cloudDeviceId"), deviceId);
    settings.addCacheData(QStringLiteral("cloudUserId"), userId);
    settings.addCacheData(QStringLiteral("cloudUserEmail"), QStringLiteral("user@ragoshell.com.tr"));

    auto creds = std::make_shared<InMemoryCloudCredentialStore>();
    creds->writeSecret(QLatin1String(ICloudCredentialStore::kRefreshTokenSlot), QStringLiteral("Refresh_Initial"));
    creds->writeSecret(
        QLatin1String(ICloudCredentialStore::kEntitlementJwtSlot),
        signEntitlementJwt(es256Key.get(), QStringLiteral("k1"), serverUrl, userId, deviceId, fixedNow));

    CloudClient client(creds, &settings);
    client.setApiUrlOverride(serverUrl);
    client.setEntitlementVerifier(verifier);
    client.setClockForTest([fixedNow]() { return fixedNow; });
    client.restoreOfflineSession();
    ASSERT_TRUE(client.isSignedIn());

    // Fire 3 concurrent withValidAccessToken calls -> single-flight refreshCount == 1!
    int resolvedCallbacks = 0;
    QStringList receivedTokens;
    for (int i = 0; i < 3; ++i)
    {
        client.withValidAccessToken([&](bool ok, const QString &tok, const QString &) {
            EXPECT_TRUE(ok);
            receivedTokens.append(tok);
            ++resolvedCallbacks;
        });
    }
    ASSERT_TRUE(waitUntil([&]() { return resolvedCallbacks == 3; }));
    EXPECT_EQ(refreshCount, 1);
    EXPECT_EQ(receivedTokens, QStringList({QStringLiteral("Access_Rotated_1"),
                                           QStringLiteral("Access_Rotated_1"),
                                           QStringLiteral("Access_Rotated_1")}));
    EXPECT_EQ(creds->peek(QLatin1String(ICloudCredentialStore::kRefreshTokenSlot)),
              QStringLiteral("Refresh_Rotated_1"));

    // SignOut when server returns 500 still wipes KeyStore and sends refresh_token without Authorization header!
    failLogout500 = true;
    bool logoutDone = false;
    QObject::connect(&client, &CloudClient::logoutRequestFinished, &client, [&](bool) { logoutDone = true; });
    client.signOut();
    EXPECT_FALSE(client.isSignedIn());
    EXPECT_EQ(creds->size(), 0);
    ASSERT_TRUE(waitUntil([&]() { return logoutDone; }));
    EXPECT_FALSE(lastLogoutReq.headers.contains(QStringLiteral("authorization")));
    EXPECT_EQ(QJsonDocument::fromJson(lastLogoutReq.body).object().value(QStringLiteral("refresh_token")).toString(),
              QStringLiteral("Refresh_Rotated_1"));

    // Re-seed session and verify 401 invalid_grant on refresh wipes secrets & degrades cleanly to Free
    settings.addCacheData(QStringLiteral("cloudSignedIn"), true);
    settings.addCacheData(QStringLiteral("cloudDeviceId"), deviceId);
    creds->writeSecret(QLatin1String(ICloudCredentialStore::kRefreshTokenSlot), QStringLiteral("Refresh_Revoked"));
    client.restoreOfflineSession();
    ASSERT_TRUE(client.isSignedIn());

    revokeNextRefresh = true;
    bool revokedSignal = false;
    QObject::connect(&client, &CloudClient::sessionRevoked, &client, [&](const QString &code) {
        EXPECT_EQ(code, QStringLiteral("invalid_grant"));
        revokedSignal = true;
    });
    client.expireAccessTokenInMemoryForTest();
    bool waiterFailed = false;
    client.withValidAccessToken([&](bool ok, const QString &, const QString &) { waiterFailed = !ok; });
    ASSERT_TRUE(waitUntil([&]() { return waiterFailed && revokedSignal; }));
    EXPECT_FALSE(client.isSignedIn());
    EXPECT_EQ(client.plan(), EntitlementPlan::Free);
    EXPECT_EQ(creds->size(), 0);

    if (hadCfg)
        qputenv("RAGOSHELL_CONFIG_DIR", prevCfg);
    else
        qunsetenv("RAGOSHELL_CONFIG_DIR");
}

TEST(CloudClientTest, SessionOnlyRamModeWhenKeyStoreUnavailableNeverPersistsGhostSessionAndRotatesInMemory)
{
    QTemporaryDir tempCfg;
    ASSERT_TRUE(tempCfg.isValid());
    const QByteArray prevCfg = qgetenv("RAGOSHELL_CONFIG_DIR");
    const bool hadCfg = qEnvironmentVariableIsSet("RAGOSHELL_CONFIG_DIR");
    qputenv("RAGOSHELL_CONFIG_DIR", tempCfg.path().toUtf8());

    const EvpPkeyPtr es256Key = makeP256Key();
    ASSERT_TRUE(es256Key != nullptr);
    const QString ring = QStringLiteral("k1:%1").arg(QString::fromLatin1(exportSpkiDer(es256Key.get()).toBase64()));
    bool ringOk = false;
    const EntitlementVerifier verifier = EntitlementVerifier::fromKeyRingString(ring, &ringOk);
    ASSERT_TRUE(ringOk);

    const qint64 fixedNow = 1793000000LL;
    const QString userId = QStringLiteral("550e8400-e29b-41d4-a716-446655440000");
    const QString deviceId = QStringLiteral("7c9e6679-7425-40de-944b-e07fc1f90ae7");
    RecordedRequest logoutReq;
    QString serverUrl;

    FakeCloudServer server([&](const RecordedRequest &req) -> FakeHttpResponse {
        if (req.path == QLatin1String("/v1/auth/device/code"))
        {
            const QJsonObject resp{
                {QStringLiteral("device_code"), QStringLiteral("VolatileDeviceCode")},
                {QStringLiteral("user_code"), QStringLiteral("WXYZ-4567")},
                {QStringLiteral("verification_uri"), serverUrl + QStringLiteral("/device")},
                {QStringLiteral("verification_uri_complete"), serverUrl + QStringLiteral("/device?code=WXYZ-4567")},
                {QStringLiteral("expires_in"), 600},
                {QStringLiteral("interval"), 0},
            };
            return {200, QJsonDocument(resp).toJson(QJsonDocument::Compact)};
        }
        if (req.path == QLatin1String("/v1/auth/device/token"))
        {
            const QJsonObject tok{
                {QStringLiteral("access_token"), QStringLiteral("VolAccess_1")},
                {QStringLiteral("refresh_token"), QStringLiteral("VolRefresh_1")},
                {QStringLiteral("token_type"), QStringLiteral("Bearer")},
                {QStringLiteral("expires_in"), 900},
                {QStringLiteral("device_id"), deviceId},
            };
            return {200, QJsonDocument(tok).toJson(QJsonDocument::Compact)};
        }
        if (req.path == QLatin1String("/v1/me"))
        {
            const QJsonObject me{
                {QStringLiteral("id"), userId},
                {QStringLiteral("email"), QStringLiteral("volatile@ragoshell.com.tr")},
                {QStringLiteral("plan"), QStringLiteral("pro")},
            };
            return {200, QJsonDocument(me).toJson(QJsonDocument::Compact)};
        }
        if (req.path == QLatin1String("/v1/me/entitlements"))
        {
            const QString jwt = signEntitlementJwt(
                es256Key.get(), QStringLiteral("k1"), serverUrl, userId, deviceId, fixedNow);
            const QJsonObject ent{
                {QStringLiteral("token"), jwt},
                {QStringLiteral("kid"), QStringLiteral("k1")},
                {QStringLiteral("plan"), QStringLiteral("pro")},
                {QStringLiteral("status"), QStringLiteral("active")},
            };
            return {200, QJsonDocument(ent).toJson(QJsonDocument::Compact)};
        }
        if (req.path == QLatin1String("/v1/auth/refresh"))
        {
            const QJsonObject reqObj = QJsonDocument::fromJson(req.body).object();
            EXPECT_EQ(reqObj.value(QStringLiteral("refresh_token")).toString(), QStringLiteral("VolRefresh_1"));
            const QJsonObject tok{
                {QStringLiteral("access_token"), QStringLiteral("VolAccess_2")},
                {QStringLiteral("refresh_token"), QStringLiteral("VolRefresh_2")},
                {QStringLiteral("token_type"), QStringLiteral("Bearer")},
                {QStringLiteral("expires_in"), 900},
                {QStringLiteral("device_id"), deviceId},
            };
            return {200, QJsonDocument(tok).toJson(QJsonDocument::Compact)};
        }
        if (req.path == QLatin1String("/v1/auth/logout"))
        {
            logoutReq = req;
            return {200, R"({"status":"ok"})"};
        }
        return {404, "{}"};
    });
    serverUrl = server.baseUrl();

    SettingsManager settings;
    auto unavailableCreds = std::make_shared<InMemoryCloudCredentialStore>(false);
    CloudClient client(unavailableCreds, &settings);
    client.setApiUrlOverride(serverUrl);
    client.setEntitlementVerifier(verifier);
    client.setClockForTest([fixedNow]() { return fixedNow; });
    client.setMinPollIntervalMsForTest(15);

    client.startDeviceSignIn(QStringLiteral("Portable Box"));
    ASSERT_TRUE(waitUntil(
        [&]() { return client.isSignedIn() && client.plan() == EntitlementPlan::Pro && !client.isBusy(); }));

    // ragoshell.json MUST NOT record cloudSignedIn: true or any user/device/token data!
    EXPECT_FALSE(settings.cacheData(QStringLiteral("cloudSignedIn")).toBool());
    EXPECT_TRUE(settings.cacheData(QStringLiteral("cloudDeviceId")).toString().isEmpty());
    EXPECT_TRUE(settings.cacheData(QStringLiteral("cloudUserEmail")).toString().isEmpty());

    // After 15 minutes, volatile RAM refresh_token rotates cleanly in memory!
    client.expireAccessTokenInMemoryForTest();
    QString rotatedAccess;
    client.withValidAccessToken([&](bool ok, const QString &tok, const QString &) {
        EXPECT_TRUE(ok);
        rotatedAccess = tok;
    });
    ASSERT_TRUE(waitUntil([&]() { return rotatedAccess == QLatin1String("VolAccess_2"); }));

    // Restarting in a new CloudClient with unavailable KeyStore starts SignedOut (no ghost session)
    CloudClient restarted(unavailableCreds, &settings);
    restarted.restoreOfflineSession();
    EXPECT_FALSE(restarted.isSignedIn());

    // SignOut sends the rotated volatile refresh_token to /v1/auth/logout without Authorization header
    client.expireAccessTokenInMemoryForTest();
    bool logoutFinished = false;
    QObject::connect(&client, &CloudClient::logoutRequestFinished, &client, [&](bool ok) {
        EXPECT_TRUE(ok);
        logoutFinished = true;
    });
    client.signOut();
    ASSERT_TRUE(waitUntil([&]() { return logoutFinished; }));
    EXPECT_FALSE(logoutReq.headers.contains(QStringLiteral("authorization")));
    EXPECT_EQ(QJsonDocument::fromJson(logoutReq.body).object().value(QStringLiteral("refresh_token")).toString(),
              QStringLiteral("VolRefresh_2"));

    // Transient OS keychain lock (readSecret returns false while isAvailable() is true) preserves session!
    class LockedKeyStore final : public ICloudCredentialStore
    {
    public:
        bool isAvailable() const override { return true; }
        bool readSecret(const QString &, QString *, QString *err) override
        {
            if (err)
                *err = QStringLiteral("Keychain temporarily locked");
            return false;
        }
        bool writeSecret(const QString &, const QString &, QString *) override { return true; }
        bool removeSecret(const QString &, QString *) override
        {
            removed = true;
            return true;
        }
        bool removed = false;
    };
    auto lockedStore = std::make_shared<LockedKeyStore>();
    settings.addCacheData(QStringLiteral("cloudSignedIn"), true);
    settings.addCacheData(QStringLiteral("cloudDeviceId"), deviceId);
    CloudClient lockedClient(lockedStore, &settings);
    lockedClient.restoreOfflineSession();
    EXPECT_TRUE(lockedClient.isSignedIn());
    QString errGot;
    lockedClient.withValidAccessToken([&](bool ok, const QString &, const QString &err) {
        EXPECT_FALSE(ok);
        errGot = err;
    });
    EXPECT_EQ(errGot, QStringLiteral("keystore_error"));
    EXPECT_TRUE(lockedClient.isSignedIn());
    EXPECT_FALSE(lockedStore->removed);

    if (hadCfg)
        qputenv("RAGOSHELL_CONFIG_DIR", prevCfg);
    else
        qunsetenv("RAGOSHELL_CONFIG_DIR");
}

// docs/HostedGateway.md §9.8: the app refreshes the entitlement once it is past half of its lifetime, stops
// offering a token whose exp has passed, backs off after failures, and does nothing while off or signed out.
TEST(CloudClientTest, AutoRefreshRenewsPastHalfLifeDropsExpiredTokenAndBacksOff)
{
    EvpPkeyPtr es256Key = makeP256Key();
    EntitlementVerifier verifier;
    ASSERT_TRUE(verifier.addPublicKeySpkiDer(QStringLiteral("k1"), exportSpkiDer(es256Key.get())));

    const qint64 issued = 1793707200LL;
    qint64 now = issued + 3600;
    const QString userId = QStringLiteral("550e8400-e29b-41d4-a716-446655440000");
    const QString deviceId = QStringLiteral("7c9e6679-7425-40de-944b-e07fc1f90ae7");

    int entitlementRequests = 0;
    bool failEntitlements = false;
    QString serverUrl;
    FakeCloudServer server([&](const RecordedRequest &req) -> FakeHttpResponse {
        if (req.path == QLatin1String("/v1/auth/refresh"))
        {
            const QJsonObject tok{
                {QStringLiteral("access_token"), QStringLiteral("Access_Auto")},
                {QStringLiteral("refresh_token"), QStringLiteral("Refresh_Auto")},
                {QStringLiteral("token_type"), QStringLiteral("Bearer")},
                {QStringLiteral("expires_in"), 900},
                {QStringLiteral("device_id"), deviceId},
            };
            return {200, QJsonDocument(tok).toJson(QJsonDocument::Compact)};
        }
        if (req.path == QLatin1String("/v1/me"))
            return {200, QJsonDocument(QJsonObject{{QStringLiteral("id"), userId}}).toJson(QJsonDocument::Compact)};
        if (req.path == QLatin1String("/v1/me/entitlements"))
        {
            ++entitlementRequests;
            if (failEntitlements)
                return {500, R"({"error":{"code":"internal_error","message":"boom"}})"};
            const QJsonObject ent{
                {QStringLiteral("token"),
                 signEntitlementJwt(es256Key.get(), QStringLiteral("k1"), serverUrl, userId, deviceId, now,
                                    QStringLiteral("free"))},
                {QStringLiteral("kid"), QStringLiteral("k1")},
                {QStringLiteral("plan"), QStringLiteral("free")},
                {QStringLiteral("status"), QStringLiteral("active")},
            };
            return {200, QJsonDocument(ent).toJson(QJsonDocument::Compact)};
        }
        return {404, "{}"};
    });
    serverUrl = server.baseUrl();

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    const QByteArray prevCfg = qgetenv("RAGOSHELL_CONFIG_DIR");
    const bool hadCfg = qEnvironmentVariableIsSet("RAGOSHELL_CONFIG_DIR");
    qputenv("RAGOSHELL_CONFIG_DIR", tmpDir.path().toUtf8());

    SettingsManager settings;
    settings.addCacheData(QStringLiteral("cloudSignedIn"), true);
    settings.addCacheData(QStringLiteral("cloudDeviceId"), deviceId);
    settings.addCacheData(QStringLiteral("cloudUserId"), userId);
    auto creds = std::make_shared<InMemoryCloudCredentialStore>();
    creds->writeSecret(QLatin1String(ICloudCredentialStore::kRefreshTokenSlot), QStringLiteral("Refresh_Initial"));
    creds->writeSecret(QLatin1String(ICloudCredentialStore::kEntitlementJwtSlot),
                       signEntitlementJwt(es256Key.get(), QStringLiteral("k1"), serverUrl, userId, deviceId, issued));

    CloudClient client(creds, &settings);
    client.setApiUrlOverride(serverUrl);
    client.setEntitlementVerifier(verifier);
    client.setClockForTest([&now]() { return now; });
    client.restoreOfflineSession();
    ASSERT_TRUE(client.isSignedIn());
    ASSERT_EQ(client.entitlementState(), EntitlementState::ValidOnline);
    int updates = 0;
    QObject::connect(&client, &CloudClient::entitlementsUpdated, &client, [&]() { ++updates; });
    const auto settle = [&]() { waitUntil([&]() { return !client.isBusy(); }, 3000); };

    // Off by default: nothing happens even past half-life.
    now = issued + 4 * 24 * 3600;
    client.checkEntitlementRefresh();
    settle();
    EXPECT_EQ(entitlementRequests, 0);

    // On, before half-life: no request.
    client.setAutoRefreshEnabled(true);
    now = issued + 3 * 24 * 3600;
    client.checkEntitlementRefresh();
    settle();
    EXPECT_EQ(entitlementRequests, 0);

    // Past half-life: one refresh, a new token issued now, the backoff is cleared.
    now = issued + 4 * 24 * 3600;
    client.checkEntitlementRefresh();
    ASSERT_TRUE(waitUntil([&]() { return updates == 1; }));
    EXPECT_EQ(entitlementRequests, 1);
    EXPECT_EQ(client.entitlementState(), EntitlementState::ValidOnline);
    const qint64 renewedAt = now;
    client.checkEntitlementRefresh();
    settle();
    EXPECT_EQ(entitlementRequests, 1); // the new token is young

    // The server fails; time passes beyond exp while the app keeps running: the expired token is no longer
    // offered, and attempts back off (5 min, then 10 min).
    failEntitlements = true;
    now = renewedAt + EntitlementVerifier::kOnlineValiditySec + 60;
    client.checkEntitlementRefresh();
    EXPECT_NE(client.entitlementState(), EntitlementState::ValidOnline);
    EXPECT_TRUE(client.entitlementJwt().isEmpty());
    ASSERT_TRUE(waitUntil([&]() { return entitlementRequests == 2; }));
    settle();
    client.checkEntitlementRefresh();
    settle();
    EXPECT_EQ(entitlementRequests, 2); // inside the 5-minute backoff
    now += CloudClient::kAutoRefreshMinBackoffSec;
    client.checkEntitlementRefresh();
    ASSERT_TRUE(waitUntil([&]() { return entitlementRequests == 3; }));
    settle();
    now += CloudClient::kAutoRefreshMinBackoffSec; // 5 min later: the backoff is now 10 min
    client.checkEntitlementRefresh();
    settle();
    EXPECT_EQ(entitlementRequests, 3);
    now += CloudClient::kAutoRefreshMinBackoffSec;
    failEntitlements = false;
    client.checkEntitlementRefresh();
    ASSERT_TRUE(waitUntil([&]() { return updates == 2; }));
    EXPECT_EQ(entitlementRequests, 4);
    EXPECT_EQ(client.entitlementState(), EntitlementState::ValidOnline);
    EXPECT_FALSE(client.entitlementJwt().isEmpty());

    // Signed out: no request at all.
    client.signOut();
    now += 30LL * 24 * 3600;
    client.checkEntitlementRefresh();
    settle();
    EXPECT_EQ(entitlementRequests, 4);

    if (hadCfg)
        qputenv("RAGOSHELL_CONFIG_DIR", prevCfg);
    else
        qunsetenv("RAGOSHELL_CONFIG_DIR");
}
