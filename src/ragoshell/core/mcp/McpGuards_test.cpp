#include <gtest/gtest.h>

#include <QJsonDocument>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

#include "ragoshell/core/mcp/McpGuards.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/settings/SslSettings.h"

using namespace RagoShell;

namespace
{
    QString checkPipelineJson(const char *json)
    {
        // Wrap so from_json accepts a top-level array.
        const auto doc = bsoncxx::from_json(std::string("{\"p\": ") + json + "}");
        return McpGuards::checkPipeline(doc.view()["p"].get_array().value);
    }

    QString checkDocumentJson(const char *json)
    {
        const auto doc = bsoncxx::from_json(json);
        return McpGuards::checkDocument(doc.view());
    }
}

TEST(McpGuardsTest, accepts_read_only_pipelines)
{
    EXPECT_TRUE(checkPipelineJson("[]").isEmpty());
    EXPECT_TRUE(checkPipelineJson(R"([{"$match": {"a": {"$gt": 1}}}, {"$group": {"_id": "$b", "n": {"$sum": 1}}},
                                      {"$sort": {"n": -1}}, {"$project": {"out": "$_id"}}])").isEmpty());
    EXPECT_TRUE(checkPipelineJson(R"([{"$lookup": {"from": "x", "localField": "a", "foreignField": "b", "as": "j"}}])")
                    .isEmpty());
}

TEST(McpGuardsTest, rejects_out_and_merge_anywhere)
{
    EXPECT_FALSE(checkPipelineJson(R"([{"$match": {}}, {"$out": "copy"}])").isEmpty());
    EXPECT_FALSE(checkPipelineJson(R"([{"$merge": {"into": "copy"}}])").isEmpty());
    EXPECT_FALSE(checkPipelineJson(R"([{"$facet": {"a": [{"$out": "copy"}]}}])").isEmpty());
    EXPECT_FALSE(checkPipelineJson(R"([{"$lookup": {"from": "x", "pipeline": [{"$merge": "copy"}], "as": "j"}}])")
                     .isEmpty());
    EXPECT_FALSE(checkPipelineJson(R"([{"$unionWith": {"coll": "x", "pipeline": [{"$out": "y"}]}}])").isEmpty());
}

TEST(McpGuardsTest, rejects_server_side_javascript)
{
    EXPECT_FALSE(checkDocumentJson(R"({"$where": "this.a > 1"})").isEmpty());
    EXPECT_FALSE(checkDocumentJson(R"({"$expr": {"$function": {"body": "x", "args": [], "lang": "js"}}})").isEmpty());
    EXPECT_FALSE(checkPipelineJson(R"([{"$group": {"_id": 1, "x": {"$accumulator": {}}}}])").isEmpty());

    bsoncxx::builder::basic::document doc;
    doc.append(bsoncxx::builder::basic::kvp("a", bsoncxx::types::b_code{"function() {}"}));
    EXPECT_FALSE(McpGuards::checkDocument(doc.view()).isEmpty());

    EXPECT_TRUE(checkDocumentJson(R"({"a": {"$in": [1, 2]}, "b": "$where is just text"})").isEmpty());
}

TEST(McpGuardsTest, rejects_malformed_stages)
{
    EXPECT_FALSE(checkPipelineJson(R"([1])").isEmpty());
    EXPECT_FALSE(checkPipelineJson(R"([{"match": {}}])").isEmpty());
    EXPECT_FALSE(checkPipelineJson(R"([{"$match": {}, "$limit": 1}])").isEmpty());
    EXPECT_FALSE(checkPipelineJson(R"([{}])").isEmpty());
}

TEST(McpGuardsTest, validates_names)
{
    EXPECT_TRUE(McpGuards::checkDatabaseName("samples").isEmpty());
    EXPECT_FALSE(McpGuards::checkDatabaseName("").isEmpty());
    EXPECT_FALSE(McpGuards::checkDatabaseName("a.b").isEmpty());
    EXPECT_FALSE(McpGuards::checkDatabaseName("a/b").isEmpty());
    EXPECT_FALSE(McpGuards::checkDatabaseName(QString(65, 'x')).isEmpty());
    EXPECT_TRUE(McpGuards::checkCollectionName("restaurants.archive").isEmpty());
    EXPECT_FALSE(McpGuards::checkCollectionName("").isEmpty());
    EXPECT_FALSE(McpGuards::checkCollectionName("a$b").isEmpty());
}

TEST(McpGuardsTest, refuses_credential_collections)
{
    EXPECT_FALSE(McpGuards::checkCollectionName("system.users").isEmpty());
    EXPECT_FALSE(McpGuards::checkCollectionName("system.keys").isEmpty());
    EXPECT_TRUE(McpGuards::checkCollectionName("system.profile").isEmpty());
    EXPECT_TRUE(McpGuards::checkCollectionName("system.views").isEmpty());
    EXPECT_TRUE(McpGuards::checkCollectionName("users").isEmpty());

    EXPECT_FALSE(checkPipelineJson(R"([{"$lookup": {"from": "system.users", "localField": "a",
                                       "foreignField": "user", "as": "j"}}])").isEmpty());
    EXPECT_FALSE(checkPipelineJson(R"([{"$lookup": {"from": {"db": "admin", "coll": "system.keys"},
                                       "pipeline": [], "as": "j"}}])").isEmpty());
    EXPECT_FALSE(checkPipelineJson(R"([{"$graphLookup": {"from": "system.users", "startWith": "$a",
                                       "connectFromField": "a", "connectToField": "b", "as": "g"}}])").isEmpty());
    EXPECT_FALSE(checkPipelineJson(R"([{"$unionWith": "system.users"}])").isEmpty());
    EXPECT_FALSE(checkPipelineJson(R"([{"$unionWith": {"coll": "system.keys", "pipeline": []}}])").isEmpty());
    // Nested inside $facet / a $lookup sub-pipeline too.
    EXPECT_FALSE(checkPipelineJson(R"([{"$facet": {"a": [{"$unionWith": "system.users"}]}}])").isEmpty());
    EXPECT_FALSE(checkPipelineJson(R"([{"$lookup": {"from": "x", "as": "j",
                                       "pipeline": [{"$unionWith": "system.users"}]}}])").isEmpty());

    EXPECT_TRUE(checkPipelineJson(R"([{"$unionWith": "orders"}, {"$lookup": {"from": "users", "localField": "a",
                                      "foreignField": "b", "as": "j"}}])").isEmpty());
}

TEST(McpGuardsTest, unsupported_connections)
{
    ConnectionSettings plain(/*isClone=*/false);
    EXPECT_TRUE(McpGuards::unsupportedReason(plain).isEmpty());

    ConnectionSettings ssh(/*isClone=*/false);
    ssh.sshSettings()->setEnabled(true);
    EXPECT_FALSE(McpGuards::unsupportedReason(ssh).isEmpty());

    ConnectionSettings askPem(/*isClone=*/false);
    askPem.sslSettings()->enableSSL(true);
    askPem.sslSettings()->setUsePemFile(true);
    askPem.sslSettings()->setAskPassphrase(true);
    EXPECT_FALSE(McpGuards::unsupportedReason(askPem).isEmpty());

    // Self-signed server certificate with a client PEM (stored passphrase) is fine.
    ConnectionSettings tls(/*isClone=*/false);
    tls.sslSettings()->enableSSL(true);
    tls.sslSettings()->setUsePemFile(true);
    tls.sslSettings()->setPemKeyFile("C:/certs/client.pem");
    tls.sslSettings()->setAllowInvalidCertificates(true);
    tls.sslSettings()->setAllowInvalidHostnames(true);
    EXPECT_TRUE(McpGuards::unsupportedReason(tls).isEmpty());
}

TEST(McpGuardsTest, connection_summary_has_no_secrets)
{
    ConnectionSettings conn(/*isClone=*/false);
    conn.setConnectionName("Reporting");
    conn.setServerHost("db.example.test");
    conn.setServerPort(27018);
    conn.setUuid("uuid-1");
    auto *cred = new CredentialSettings();
    cred->setUserName("report_user");
    cred->setUserPassword("Sup3rS3cret!");
    cred->setDatabaseName("admin");
    cred->setEnabled(true);
    conn.addCredential(cred);
    conn.sslSettings()->enableSSL(true);
    conn.sslSettings()->setUsePemFile(true);
    conn.sslSettings()->setPemKeyFile("C:/certs/client.pem");
    conn.sslSettings()->setPemPassPhrase("PemPhrase42");
    conn.sslSettings()->setCaFile("C:/certs/ca.pem");

    const QJsonObject summary = McpGuards::connectionSummary(conn);
    EXPECT_EQ(summary.value("name").toString(), QStringLiteral("Reporting"));
    EXPECT_EQ(summary.value("uuid").toString(), QStringLiteral("uuid-1"));
    EXPECT_EQ(summary.value("address").toString(), QStringLiteral("db.example.test:27018"));
    EXPECT_TRUE(summary.value("tls").toBool());

    const QString text = QString::fromUtf8(QJsonDocument(summary).toJson());
    for (const char *secret : {"Sup3rS3cret!", "report_user", "PemPhrase42", "client.pem", "ca.pem"})
        EXPECT_FALSE(text.contains(QString::fromLatin1(secret))) << secret;
}
