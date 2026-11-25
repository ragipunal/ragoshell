#include "gtest/gtest.h"

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/cloud/CloudClient.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/AiQueryAssistant.h"
#include "ragoshell/utils/KeyStore.h"
#include "ragoshell/utils/RagoCrypt.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QTemporaryDir>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

namespace
{
    std::string decryptField(const QVariantMap &map, const QString &key)
    {
        return RagoShell::RagoCrypt::decrypt(map.value(key).toString().toStdString());
    }

    class FakeAiKeyStore final : public RagoShell::KeyStore
    {
    public:
        explicit FakeAiKeyStore(bool available = true) : _available(available) {}

        QString name() const override { return QStringLiteral("FakeKeyStore"); }
        bool isAvailable() override { return _available; }

        Result read(const QString &account, QString *value, QString *) override
        {
            if (!_available)
                return Result::Error;
            auto it = _items.constFind(account);
            if (it == _items.constEnd())
                return Result::NotFound;
            if (value)
                *value = it.value();
            return Result::Ok;
        }

        bool write(const QString &account, const QString &value, QString *error) override
        {
            if (!_available || _failWrites) {
                if (error)
                    *error = QStringLiteral("simulated write failure");
                return false;
            }
            _items.insert(account, value);
            return true;
        }

        bool remove(const QString &account, QString *error) override
        {
            if (!_available || _failRemoves) {
                if (error)
                    *error = QStringLiteral("simulated remove failure");
                return false;
            }
            _items.remove(account);
            return true;
        }

        void setAvailable(bool available) { _available = available; }
        void setFailWrites(bool fail) { _failWrites = fail; }
        void setFailRemoves(bool fail) { _failRemoves = fail; }
        const QHash<QString, QString> &items() const { return _items; }

    private:
        bool _available;
        bool _failWrites = false;
        bool _failRemoves = false;
        QHash<QString, QString> _items;
    };

    struct ScopedConfigDir
    {
        QTemporaryDir dir;
        QByteArray prev;
        bool hadPrev = false;

        ScopedConfigDir()
            : prev(qgetenv("RAGOSHELL_CONFIG_DIR")),
              hadPrev(qEnvironmentVariableIsSet("RAGOSHELL_CONFIG_DIR"))
        {
            EXPECT_TRUE(dir.isValid());
            qputenv("RAGOSHELL_CONFIG_DIR", dir.path().toUtf8());
        }

        ~ScopedConfigDir()
        {
            if (hadPrev)
                qputenv("RAGOSHELL_CONFIG_DIR", prev);
            else
                qunsetenv("RAGOSHELL_CONFIG_DIR");
        }
    };
}

// The hosted "RagoShell AI" provider was removed (docs: BYOK only). A saved
// choice must turn the assistant off, never fall back to Anthropic (which
// would use a personal ANTHROPIC_API_KEY from the environment unasked).
using AiMigration = RagoShell::SettingsManager::AiProviderMigration;

TEST(SettingsManager_AiProviderMigration, RemovedHostedProvider_TurnsAssistantOff)
{
    QString provider = QStringLiteral("ragoshell_cloud");
    bool enabled = true;
    EXPECT_EQ(RagoShell::SettingsManager::migrateAiProvider(provider, enabled), AiMigration::HostedRemoved);
    EXPECT_TRUE(provider.isEmpty());
    EXPECT_FALSE(enabled);

    // Idempotent: a second pass finds nothing to do.
    EXPECT_EQ(RagoShell::SettingsManager::migrateAiProvider(provider, enabled), AiMigration::None);
    EXPECT_FALSE(enabled);

    QString padded = QStringLiteral(" ragoshell_cloud ");
    bool on = true;
    EXPECT_EQ(RagoShell::SettingsManager::migrateAiProvider(padded, on), AiMigration::HostedRemoved);
    EXPECT_FALSE(on);
}

// Any other unsupported id (hand edit, settings from a newer build) must not
// fall back to Anthropic with the assistant still on either.
TEST(SettingsManager_AiProviderMigration, UnknownProvider_TurnsAssistantOff)
{
    for (const QString &id : {QStringLiteral("bogus"), QStringLiteral("mistral"), QStringLiteral("Anthropic")}) {
        QString provider = id;
        bool enabled = true;
        EXPECT_EQ(RagoShell::SettingsManager::migrateAiProvider(provider, enabled), AiMigration::Unknown)
            << id.toStdString();
        EXPECT_TRUE(provider.isEmpty());
        EXPECT_FALSE(enabled);
    }
}

TEST(SettingsManager_AiProviderMigration, SupportedOrDefaultProvider_IsLeftAlone)
{
    for (const QString &id : {QStringLiteral("anthropic"), QStringLiteral("gemini"), QStringLiteral("openai_compat"),
                              QStringLiteral("ollama"), QString()}) {
        QString provider = id;
        bool enabled = true;
        EXPECT_EQ(RagoShell::SettingsManager::migrateAiProvider(provider, enabled), AiMigration::None)
            << id.toStdString();
        EXPECT_EQ(provider, id);
        EXPECT_TRUE(enabled);
    }
}

// docs: T1. A key typed for one server is never sent to another one.
TEST(SettingsManager_AiKeyOrigin, KeyFollowsTheOriginItWasSavedFor)
{
    ScopedConfigDir cfg;
    auto store = std::make_shared<FakeAiKeyStore>(true);
    RagoShell::SettingsManager s(store);
    const QString p = QStringLiteral("openai_compat");
    s.setAiBaseUrl(p, QStringLiteral("https://api.openai.com/v1"));
    s.setAiApiKey(p, QStringLiteral("sk-openai"));
    EXPECT_EQ(s.aiApiKey(p), QStringLiteral("sk-openai"));

    // Same origin, other path: the key stays.
    s.setAiBaseUrl(p, QStringLiteral("https://api.openai.com/v2"));
    EXPECT_EQ(s.aiApiKey(p), QStringLiteral("sk-openai"));
    // Explicit default port is the same origin.
    s.setAiBaseUrl(p, QStringLiteral("https://api.openai.com:443/v1"));
    EXPECT_EQ(s.aiApiKey(p), QStringLiteral("sk-openai"));

    // Another host, port or scheme: the key is forgotten, not reused.
    s.setAiBaseUrl(p, QStringLiteral("https://evil.example/v1"));
    EXPECT_TRUE(s.aiApiKey(p).isEmpty());
    s.setAiBaseUrl(p, QStringLiteral("https://api.openai.com/v1"));
    EXPECT_TRUE(s.aiApiKey(p).isEmpty());

    s.setAiApiKey(p, QStringLiteral("sk-2"));
    s.setAiBaseUrl(p, QStringLiteral("https://api.openai.com:8443/v1"));
    EXPECT_TRUE(s.aiApiKey(p).isEmpty());
    s.setAiBaseUrl(p, QStringLiteral("http://localhost:1234/v1"));
    s.setAiApiKey(p, QStringLiteral("sk-local"));
    s.setAiBaseUrl(p, QStringLiteral("https://localhost:1234/v1"));
    EXPECT_TRUE(s.aiApiKey(p).isEmpty());

    // Fixed-address providers are not affected.
    s.setAiApiKey(QStringLiteral("anthropic"), QStringLiteral("sk-ant"));
    s.setAiBaseUrl(QStringLiteral("anthropic"), QStringLiteral("https://evil.example"));
    EXPECT_EQ(s.aiApiKey(QStringLiteral("anthropic")), QStringLiteral("sk-ant"));
    EXPECT_TRUE(s.aiBaseUrl(QStringLiteral("anthropic")).isEmpty());
}

// docs: B7. Ollama keeps no key, starts at localhost and has its own time limit.
TEST(SettingsManager_AiOllama, NoKeyDefaultAddressAndTimeLimit)
{
    ScopedConfigDir cfg;
    auto store = std::make_shared<FakeAiKeyStore>(true);
    RagoShell::SettingsManager s(store);
    const QString o = QStringLiteral("ollama");
    EXPECT_EQ(s.aiBaseUrl(o), QStringLiteral("http://localhost:11434/v1"));
    EXPECT_TRUE(s.aiBaseUrl(QStringLiteral("openai_compat")).isEmpty());

    s.setAiApiKey(o, QStringLiteral("sk-never-stored"));
    EXPECT_TRUE(s.aiApiKey(o).isEmpty());

    // A test run before the first save keeps its list.
    s.setAiAvailableModels(o, {QStringLiteral("llama3.2:latest")});
    s.setAiBaseUrl(o, QStringLiteral("http://localhost:11434/v1"));
    EXPECT_EQ(s.aiAvailableModels(o), QStringList{QStringLiteral("llama3.2:latest")});
    // Another server: the list belonged to the previous one.
    s.setAiBaseUrl(o, QStringLiteral("http://192.168.1.20:11434/v1"));
    EXPECT_TRUE(s.aiAvailableModels(o).isEmpty());
    EXPECT_EQ(s.aiBaseUrl(o), QStringLiteral("http://192.168.1.20:11434/v1"));

    EXPECT_EQ(s.aiTimeoutSec(o), 180);
    EXPECT_EQ(s.aiTimeoutSec(QStringLiteral("openai_compat")), 120);
    s.setAiTimeoutSec(o, 240);
    EXPECT_EQ(s.aiTimeoutSec(o), 240);
    s.setAiTimeoutSec(o, 1);
    EXPECT_EQ(s.aiTimeoutSec(o), 30);
    s.setAiTimeoutSec(o, 9999);
    EXPECT_EQ(s.aiTimeoutSec(o), 300);
    // Fixed-address providers keep the built-in limit.
    s.setAiTimeoutSec(QStringLiteral("anthropic"), 300);
    EXPECT_EQ(s.aiTimeoutSec(QStringLiteral("anthropic")), 120);
}

TEST(SettingsManager_SchemaMigrationTests, CurrentSchema_ReturnsFalseAndLeavesMapUntouched)
{
    QVariantMap config;
    config["version"] = RagoShell::SettingsManager::schemaVersion();
    config["uiTheme"] = QStringLiteral("Light");
    config["batchSize"] = 120;
    config["customUserField"] = QStringLiteral("keep-me");

    QVariantMap conn;
    conn["uuid"] = QStringLiteral("11111111-2222-3333-4444-555555555555");
    conn["connectionName"] = QStringLiteral("Prod Cluster");
    config["connections"] = QVariantList{ conn };

    const QVariantMap snapshotBefore = config;
    QString previousSchema;
    const bool migrated =
        RagoShell::SettingsManager::migrateMapToCurrentSchema(config, &previousSchema);

    EXPECT_FALSE(migrated);
    EXPECT_EQ(RagoShell::SettingsManager::schemaVersion(), previousSchema);
    EXPECT_EQ(snapshotBefore, config);
}

TEST(SettingsManager_SchemaMigrationTests, LegacySchema_NormalizesModelEncryptsSecretsAndIsIdempotent)
{
    QVariantMap legacyCred;
    legacyCred["databaseName"] = QStringLiteral("admin");
    legacyCred["userName"] = QStringLiteral("dbAdmin");
    legacyCred["userPassword"] = QStringLiteral("s3cr3t!db");

    QVariantMap legacySsh;
    legacySsh["host"] = QStringLiteral("bastion.internal");
    legacySsh["userPassword"] = QStringLiteral("sshPass#42");
    legacySsh["passphrase"] = QStringLiteral("keyPhrase$99");

    QVariantMap legacyConn;
    legacyConn["connectionName"] = QStringLiteral("Legacy 1.x Server");
    legacyConn["serverHost"] = QStringLiteral("localhost");
    legacyConn["serverPort"] = 27017;
    legacyConn["sslEnabled"] = true;
    legacyConn["sslPemKeyFile"] = QStringLiteral("C:/certs/client.pem");
    legacyConn["ssh"] = legacySsh;
    legacyConn["credentials"] = QVariantList{ legacyCred };

    QVariantMap config;
    config["version"] = QStringLiteral("1.0");
    config["style"] = QStringLiteral("Ocean Dark");
    config["batchSize"] = 0;
    config["uuidEncoding"] = 99; // out-of-range -> clamped to 0
    config["connections"] = QVariantList{ legacyConn };

    QString previousSchema;
    const bool migrated =
        RagoShell::SettingsManager::migrateMapToCurrentSchema(config, &previousSchema);

    EXPECT_TRUE(migrated);
    EXPECT_EQ(QStringLiteral("1.0"), previousSchema);
    EXPECT_EQ(RagoShell::SettingsManager::schemaVersion(), config.value("version").toString());
    EXPECT_EQ(QStringLiteral("Dark"), config.value("uiTheme").toString());
    EXPECT_EQ(50, config.value("batchSize").toInt());
    EXPECT_EQ(0, config.value("uuidEncoding").toInt());
    EXPECT_TRUE(config.value("autoExpand").toBool());
    EXPECT_TRUE(config.value("autoExec").toBool());

    const QVariantList conns = config.value("connections").toList();
    ASSERT_EQ(1, conns.size());
    const QVariantMap migratedConn = conns.first().toMap();

    EXPECT_FALSE(migratedConn.value("uuid").toString().isEmpty());
    EXPECT_FALSE(migratedConn.contains("sslEnabled"));
    EXPECT_FALSE(migratedConn.contains("sslPemKeyFile"));

    const QVariantMap ssl = migratedConn.value("ssl").toMap();
    EXPECT_TRUE(ssl.value("sslEnabled").toBool());
    EXPECT_TRUE(ssl.value("usePemFile").toBool());
    EXPECT_EQ(QStringLiteral("C:/certs/client.pem"), ssl.value("pemKeyFile").toString());

    const QVariantMap ssh = migratedConn.value("ssh").toMap();
    EXPECT_FALSE(ssh.contains("userPassword"));
    EXPECT_FALSE(ssh.contains("passphrase"));
    EXPECT_EQ("sshPass#42", decryptField(ssh, QStringLiteral("userPasswordEncrypted")));
    EXPECT_EQ("keyPhrase$99", decryptField(ssh, QStringLiteral("passphraseEncrypted")));

    const QVariantList creds = migratedConn.value("credentials").toList();
    ASSERT_EQ(1, creds.size());
    const QVariantMap cred = creds.first().toMap();
    EXPECT_FALSE(cred.contains("userPassword"));
    EXPECT_EQ("s3cr3t!db", decryptField(cred, QStringLiteral("userPasswordEncrypted")));

    // Second run with unchanged schema version must be a no-op
    const QVariantMap afterFirstMigration = config;
    QString secondPrevious;
    EXPECT_FALSE(RagoShell::SettingsManager::migrateMapToCurrentSchema(config, &secondPrevious));
    EXPECT_EQ(RagoShell::SettingsManager::schemaVersion(), secondPrevious);
    EXPECT_EQ(afterFirstMigration, config);
}

TEST(SettingsManager_AiKeyStore, AiKeysStoredInKeyStoreNeverInJson)
{
    ScopedConfigDir cfg;
    auto store = std::make_shared<FakeAiKeyStore>(true);
    const QString cfgPath = cfg.dir.filePath(QStringLiteral("ragoshell.json"));

    {
        RagoShell::SettingsManager s(store);
        s.setAiApiKey(QStringLiteral("anthropic"), QStringLiteral("sk-ant-secret"));
        s.setAiApiKey(QStringLiteral("gemini"), QStringLiteral("AIza-secret-gem"));
        s.setAiBaseUrl(QStringLiteral("openai_compat"), QStringLiteral("https://api.openai.com/v1"));
        s.setAiApiKey(QStringLiteral("openai_compat"), QStringLiteral("sk-oai-secret"));
        ASSERT_TRUE(s.save());
    }

    QFile f(cfgPath);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray jsonBytes = f.readAll();
    f.close();
    EXPECT_FALSE(jsonBytes.contains("apiKeyEncrypted"));
    EXPECT_FALSE(jsonBytes.contains("ApiKeyEncrypted"));
    EXPECT_FALSE(jsonBytes.contains("sk-ant-secret"));
    EXPECT_FALSE(jsonBytes.contains("AIza-secret-gem"));
    EXPECT_FALSE(jsonBytes.contains("sk-oai-secret"));
    EXPECT_EQ(store->items().size(), 3);

    RagoShell::SettingsManager reloaded(store);
    EXPECT_EQ(reloaded.aiApiKey(QStringLiteral("anthropic")), QStringLiteral("sk-ant-secret"));
    EXPECT_EQ(reloaded.aiApiKey(QStringLiteral("gemini")), QStringLiteral("AIza-secret-gem"));
    EXPECT_EQ(reloaded.aiApiKey(QStringLiteral("openai_compat")), QStringLiteral("sk-oai-secret"));

    // Changing origin forgets the key and removes its KeyStore slot on save().
    reloaded.setAiBaseUrl(QStringLiteral("openai_compat"), QStringLiteral("https://evil.example/v1"));
    EXPECT_TRUE(reloaded.aiApiKey(QStringLiteral("openai_compat")).isEmpty());
    store->setFailRemoves(true);
    ASSERT_TRUE(reloaded.save());
    EXPECT_EQ(store->items().size(), 3); // failed remove leaves prior slot intact rather than ignoring error
    store->setFailRemoves(false);
    ASSERT_TRUE(reloaded.save());
    EXPECT_EQ(store->items().size(), 2);

    RagoShell::SettingsManager afterMove(store);
    EXPECT_TRUE(afterMove.aiApiKey(QStringLiteral("openai_compat")).isEmpty());
}

TEST(SettingsManager_AiKeyStore, LegacyEncryptedAiKeysMigrateToKeyStoreAndScrubJson)
{
    ScopedConfigDir cfg;
    RagoShell::RagoCrypt::initKey();
    const QString cfgPath = cfg.dir.filePath(QStringLiteral("ragoshell.json"));

    const QString encAnt = QString::fromStdString(RagoShell::RagoCrypt::encrypt("sk-ant-legacy"));
    const QString encOai = QString::fromStdString(
        RagoShell::RagoCrypt::encrypt("https://api.openai.com:443\nsk-oai-legacy"));
    const QByteArray legacyJson = QStringLiteral(
        "{\n"
        "  \"version\": \"2.0\",\n"
        "  \"aiAssistant\": {\n"
        "    \"enabled\": true,\n"
        "    \"provider\": \"openai_compat\",\n"
        "    \"apiKeyEncrypted\": \"%1\",\n"
        "    \"openai_compatBaseUrl\": \"https://api.openai.com/v1\",\n"
        "    \"openai_compatApiKeyEncrypted\": \"%2\"\n"
        "  }\n"
        "}\n").arg(encAnt, encOai).toUtf8();
    {
        QFile f(cfgPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(f.write(legacyJson), legacyJson.size());
    }

    auto store = std::make_shared<FakeAiKeyStore>(true);
    RagoShell::SettingsManager migrated(store);
    EXPECT_EQ(migrated.aiApiKey(QStringLiteral("anthropic")), QStringLiteral("sk-ant-legacy"));
    EXPECT_EQ(migrated.aiApiKey(QStringLiteral("openai_compat")), QStringLiteral("sk-oai-legacy"));
    EXPECT_EQ(store->items().size(), 2);

    QFile scrubbed(cfgPath);
    ASSERT_TRUE(scrubbed.open(QIODevice::ReadOnly));
    const QByteArray updatedBytes = scrubbed.readAll();
    scrubbed.close();
    EXPECT_FALSE(updatedBytes.contains("apiKeyEncrypted"));
    EXPECT_FALSE(updatedBytes.contains("ApiKeyEncrypted"));
    EXPECT_FALSE(updatedBytes.contains("sk-ant-legacy"));
    EXPECT_FALSE(updatedBytes.contains("sk-oai-legacy"));

    RagoShell::SettingsManager nextStart(store);
    EXPECT_EQ(nextStart.aiApiKey(QStringLiteral("anthropic")), QStringLiteral("sk-ant-legacy"));
    EXPECT_EQ(nextStart.aiApiKey(QStringLiteral("openai_compat")), QStringLiteral("sk-oai-legacy"));
}

TEST(SettingsManager_AiKeyStore, UnavailableKeyStoreKeepsAiKeysInSessionMemoryOnly)
{
    ScopedConfigDir cfg;
    const QString cfgPath = cfg.dir.filePath(QStringLiteral("ragoshell.json"));

    {
        RagoShell::SettingsManager s(nullptr);
        s.setAiApiKey(QStringLiteral("anthropic"), QStringLiteral("sk-session-only"));
        ASSERT_TRUE(s.save());
        EXPECT_EQ(s.aiApiKey(QStringLiteral("anthropic")), QStringLiteral("sk-session-only"));
    }

    QFile f(cfgPath);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray jsonBytes = f.readAll();
    f.close();
    EXPECT_FALSE(jsonBytes.contains("apiKeyEncrypted"));
    EXPECT_FALSE(jsonBytes.contains("sk-session-only"));

    RagoShell::SettingsManager reloaded(nullptr);
    EXPECT_TRUE(reloaded.aiApiKey(QStringLiteral("anthropic")).isEmpty());
}

TEST(SettingsManager_AiKeyStore, ScopedConfigDirIsolatesAiKeyStoreSlotsBetweenProfiles)
{
    auto sharedStore = std::make_shared<FakeAiKeyStore>(true);
    {
        ScopedConfigDir cfgA;
        RagoShell::SettingsManager profileA(sharedStore);
        profileA.setAiApiKey(QStringLiteral("anthropic"), QStringLiteral("sk-profile-a"));
        ASSERT_TRUE(profileA.save());
    }
    {
        ScopedConfigDir cfgB;
        RagoShell::SettingsManager profileB(sharedStore);
        EXPECT_TRUE(profileB.aiApiKey(QStringLiteral("anthropic")).isEmpty());
        profileB.setAiApiKey(QStringLiteral("anthropic"), QStringLiteral("sk-profile-b"));
        ASSERT_TRUE(profileB.save());
        RagoShell::SettingsManager reloadB(sharedStore);
        EXPECT_EQ(reloadB.aiApiKey(QStringLiteral("anthropic")), QStringLiteral("sk-profile-b"));
    }
    EXPECT_EQ(sharedStore->items().size(), 2);
}

TEST(SettingsManager_AiKeyStore, LegacyAiKeysPreservedWhenUnavailableOrWriteFailsAndScrubbedInSchemaBackup)
{
    ScopedConfigDir cfg;
    RagoShell::RagoCrypt::initKey();
    const QString cfgPath = cfg.dir.filePath(QStringLiteral("ragoshell.json"));
    const QString bakPath = cfgPath + QStringLiteral(".bak-1.0");
    const QString encAnt = QString::fromStdString(RagoShell::RagoCrypt::encrypt("sk-keep-me"));
    const QByteArray legacyV1 = QStringLiteral(
        "{\n"
        "  \"version\": \"1.0\",\n"
        "  \"aiAssistant\": {\n"
        "    \"enabled\": true,\n"
        "    \"provider\": \"anthropic\",\n"
        "    \"apiKeyEncrypted\": \"%1\"\n"
        "  }\n"
        "}\n").arg(encAnt).toUtf8();
    {
        QFile f(cfgPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(f.write(legacyV1), legacyV1.size());
    }

    // 1. KeyStore unavailable: legacy key stays usable in RAM and is NOT deleted from disk.
    auto store = std::make_shared<FakeAiKeyStore>(false);
    {
        RagoShell::SettingsManager offline(store);
        EXPECT_EQ(offline.aiApiKey(QStringLiteral("anthropic")), QStringLiteral("sk-keep-me"));
    }
    {
        QFile f(cfgPath);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        EXPECT_TRUE(f.readAll().contains("apiKeyEncrypted"));
    }

    // 2. KeyStore available but write fails: legacy key is still preserved on disk.
    store->setAvailable(true);
    store->setFailWrites(true);
    {
        RagoShell::SettingsManager failing(store);
        EXPECT_EQ(failing.aiApiKey(QStringLiteral("anthropic")), QStringLiteral("sk-keep-me"));
    }
    {
        QFile f(cfgPath);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        EXPECT_TRUE(f.readAll().contains("apiKeyEncrypted"));
    }

    // 3. KeyStore succeeds: schema migration + key migration run and scrub both main config and .bak-1.0.
    store->setFailWrites(false);
    {
        RagoShell::SettingsManager okMgr(store);
        EXPECT_EQ(okMgr.aiApiKey(QStringLiteral("anthropic")), QStringLiteral("sk-keep-me"));
        EXPECT_EQ(store->items().size(), 1);
    }
    for (const QString &p : {cfgPath, bakPath}) {
        QFile checkFile(p);
        ASSERT_TRUE(checkFile.open(QIODevice::ReadOnly)) << p.toStdString();
        const QByteArray bytes = checkFile.readAll();
        EXPECT_FALSE(bytes.contains("apiKeyEncrypted")) << p.toStdString();
        EXPECT_FALSE(bytes.contains("sk-keep-me")) << p.toStdString();
    }
}

namespace
{
    struct ScopedPolicyPath
    {
        explicit ScopedPolicyPath(const QString &path)
        {
            RagoShell::SettingsManager::setPolicyFilePathForTest(path);
        }
        ~ScopedPolicyPath()
        {
            RagoShell::SettingsManager::clearPolicyFilePathForTest();
        }
    };

    QString generateTestP256Ring(const QString &kid)
    {
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey(
            EVP_EC_gen("prime256v1"), &EVP_PKEY_free);
        if (!pkey)
            return {};
        unsigned char *der = nullptr;
        const int len = i2d_PUBKEY(pkey.get(), &der);
        if (len <= 0 || !der)
            return {};
        const QByteArray b64 = QByteArray(reinterpret_cast<const char *>(der), len).toBase64();
        OPENSSL_free(der);
        return kid + QLatin1Char(':') + QString::fromLatin1(b64);
    }

    bool writeRawFile(const QString &path, const QByteArray &data)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        const bool ok = f.write(data) == data.size();
        f.close();
        return ok;
    }
}

TEST(SettingsManager_EnterpriseServer, UserCloudServerConfigRoundTripAndClear)
{
    ScopedConfigDir cfg;
    ScopedPolicyPath noPolicy{QString()};
    auto store = std::make_shared<FakeAiKeyStore>(true);
    const QString ring = generateTestP256Ring(QStringLiteral("2026-11"));
    ASSERT_FALSE(ring.isEmpty());

    {
        RagoShell::SettingsManager s(store);
        EXPECT_FALSE(s.hasUserCloudServerConfig());
        EXPECT_FALSE(s.hasEnterprisePolicy());
        EXPECT_FALSE(s.isEnterprisePolicyInvalid());

        RagoShell::SettingsManager::CloudServerConfig cs;
        cs.mode = QStringLiteral("custom");
        cs.apiUrl = QStringLiteral("https://api.corp.example/");
        cs.portalUrl = QStringLiteral("https://app.corp.example/");
        cs.bridgeUrl = QStringLiteral("wss://bridge.corp.example/ws/");
        cs.entitlementPubkeys = ring;
        s.setUserCloudServerConfig(cs);
        ASSERT_TRUE(s.save());
    }

    {
        RagoShell::SettingsManager reloaded(store);
        ASSERT_TRUE(reloaded.hasUserCloudServerConfig());
        const auto got = reloaded.userCloudServerConfig();
        EXPECT_EQ(got.mode, QStringLiteral("custom"));
        EXPECT_EQ(got.apiUrl, QStringLiteral("https://api.corp.example"));
        EXPECT_EQ(got.portalUrl, QStringLiteral("https://app.corp.example"));
        EXPECT_EQ(got.bridgeUrl, QStringLiteral("wss://bridge.corp.example/ws"));
        EXPECT_EQ(got.entitlementPubkeys, ring);

        reloaded.clearUserCloudServerConfig();
        ASSERT_TRUE(reloaded.save());
    }

    RagoShell::SettingsManager cleared(store);
    EXPECT_FALSE(cleared.hasUserCloudServerConfig());
}

TEST(SettingsManager_EnterpriseServer, ValidLockedAndUnlockedPoliciesAndLegacyCheckForUpdates)
{
    ScopedConfigDir cfg;
    QTemporaryDir polDir;
    ASSERT_TRUE(polDir.isValid());
    const QString policyPath = polDir.filePath(QStringLiteral("policy.json"));
    ScopedPolicyPath usePolicy(policyPath);
    auto store = std::make_shared<FakeAiKeyStore>(true);
    const QString ring = generateTestP256Ring(QStringLiteral("corp-1"));
    ASSERT_FALSE(ring.isEmpty());

    // 1. Locked policy loads even on first launch before ragoshell.json exists.
    ASSERT_TRUE(writeRawFile(policyPath, QStringLiteral(
        "{\n"
        "  \"locked\": true,\n"
        "  \"disableHttpsFeatures\": true,\n"
        "  \"checkForUpdates\": false,\n"
        "  \"updateUrl\": \"https://updates.corp.example/v1/release-check\",\n"
        "  \"cloudServer\": {\n"
        "    \"mode\": \"custom\",\n"
        "    \"apiUrl\": \"https://api.corp.example\",\n"
        "    \"portalUrl\": \"https://app.corp.example\",\n"
        "    \"bridgeUrl\": \"wss://bridge.corp.example/ws\",\n"
        "    \"entitlementPubkeys\": \"%1\"\n"
        "  }\n"
        "}\n").arg(ring).toUtf8()));

    {
        RagoShell::SettingsManager locked(store);
        EXPECT_TRUE(locked.hasEnterprisePolicy());
        EXPECT_TRUE(locked.isEnterprisePolicyLocked());
        EXPECT_FALSE(locked.isEnterprisePolicyInvalid());
        EXPECT_TRUE(locked.isCloudServerPolicyLocked());
        EXPECT_TRUE(locked.isCheckForUpdatesLocked());
        EXPECT_TRUE(locked.disableHttpsFeatures());
        EXPECT_FALSE(locked.checkForUpdates());
        EXPECT_EQ(locked.policyUpdateUrl(), QStringLiteral("https://updates.corp.example/v1/release-check"));

        // Locked setters are ignored.
        locked.setCheckForUpdates(true);
        EXPECT_FALSE(locked.checkForUpdates());
        RagoShell::SettingsManager::CloudServerConfig rogue;
        rogue.mode = QStringLiteral("custom");
        rogue.apiUrl = QStringLiteral("https://evil.example");
        locked.setUserCloudServerConfig(rogue);
        EXPECT_FALSE(locked.hasUserCloudServerConfig());
    }

    // 2. Unlocked policy: auto-saved legacy checkForUpdates=true must NOT override unlocked policy false,
    //    whereas an explicit user setCheckForUpdates(true) DOES override unlocked policy false.
    ASSERT_TRUE(writeRawFile(cfg.dir.filePath(QStringLiteral("ragoshell.json")),
                             "{\n  \"version\": \"2.0\",\n  \"checkForUpdates\": true\n}\n"));
    ASSERT_TRUE(writeRawFile(policyPath,
                             "{\n  \"locked\": false,\n  \"checkForUpdates\": false\n}\n"));
    {
        RagoShell::SettingsManager unlocked(store);
        EXPECT_TRUE(unlocked.hasEnterprisePolicy());
        EXPECT_FALSE(unlocked.isEnterprisePolicyLocked());
        EXPECT_FALSE(unlocked.isCheckForUpdatesLocked());
        EXPECT_FALSE(unlocked.checkForUpdates());

        unlocked.setCheckForUpdates(true);
        ASSERT_TRUE(unlocked.save());
    }
    {
        RagoShell::SettingsManager userWon(store);
        EXPECT_TRUE(userWon.checkForUpdates());
    }
}

TEST(SettingsManager_EnterpriseServer, InvalidPolicyFailsClosedWithoutFallback)
{
    ScopedConfigDir cfg;
    QTemporaryDir polDir;
    ASSERT_TRUE(polDir.isValid());
    const QString policyPath = polDir.filePath(QStringLiteral("policy.json"));
    ScopedPolicyPath usePolicy(policyPath);
    auto store = std::make_shared<FakeAiKeyStore>(true);
    const QString ring = generateTestP256Ring(QStringLiteral("corp-1"));
    ASSERT_FALSE(ring.isEmpty());

    const QList<QByteArray> invalidPayloads = {
        QByteArrayLiteral("{ not valid json"),
        QByteArrayLiteral("{\"locked\": \"false\"}"),
        QByteArrayLiteral("{\"updateUrl\": \"http://insecure.corp.example/v1/release-check\"}"),
        QStringLiteral("{\"cloudServer\":{\"mode\":\"custom\",\"apiUrl\":\"http://192.168.1.10\",\"portalUrl\":\"https://app.corp.example\",\"entitlementPubkeys\":\"%1\"}}").arg(ring).toUtf8(),
        QStringLiteral("{\"cloudServer\":{\"mode\":\"custom\",\"apiUrl\":\"https://user:pw@api.corp.example\",\"portalUrl\":\"https://app.corp.example\",\"entitlementPubkeys\":\"%1\"}}").arg(ring).toUtf8(),
        QStringLiteral("{\"cloudServer\":{\"mode\":\"custom\",\"apiUrl\":\"https://api.corp.example/v1\",\"portalUrl\":\"https://app.corp.example\",\"entitlementPubkeys\":\"%1\"}}").arg(ring).toUtf8(),
        QByteArrayLiteral("{\"cloudServer\":{\"mode\":\"custom\",\"apiUrl\":\"https://api.corp.example\",\"portalUrl\":\"https://app.corp.example\",\"entitlementPubkeys\":\"\"}}"),
        QByteArray(70 * 1024, 'x'),
    };

    for (const QByteArray &payload : invalidPayloads) {
        ASSERT_TRUE(writeRawFile(policyPath, payload));
        RagoShell::SettingsManager s(store);
        EXPECT_TRUE(s.hasEnterprisePolicy());
        EXPECT_TRUE(s.isEnterprisePolicyInvalid());
        EXPECT_TRUE(s.isCloudServerPolicyLocked());
        EXPECT_TRUE(s.isCheckForUpdatesLocked());
        EXPECT_FALSE(s.checkForUpdates());
        EXPECT_TRUE(s.policyUpdateUrl().isEmpty());
    }

    // Non-regular path (directory) must also fail closed.
    ASSERT_TRUE(QFile::remove(policyPath));
    ASSERT_TRUE(QDir().mkpath(policyPath));
    {
        RagoShell::SettingsManager dirPolicy(store);
        EXPECT_TRUE(dirPolicy.hasEnterprisePolicy());
        EXPECT_TRUE(dirPolicy.isEnterprisePolicyInvalid());
    }
    ASSERT_TRUE(QDir().rmdir(policyPath));

    // Symlink to valid policy file must be rejected if OS permits symlink creation.
    const QString realTarget = polDir.filePath(QStringLiteral("real_policy.json"));
    ASSERT_TRUE(writeRawFile(realTarget, QStringLiteral(
        "{\"locked\":true,\"cloudServer\":{\"mode\":\"custom\",\"apiUrl\":\"https://api.corp.example\",\"portalUrl\":\"https://app.corp.example\",\"entitlementPubkeys\":\"%1\"}}")
                                             .arg(ring).toUtf8()));
    if (QFile::link(realTarget, policyPath)) {
        RagoShell::SettingsManager linkPolicy(store);
        EXPECT_TRUE(linkPolicy.hasEnterprisePolicy());
        EXPECT_TRUE(linkPolicy.isEnterprisePolicyInvalid());
        QFile::remove(policyPath);
    }
}
