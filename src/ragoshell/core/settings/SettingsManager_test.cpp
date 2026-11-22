#include "gtest/gtest.h"

#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/utils/RagoCrypt.h"

#include <QVariantList>
#include <QVariantMap>

namespace
{
    std::string decryptField(const QVariantMap &map, const QString &key)
    {
        return RagoShell::RagoCrypt::decrypt(map.value(key).toString().toStdString());
    }
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
    RagoShell::SettingsManager s;
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
    RagoShell::SettingsManager s;
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
