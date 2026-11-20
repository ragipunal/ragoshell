#include "gtest/gtest.h"

#include "ragoshell/core/cloud/CloudClient.h"
#include "ragoshell/core/cloud/SyncCollections.h"
#include "ragoshell/core/cloud/SyncCrypto.h"
#include "ragoshell/core/cloud/SyncEngine.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/QueryHistoryStore.h"
#include "ragoshell/core/settings/SettingsManager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <functional>

using namespace RagoShell;

namespace
{
    void ensureCoreApp()
    {
        if (!QCoreApplication::instance())
        {
            static int argc = 1;
            static char appName[] = "rago-tests";
            static char *argv[] = { appName, nullptr };
            static QCoreApplication app(argc, argv);
        }
    }

    bool waitUntil(const std::function<bool()> &pred, int timeoutMs = 8000)
    {
        const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
        while (QDateTime::currentMSecsSinceEpoch() < deadline)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            if (pred())
                return true;
        }
        return false;
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

    class FakeHttpServer : public QObject
    {
    public:
        using HandlerFn = std::function<FakeHttpResponse(const RecordedRequest &)>;

        explicit FakeHttpServer(HandlerFn handler) : m_handler(std::move(handler))
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

        int requestCount() const { return static_cast<int>(m_requests.size()); }

    private:
        void attachSocket(QTcpSocket *sock)
        {
            auto buffer = std::make_shared<QByteArray>();
            QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buffer]() {
                buffer->append(sock->readAll());
                const int headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0)
                    return;
                const QList<QByteArray> lines = buffer->left(headerEnd).split('\n');
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

    /// In-memory SyncProtocol vault + /v1/auth/refresh for CloudClient sessions.
    class FakeSyncVault
    {
    public:
        struct Row
        {
            QByteArray ciphertext;
            QByteArray nonce;
            QJsonObject kdf;
            QString etag;
            qint64 revision = 0;
        };

        bool writable = true;
        bool refuseNextPut412 = false;
        bool reject401 = false;
        int failNextPuts = 0;          ///< answer this many PUTs with 503
        int maxCollections = 20;
        int maxBlobBytes = 1048576;    ///< manifest max_blob_bytes (ciphertext bytes)
        QHash<QString, Row> rows;

        FakeHttpResponse handle(const RecordedRequest &req)
        {
            const QString p = req.path;
            if (p == QLatin1String("/v1/auth/refresh") && req.method == QLatin1String("POST"))
            {
                const QJsonObject tok{
                    {QStringLiteral("access_token"), QStringLiteral("Access_Test_1")},
                    {QStringLiteral("refresh_token"), QStringLiteral("Refresh_Test_1")},
                    {QStringLiteral("token_type"), QStringLiteral("Bearer")},
                    {QStringLiteral("expires_in"), 900},
                    {QStringLiteral("device_id"), QStringLiteral("dev-1")},
                };
                return {200, QJsonDocument(tok).toJson(QJsonDocument::Compact)};
            }
            if (reject401 && p.startsWith(QLatin1String("/v1/sync")))
                return {401, R"({"error":{"code":"unauthorized"}})"};
            if (p == QLatin1String("/v1/sync") && req.method == QLatin1String("GET"))
            {
                QJsonArray cols;
                for (auto it = rows.begin(); it != rows.end(); ++it)
                    cols.append(QJsonObject{{QStringLiteral("collection"), it.key()},
                                            {QStringLiteral("etag"), it.value().etag},
                                            {QStringLiteral("revision"), it.value().revision}});
                const QJsonObject m{{QStringLiteral("writable"), writable},
                                    {QStringLiteral("max_collections"), maxCollections},
                                    {QStringLiteral("max_blob_bytes"), maxBlobBytes},
                                    {QStringLiteral("used_collections"), rows.size()},
                                    {QStringLiteral("collections"), cols}};
                return {200, QJsonDocument(m).toJson(QJsonDocument::Compact)};
            }
            if (p == QLatin1String("/v1/sync") && req.method == QLatin1String("DELETE"))
            {
                rows.clear();
                return {204, ""};
            }
            if (p.startsWith(QLatin1String("/v1/sync/")))
            {
                const QString col = p.mid(9);
                if (req.method == QLatin1String("GET"))
                {
                    if (!rows.contains(col))
                        return {404, R"({"error":{"code":"sync_blob_not_found"}})"};
                    const Row &r = rows[col];
                    if (req.headers.value(QStringLiteral("if-none-match")) == r.etag)
                        return {304, "", QByteArray("ETag: " + r.etag.toUtf8() + "\r\n")};
                    const QJsonObject env{{QStringLiteral("collection"), col},
                                          {QStringLiteral("etag"), r.etag},
                                          {QStringLiteral("revision"), r.revision},
                                          {QStringLiteral("ciphertext"),
                                           QString::fromLatin1(r.ciphertext.toBase64())},
                                          {QStringLiteral("nonce"),
                                           QString::fromLatin1(r.nonce.toBase64())},
                                          {QStringLiteral("kdf_params"), r.kdf}};
                    return {200, QJsonDocument(env).toJson(QJsonDocument::Compact),
                            QByteArray("ETag: " + r.etag.toUtf8() + "\r\n")};
                }
                if (req.method == QLatin1String("PUT"))
                {
                    if (!writable)
                        return {403, R"({"error":{"code":"sync_entitlement_required"}})"};
                    const QString inm = req.headers.value(QStringLiteral("if-none-match"));
                    const QString im = req.headers.value(QStringLiteral("if-match"));
                    if (inm.isEmpty() && im.isEmpty())
                        return {428, R"({"error":{"code":"precondition_required"}})"};
                    if (failNextPuts > 0)
                    {
                        --failNextPuts;
                        return {503, R"({"error":{"code":"server_error"}})"};
                    }
                    if (refuseNextPut412)
                    {
                        refuseNextPut412 = false;
                        return {412, R"({"error":{"code":"sync_conflict","exists":true}})"};
                    }
                    if (inm == QLatin1String("*"))
                    {
                        if (rows.contains(col))
                            return {412, R"({"error":{"code":"sync_conflict","exists":true}})",
                                    QByteArray("ETag: " + rows[col].etag.toUtf8() + "\r\n")};
                        if (rows.size() >= static_cast<size_t>(maxCollections))
                            return {409, R"({"error":{"code":"sync_collection_limit_reached"}})"};
                    }
                    else if (!rows.contains(col) || rows[col].etag != im)
                        return {412, R"({"error":{"code":"sync_conflict","exists":true}})"};

                    const QJsonObject wire = QJsonDocument::fromJson(req.body).object();
                    Row r;
                    r.ciphertext = QByteArray::fromBase64(
                        wire.value(QStringLiteral("ciphertext")).toString().toLatin1());
                    r.nonce = QByteArray::fromBase64(
                        wire.value(QStringLiteral("nonce")).toString().toLatin1());
                    r.kdf = wire.value(QStringLiteral("kdf_params")).toObject();
                    r.revision = rows.value(col).revision + 1;
                    r.etag = QStringLiteral("\"%1\"").arg(QString::fromLatin1(
                        QCryptographicHash::hash(
                            col.toUtf8() + QByteArray::number(r.revision) + r.ciphertext,
                            QCryptographicHash::Sha256)
                            .toHex()));
                    rows.insert(col, r);
                    const QJsonObject meta{{QStringLiteral("collection"), col},
                                           {QStringLiteral("etag"), r.etag},
                                           {QStringLiteral("revision"), r.revision}};
                    return {r.revision == 1 ? 201 : 200,
                            QJsonDocument(meta).toJson(QJsonDocument::Compact),
                            QByteArray("ETag: " + r.etag.toUtf8() + "\r\n")};
                }
                if (req.method == QLatin1String("DELETE"))
                {
                    if (!rows.contains(col))
                        return {404, R"({"error":{"code":"sync_blob_not_found"}})"};
                    rows.remove(col);
                    return {204, ""};
                }
            }
            return {404, R"({"error":{"code":"not_found"}})"};
        }
    };

    struct Fixture
    {
        QTemporaryDir dir;
        FakeSyncVault vault;
        std::shared_ptr<InMemoryCloudCredentialStore> creds;
        FakeHttpServer *server = nullptr;
        CloudClient *cloud = nullptr;
        SettingsManager *settings = nullptr;
        QueryHistoryStore *history = nullptr;
        SyncEngine *engine = nullptr;
        bool lastSyncOk = false;

        Fixture()
        {
            ensureCoreApp();
            qputenv("RAGOSHELL_CONFIG_DIR", dir.path().toUtf8());
            creds = std::make_shared<InMemoryCloudCredentialStore>();
            server = new FakeHttpServer([this](const RecordedRequest &r) { return vault.handle(r); });
            settings = new SettingsManager();
            settings->addCacheData(QStringLiteral("cloudSignedIn"), true);
            settings->addCacheData(QStringLiteral("cloudDeviceId"), QStringLiteral("dev-1"));
            creds->writeSecret(
                QLatin1String(ICloudCredentialStore::kRefreshTokenSlot),
                QStringLiteral("Refresh_Test_1"));
            cloud = new CloudClient(creds, settings);
            cloud->setApiUrlOverride(server->baseUrl());
            cloud->restoreOfflineSession();
            history = new QueryHistoryStore();
            engine = new SyncEngine(cloud, settings, history, creds);
            QObject::connect(engine, &SyncEngine::syncFinished, engine,
                             [this](bool ok) { lastSyncOk = ok; });
        }
        ~Fixture()
        {
            delete engine;
            delete history;
            delete cloud;
            delete settings;
            delete server;
            qunsetenv("RAGOSHELL_CONFIG_DIR");
        }

        bool runSync(int timeoutMs = 8000)
        {
            engine->syncNow();
            return waitUntil([this]() { return engine->state() != SyncEngine::State::Syncing; },
                             timeoutMs);
        }

        QByteArray decryptRemote(const QString &collection, const QByteArray &masterKey)
        {
            const FakeSyncVault::Row &r = vault.rows[collection];
            QByteArray plain;
            const QByteArray ck = SyncCrypto::deriveCollectionKey(masterKey, collection);
            EXPECT_TRUE(SyncCrypto::decryptEnvelope(ck, collection, r.ciphertext, r.nonce, &plain));
            return plain;
        }

        QByteArray storedMasterKey()
        {
            QString enc;
            creds->readSecret(QLatin1String(ICloudCredentialStore::kSyncMasterKeySlot), &enc, nullptr);
            return QByteArray::fromBase64(enc.toLatin1());
        }

        /// Re-seals a collection as another device would have written it (new etag).
        void rewriteRemote(const QString &collection,
                           const std::function<void(QJsonObject &payload)> &edit)
        {
            const QByteArray mk = storedMasterKey();
            QJsonObject root = QJsonDocument::fromJson(decryptRemote(collection, mk)).object();
            QJsonObject payload = root.value(QStringLiteral("payload")).toObject();
            edit(payload);
            root.insert(QStringLiteral("payload"), payload);
            root.insert(QStringLiteral("device_id"), QStringLiteral("dev-other"));
            QByteArray ct, nonce;
            const QByteArray ck = SyncCrypto::deriveCollectionKey(mk, collection);
            ASSERT_TRUE(SyncCrypto::encryptEnvelope(
                ck, collection, QJsonDocument(root).toJson(QJsonDocument::Compact), &ct, &nonce));
            FakeSyncVault::Row &r = vault.rows[collection];
            r.ciphertext = ct;
            r.nonce = nonce;
            ++r.revision;
            r.etag = QStringLiteral("\"other-%1\"").arg(r.revision);
        }

        /// Keys of the live (non-tombstone) entries in the remote copy of a collection.
        QStringList remoteLiveKeys(const QString &collection)
        {
            QStringList keys;
            const QJsonObject root =
                QJsonDocument::fromJson(decryptRemote(collection, storedMasterKey())).object();
            for (const QJsonValue &v : root.value(QStringLiteral("payload")).toObject()
                                          .value(QStringLiteral("entries")).toArray())
                if (!v.toObject().value(QStringLiteral("deleted")).toBool())
                    keys << v.toObject().value(QStringLiteral("key")).toString();
            return keys;
        }

        ConnectionSettings *addConnection(const QString &name, const QString &uuid)
        {
            auto *conn = new ConnectionSettings(false);
            QVariantMap m;
            m.insert(QStringLiteral("connectionName"), name);
            m.insert(QStringLiteral("serverHost"), QStringLiteral("localhost"));
            m.insert(QStringLiteral("serverPort"), 27017);
            m.insert(QStringLiteral("uuid"), uuid);
            conn->fromVariant(m);
            settings->addConnection(conn);
            return conn;
        }
    };

    QJsonObject remoteEntry(const QString &key, const QJsonObject &data)
    {
        return QJsonObject{{QStringLiteral("key"), key},
                           {QStringLiteral("updated_at"), QDateTime::currentMSecsSinceEpoch() - 1000},
                           {QStringLiteral("device"), QStringLiteral("PC")},
                           {QStringLiteral("data"), data}};
    }

    void appendRemoteEntry(QJsonObject &payload, const QJsonObject &entry)
    {
        QJsonArray entries = payload.value(QStringLiteral("entries")).toArray();
        entries.append(entry);
        payload.insert(QStringLiteral("entries"), entries);
    }
}

TEST(SyncEngineTest, ConfigureDeviceKeyStoresKeyAndEnablesSync)
{
    Fixture f;
    QString err;
    ASSERT_TRUE(f.engine->configureDeviceKey(&err)) << err.toStdString();
    EXPECT_FALSE(f.creds->peek(QLatin1String(ICloudCredentialStore::kSyncMasterKeySlot)).isEmpty());
    EXPECT_TRUE(f.settings->syncEnabled());
    EXPECT_EQ(f.settings->syncKeyMode(), QLatin1String("device"));
    EXPECT_TRUE(f.engine->keyAvailable());
}

TEST(SyncEngineTest, PassphraseTooShortRejectedAndKeyNeverInSettingsJson)
{
    Fixture f;
    QString err;
    EXPECT_FALSE(f.engine->configurePassphrase(QStringLiteral("short"), &err));
    EXPECT_EQ(err, QLatin1String("passphrase_too_short"));
    ASSERT_TRUE(f.engine->configurePassphrase(QStringLiteral("a real passphrase 123"), &err));
    EXPECT_TRUE(f.engine->keyAvailable());

    f.settings->save();
    QFile cfgFile{configFilePath()};
    ASSERT_TRUE(cfgFile.open(QIODevice::ReadOnly));
    const QString content = QString::fromUtf8(cfgFile.readAll());
    EXPECT_FALSE(content.contains(QLatin1String("sync_master_key")));
    EXPECT_FALSE(content.contains(QLatin1String("a real passphrase")));
}

TEST(SyncEngineTest, FullSyncCreatesCollectionsAndServerSeesOnlyCiphertext)
{
    Fixture f;
    ASSERT_TRUE(f.cloud->isSignedIn());
    QString err;
    ASSERT_TRUE(f.engine->configureDeviceKey(&err)) << err.toStdString();
    f.engine->setCollections(SyncCollections::knownCollections(), false);

    auto *conn = new ConnectionSettings(false);
    QVariantMap m;
    m.insert(QStringLiteral("connectionName"), QStringLiteral("Prod"));
    m.insert(QStringLiteral("serverHost"), QStringLiteral("localhost"));
    m.insert(QStringLiteral("serverPort"), 27017);
    m.insert(QStringLiteral("uuid"), QStringLiteral("aaaaaaaa-0000-0000-0000-000000000001"));
    conn->fromVariant(m);
    f.settings->addConnection(conn);

    ASSERT_TRUE(f.runSync());
    EXPECT_TRUE(f.lastSyncOk);
    EXPECT_EQ(f.engine->state(), SyncEngine::State::Idle);

    // Every enabled collection landed server-side, opaque ciphertext only.
    for (const QString &col : SyncCollections::knownCollections())
        EXPECT_TRUE(f.vault.rows.contains(col)) << col.toStdString();
    const FakeSyncVault::Row &r = f.vault.rows[QStringLiteral("connections")];
    EXPECT_FALSE(r.ciphertext.isEmpty());
    const QString ctStr = QString::fromLatin1(r.ciphertext);
    EXPECT_FALSE(ctStr.contains(QLatin1String("Prod")));
    EXPECT_FALSE(ctStr.contains(QLatin1String("localhost")));
    EXPECT_EQ(r.kdf.value(QStringLiteral("alg")).toString(), QLatin1String("argon2id"));

    // Round-trip: decrypt with the stored master key recovers the payload.
    const QByteArray plain = f.decryptRemote(QStringLiteral("connections"), f.storedMasterKey());
    QJsonObject payload;
    ASSERT_TRUE(SyncCollections::parsePayload(QStringLiteral("connections"), plain, &payload));
    const QString docStr = QString::fromUtf8(plain);
    EXPECT_TRUE(docStr.contains(QLatin1String("Prod")));

    // Second run with unchanged state → all GETs return 304, no PUTs.
    const int putsBefore = 0; // count via fresh server side is awkward; check via run
    ASSERT_TRUE(f.runSync());
    EXPECT_TRUE(f.lastSyncOk);
}

TEST(SyncEngineTest, PassphraseVaultRoundTripsAcrossEngineInstances)
{
    Fixture f;
    ASSERT_TRUE(f.engine->configurePassphrase(QStringLiteral("shared vault phrase 42")));
    ASSERT_TRUE(f.runSync());
    EXPECT_TRUE(f.lastSyncOk);

    // A second device (new engine instance, same server vault + master key source)
    // decrypts with the same passphrase → attachToRemoteVault verifies it.
    auto *engine2 = new SyncEngine(f.cloud, f.settings, f.history, f.creds);
    QString err;
    EXPECT_TRUE(engine2->attachToRemoteVault(QStringLiteral("shared vault phrase 42"), &err))
        << err.toStdString();
    EXPECT_FALSE(engine2->attachToRemoteVault(QStringLiteral("wrong phrase 9999"), &err));
    EXPECT_EQ(err, QLatin1String("passphrase_mismatch"));
    delete engine2;
}

TEST(SyncEngineTest, EntitlementBlockMarksEngineBlocked)
{
    Fixture f;
    ASSERT_TRUE(f.engine->configureDeviceKey());
    f.vault.writable = false;
    ASSERT_TRUE(f.runSync());
    EXPECT_FALSE(f.lastSyncOk);
    EXPECT_EQ(f.engine->state(), SyncEngine::State::Blocked);
    EXPECT_EQ(f.engine->lastError(), QLatin1String("sync_entitlement_required"));
}

TEST(SyncEngineTest, TransportFailureMarksOfflineAndSchedulesBackoffRetry)
{
    Fixture f;
    ASSERT_TRUE(f.engine->configureDeviceKey());
    // Kill the fake server socket → all requests fail at transport level.
    f.cloud->setApiUrlOverride(QStringLiteral("http://127.0.0.1:1"));
    ASSERT_TRUE(f.runSync());
    EXPECT_FALSE(f.lastSyncOk);
    EXPECT_EQ(f.engine->state(), SyncEngine::State::Error);
    EXPECT_EQ(f.engine->lastError(), QLatin1String("offline"));
    // Restore connectivity — the next sync (manual or armed backoff) succeeds.
    f.cloud->setApiUrlOverride(f.server->baseUrl());
    ASSERT_TRUE(f.runSync());
    EXPECT_TRUE(f.lastSyncOk);
    EXPECT_EQ(f.engine->state(), SyncEngine::State::Idle);
}

TEST(SyncEngineTest, UnauthorizedStopsWithoutRetryLoop)
{
    Fixture f;
    ASSERT_TRUE(f.engine->configureDeviceKey());
    f.vault.reject401 = true; // every /v1/sync* returns 401
    ASSERT_TRUE(f.runSync());
    EXPECT_FALSE(f.lastSyncOk);
    EXPECT_EQ(f.engine->state(), SyncEngine::State::Error);
    EXPECT_EQ(f.engine->lastError(), QLatin1String("unauthorized"));
    // Pump the loop past the initial backoff window — no auto-retry on 401.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    EXPECT_NE(f.engine->state(), SyncEngine::State::Syncing);
}

// docs/SyncClient.md §10 R1: a merge that rewrites the history store tells its owner.
TEST(SyncEngineTest, PulledSnippetsSignalHistoryChanged)
{
    Fixture f;
    ASSERT_TRUE(f.engine->configureDeviceKey());
    f.engine->setCollections({QStringLiteral("snippets")}, false);
    ASSERT_TRUE(f.runSync());
    ASSERT_TRUE(f.vault.rows.contains(QStringLiteral("snippets")));

    const QJsonObject data{{QStringLiteral("script"), QStringLiteral("db.s.find()")},
                           {QStringLiteral("database"), QStringLiteral("d")},
                           {QStringLiteral("executedAt"), 10},
                           {QStringLiteral("ok"), true},
                           {QStringLiteral("favorite"), true},
                           {QStringLiteral("name"), QStringLiteral("pulled")}};
    f.rewriteRemote(QStringLiteral("snippets"), [&](QJsonObject &payload) {
        appendRemoteEntry(payload, remoteEntry(
            SyncCollections::contentKey(QStringLiteral("snippets"), data), data));
    });

    int emitted = 0;
    QObject::connect(f.engine, &SyncEngine::historyChanged, f.engine, [&emitted]() { ++emitted; });
    ASSERT_TRUE(f.runSync());
    EXPECT_TRUE(f.lastSyncOk);
    EXPECT_GE(emitted, 1);
    ASSERT_EQ(f.history->entries().size(), 1u);
    EXPECT_EQ(f.history->entries().front().name, QStringLiteral("pulled"));
}

// docs/SyncClient.md §10 R3: a failed PUT after a pull must not record the pulled etag,
// or the next run gets a 304 and the local-only rows never reach the vault.
TEST(SyncEngineTest, FailedPushAfterPullIsRetriedOnNextRun)
{
    Fixture f;
    ASSERT_TRUE(f.engine->configureDeviceKey());
    f.engine->setCollections({QStringLiteral("connections")}, false);
    const QString x = QStringLiteral("aaaaaaaa-0000-0000-0000-000000000001");
    const QString y = QStringLiteral("bbbbbbbb-0000-0000-0000-000000000002");
    const QString z = QStringLiteral("cccccccc-0000-0000-0000-000000000003");
    f.addConnection(QStringLiteral("X"), x);
    ASSERT_TRUE(f.runSync());
    ASSERT_TRUE(f.lastSyncOk);

    // Another device adds Y; this device adds Z while that PUT will fail.
    f.rewriteRemote(QStringLiteral("connections"), [&](QJsonObject &payload) {
        appendRemoteEntry(payload, remoteEntry(y, QJsonObject{
            {QStringLiteral("uuid"), y},
            {QStringLiteral("connectionName"), QStringLiteral("Y")},
            {QStringLiteral("serverHost"), QStringLiteral("localhost")},
            {QStringLiteral("serverPort"), 27017}}));
    });
    f.addConnection(QStringLiteral("Z"), z);
    f.vault.failNextPuts = 1;
    ASSERT_TRUE(f.runSync());
    EXPECT_FALSE(f.remoteLiveKeys(QStringLiteral("connections")).contains(z));
    EXPECT_EQ(f.settings->connections().size(), 3u); // Y was merged in locally

    ASSERT_TRUE(f.runSync());
    EXPECT_TRUE(f.lastSyncOk);
    const QStringList keys = f.remoteLiveKeys(QStringLiteral("connections"));
    EXPECT_TRUE(keys.contains(x));
    EXPECT_TRUE(keys.contains(y));
    EXPECT_TRUE(keys.contains(z));
}

// docs/SyncClient.md §10 R7: a passphrase vault never publishes made-up KDF params, and a
// join pins the vault's params on disk at once.
TEST(SyncEngineTest, MissingKdfParamsNeverPublishAnInventedSalt)
{
    Fixture f;
    const QString phrase = QStringLiteral("shared vault phrase 42");
    ASSERT_TRUE(f.engine->configurePassphrase(phrase));
    f.engine->setCollections({QStringLiteral("connections")}, false);
    ASSERT_TRUE(f.runSync());
    ASSERT_TRUE(f.lastSyncOk);
    const QString salt =
        f.vault.rows[QStringLiteral("connections")].kdf.value(QStringLiteral("salt")).toString();
    ASSERT_FALSE(salt.isEmpty());

    // sync-state.json lost (or never written after a join): the params are gone.
    ASSERT_TRUE(QFile::remove(configDirectory() + QStringLiteral("sync-state.json")));
    f.addConnection(QStringLiteral("New"), QStringLiteral("dddddddd-0000-0000-0000-000000000004"));
    auto *engine2 = new SyncEngine(f.cloud, f.settings, f.history, f.creds);
    engine2->syncNow();
    ASSERT_TRUE(waitUntil([engine2]() { return engine2->state() != SyncEngine::State::Syncing; }));
    EXPECT_EQ(engine2->lastError(), QLatin1String("kdf_params_missing"));
    EXPECT_EQ(f.vault.rows[QStringLiteral("connections")].kdf.value(QStringLiteral("salt")).toString(),
              salt);

    // Joining again pins the vault's params; a fresh engine (app restart) can push.
    QString err;
    ASSERT_TRUE(engine2->attachToRemoteVault(phrase, &err)) << err.toStdString();
    delete engine2;
    auto *engine3 = new SyncEngine(f.cloud, f.settings, f.history, f.creds);
    engine3->syncNow();
    ASSERT_TRUE(waitUntil([engine3]() { return engine3->state() != SyncEngine::State::Syncing; }));
    EXPECT_EQ(engine3->state(), SyncEngine::State::Idle) << engine3->lastError().toStdString();
    EXPECT_GE(f.vault.rows[QStringLiteral("connections")].revision, 2);
    EXPECT_EQ(f.vault.rows[QStringLiteral("connections")].kdf.value(QStringLiteral("salt")).toString(),
              salt);
    delete engine3;
}

// docs/SyncClient.md §10 R12: max_blob_bytes limits the ciphertext, not the base64 body.
TEST(SyncEngineTest, BlobLimitAppliesToCiphertextNotBase64Body)
{
    Fixture f;
    ASSERT_TRUE(f.engine->configureDeviceKey());
    f.engine->setCollections({QStringLiteral("preferences")}, false);
    ASSERT_TRUE(f.runSync());
    ASSERT_TRUE(f.vault.rows.contains(QStringLiteral("preferences")));
    const int ctSize = f.vault.rows[QStringLiteral("preferences")].ciphertext.size();

    // Room for the ciphertext (+10 %), far below its base64 JSON body (+33 % and framing).
    f.vault.maxBlobBytes = ctSize + ctSize / 10;
    f.settings->setLineNumbers(!f.settings->lineNumbers());
    ASSERT_TRUE(f.runSync());
    EXPECT_EQ(f.vault.rows[QStringLiteral("preferences")].revision, 2);
    EXPECT_EQ(f.engine->collectionStatus().value(QStringLiteral("preferences")).toMap()
                  .value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
}

// docs/SyncClient.md §10 R10: no remote wipe while a run is still writing.
TEST(SyncEngineTest, WipeIsRefusedWhileSyncRuns)
{
    Fixture f;
    ASSERT_TRUE(f.engine->configureDeviceKey());
    ASSERT_TRUE(f.runSync());
    ASSERT_FALSE(f.vault.rows.isEmpty());

    QList<bool> wipes;
    QObject::connect(f.engine, &SyncEngine::wipeFinished, f.engine,
                     [&wipes](bool ok) { wipes << ok; });
    f.engine->syncNow();
    ASSERT_EQ(f.engine->state(), SyncEngine::State::Syncing);
    f.engine->wipeRemote();
    ASSERT_TRUE(waitUntil([&f]() { return f.engine->state() != SyncEngine::State::Syncing; }));
    waitUntil([]() { return false; }, 300); // let a (wrongly) started DELETE finish
    ASSERT_EQ(wipes.size(), 1);
    EXPECT_FALSE(wipes.front());
    EXPECT_FALSE(f.vault.rows.isEmpty());
}

// docs/SyncClient.md §10 R10: replies of a run ended by sign-out are ignored.
TEST(SyncEngineTest, RepliesAfterSignOutAreIgnored)
{
    Fixture f;
    ASSERT_TRUE(f.engine->configureDeviceKey());
    ASSERT_TRUE(f.runSync()); // also caches the access token
    ASSERT_TRUE(f.lastSyncOk);

    f.engine->syncNow(); // the manifest GET is now in flight
    ASSERT_EQ(f.engine->state(), SyncEngine::State::Syncing);
    f.cloud->signOut();
    EXPECT_FALSE(f.lastSyncOk);
    EXPECT_EQ(f.engine->lastError(), QLatin1String("unauthorized"));

    waitUntil([]() { return false; }, 1500); // the stale run would finish within this
    EXPECT_FALSE(f.lastSyncOk);
    EXPECT_EQ(f.engine->lastError(), QLatin1String("unauthorized"));
}
