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
