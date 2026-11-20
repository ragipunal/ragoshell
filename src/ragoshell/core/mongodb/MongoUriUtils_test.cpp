#include <gtest/gtest.h>

#include <bsoncxx/json.hpp>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/domain/MongoUser.h"
#include "ragoshell/core/mongodb/MongoUriUtils.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/settings/ReplicaSetSettings.h"
#include "ragoshell/core/settings/SslSettings.h"

using namespace RagoShell;

TEST(MongoUriUtilsTest, SupportedMechanismsStartWithDefaultAutoNegotiation)
{
    const QStringList mechs = supportedAuthMechanisms();
    ASSERT_FALSE(mechs.isEmpty());
    EXPECT_EQ(mechs.first(), QStringLiteral("DEFAULT"));
    EXPECT_TRUE(mechs.contains(QStringLiteral("SCRAM-SHA-256")));
    EXPECT_TRUE(mechs.contains(QStringLiteral("SCRAM-SHA-1")));
    EXPECT_TRUE(mechs.contains(QStringLiteral("MONGODB-X509")));
}

TEST(MongoUriUtilsTest, ClientNameStartsWithTheProductId)
{
    const std::string prefix = QString::fromLatin1(BrandInfo::productId).toLower().toStdString() + "-";
    EXPECT_EQ(prefix, "ragoshell-");
    EXPECT_EQ(clientAppName("shell-1a2b"), prefix + "shell-1a2b");

    ConnectionSettings conn(/*isClone=*/false);
    conn.setServerHost("127.0.0.1");
    conn.setServerPort(37070);
    const std::string uri = buildMongoUri(conn);
    EXPECT_NE(uri.find("appname=" + prefix + PROJECT_VERSION), std::string::npos) << uri;
}

TEST(MongoUriUtilsTest, DefaultMechanismOmitsAuthMechanismParamForAutoNegotiation)
{
    ConnectionSettings conn(/*isClone=*/false);
    conn.setServerHost("127.0.0.1");
    conn.setServerPort(37070);

    auto *cred = new CredentialSettings();
    cred->setEnabled(true);
    cred->setUserName("rago_sha256");
    cred->setUserPassword("p@ss:w/rd#1?");
    cred->setDatabaseName("admin");
    // Leave mechanism unset -> defaults to "DEFAULT"
    conn.addCredential(cred);

    EXPECT_EQ(cred->mechanism(), "DEFAULT");
    const std::string uri = buildMongoUri(conn);
    EXPECT_NE(uri.find("mongodb://rago_sha256:p%40ss%3Aw%2Frd%231%3F@127.0.0.1:37070/?"), std::string::npos);
    EXPECT_NE(uri.find("authSource=admin"), std::string::npos);
    EXPECT_EQ(uri.find("authMechanism="), std::string::npos);
}

TEST(MongoUriUtilsTest, ExplicitScramAndX509MechanismsBuildValidUris)
{
    {
        ConnectionSettings conn(/*isClone=*/false);
        conn.setServerHost("localhost");
        conn.setServerPort(27017);
        auto *cred = new CredentialSettings();
        cred->setEnabled(true);
        cred->setUserName("alice");
        cred->setUserPassword("secret");
        cred->setDatabaseName("appdb");
        cred->setMechanism("SCRAM-SHA-256");
        conn.addCredential(cred);

        const std::string uri = buildMongoUri(conn);
        EXPECT_NE(uri.find("authSource=appdb"), std::string::npos);
        EXPECT_NE(uri.find("authMechanism=SCRAM-SHA-256"), std::string::npos);
    }

    {
        ConnectionSettings conn(/*isClone=*/false);
        conn.setServerHost("mongo-mtls");
        conn.setServerPort(27017);
        conn.sslSettings()->enableSSL(true);
        conn.sslSettings()->setAllowInvalidHostnames(true);

        auto *cred = new CredentialSettings();
        cred->setEnabled(true);
        cred->setUserName("CN=rago-x509-client,OU=AppUsers,O=RagoShellClient,ST=CA,C=US");
        cred->setUserPassword("must_not_appear");
        cred->setMechanism("MONGODB-X509");
        conn.addCredential(cred);

        const std::string uri = buildMongoUri(conn);
        EXPECT_EQ(uri.find("must_not_appear"), std::string::npos);
        EXPECT_NE(uri.find("authSource=$external"), std::string::npos);
        EXPECT_NE(uri.find("authMechanism=MONGODB-X509"), std::string::npos);
        EXPECT_NE(uri.find("tls=true"), std::string::npos);
        EXPECT_NE(uri.find("tlsAllowInvalidHostnames=true"), std::string::npos);
    }

    {
        // Subject-less MONGODB-X509 (server extracts subject from client certificate)
        ConnectionSettings conn(/*isClone=*/false);
        conn.setServerHost("127.0.0.1");
        conn.setServerPort(37090);
        conn.sslSettings()->enableSSL(true);

        auto *cred = new CredentialSettings();
        cred->setEnabled(true);
        cred->setMechanism("MONGODB-X509");
        conn.addCredential(cred);

        const std::string uri = buildMongoUri(conn);
        EXPECT_NE(uri.find("mongodb://127.0.0.1:37090/?"), std::string::npos);
        EXPECT_NE(uri.find("authSource=$external"), std::string::npos);
        EXPECT_NE(uri.find("authMechanism=MONGODB-X509"), std::string::npos);
    }
}

TEST(MongoUriUtilsTest, ReplicaSetUriIncludesMembersSetNameAndReadPreference)
{
    ConnectionSettings conn(/*isClone=*/false);
    conn.setReplicaSet(true);
    conn.replicaSetSettings()->setMembers({ "127.0.0.1:37117", "127.0.0.1:37118", "127.0.0.1:37119" });
    conn.replicaSetSettings()->setCachedSetName("rs0");
    conn.replicaSetSettings()->setReadPreference(ReplicaSetSettings::ReadPreference::SECONDARY_PREFERRED);

    const std::string uri = buildMongoUri(conn);
    EXPECT_NE(uri.find("mongodb://127.0.0.1:37117,127.0.0.1:37118,127.0.0.1:37119/?"), std::string::npos);
    EXPECT_NE(uri.find("replicaSet=rs0"), std::string::npos);
    EXPECT_NE(uri.find("readPreference=secondaryPreferred"), std::string::npos);
}

TEST(MongoUserDomainTest, ParsesCrossDbRolesAndScramMechanismsFromUsersInfo)
{
    const auto doc = bsoncxx::from_json(R"({
        "_id": "admin.ops_lead",
        "user": "ops_lead",
        "db": "admin",
        "roles": [
            { "role": "clusterMonitor", "db": "admin" },
            { "role": "readWrite", "db": "appdb" },
            { "role": "read", "db": "tenant_a" }
        ],
        "mechanisms": [ "SCRAM-SHA-256" ]
    })");

    MongoUser user(7.0f, doc.view());
    EXPECT_EQ(user.name(), "ops_lead");
    EXPECT_EQ(user.userSource(), "admin");
    ASSERT_EQ(user.roles().size(), 3u);
    EXPECT_EQ(user.roles()[0], "clusterMonitor");
    EXPECT_EQ(user.roles()[1], "readWrite@appdb");
    EXPECT_EQ(user.roles()[2], "read@tenant_a");
    ASSERT_EQ(user.mechanisms().size(), 1u);
    EXPECT_EQ(user.mechanisms()[0], "SCRAM-SHA-256");

    const auto extDoc = bsoncxx::from_json(R"({
        "_id": "$external.CN=rago-x509-client",
        "user": "CN=rago-x509-client",
        "db": "$external",
        "roles": [
            { "role": "readWrite", "db": "appdb" },
            { "role": "clusterMonitor", "db": "admin" }
        ]
    })");

    MongoUser extUser(8.0f, extDoc.view());
    EXPECT_EQ(extUser.name(), "CN=rago-x509-client");
    EXPECT_EQ(extUser.userSource(), "$external");
    ASSERT_EQ(extUser.roles().size(), 2u);
    EXPECT_EQ(extUser.roles()[0], "readWrite@appdb");
    EXPECT_EQ(extUser.roles()[1], "clusterMonitor@admin");
    EXPECT_TRUE(extUser.mechanisms().empty());
}
