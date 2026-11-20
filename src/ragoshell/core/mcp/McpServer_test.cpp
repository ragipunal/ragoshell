#include <gtest/gtest.h>

#include <iostream>
#include <stdexcept>

#include <QJsonDocument>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/mcp/McpBackend.h"
#include "ragoshell/core/mcp/McpServer.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/CredentialSettings.h"

using namespace RagoShell;

namespace
{
    /// Records what the server asks for; returns canned documents.
    class FakeBackend : public McpBackend
    {
    public:
        int calls = 0;
        std::int64_t lastLimit = -1;
        int lastMaxTimeMs = -1;
        std::string lastFilter;
        std::string lastPipeline;
        std::string lastExplain;
        int documents = 3;
        std::string documentJson = R"({"_id": {"$oid": "5f1d7f1c2c8f4b6a1a2b3c4d"}, "n": {"$numberLong": "9007199254740993"}})";
        std::string throwMessage;

        QJsonArray listDatabases(const ConnectionSettings &) override
        {
            ++calls;
            maybeThrow();
            return QJsonArray{QJsonObject{{"name", "samples"}}};
        }
        QJsonArray listCollections(const ConnectionSettings &, const QString &) override
        {
            ++calls;
            maybeThrow();
            return QJsonArray{QJsonObject{{"name", "restaurants"}, {"type", "collection"}}};
        }
        SchemaResult collectionSchema(const ConnectionSettings &, const QString &, const QString &,
                                      int sampleSize, int maxTimeMs) override
        {
            ++calls;
            maybeThrow();
            lastLimit = sampleSize;
            lastMaxTimeMs = maxTimeMs;
            SchemaAccumulator acc;
            acc.add(bsoncxx::from_json(R"({"name": "a", "age": 30})").view());
            acc.add(bsoncxx::from_json(R"({"name": "b"})").view());
            return acc.result();
        }
        std::vector<bsoncxx::document::value> find(const ConnectionSettings &, const McpFindQuery &q) override
        {
            ++calls;
            maybeThrow();
            lastLimit = q.limit;
            lastMaxTimeMs = q.maxTimeMs;
            lastFilter = bsoncxx::to_json(q.filter.view());
            return make(std::min<std::int64_t>(documents, q.limit));
        }
        std::vector<bsoncxx::document::value> aggregate(const ConnectionSettings &, const QString &,
                                                        const QString &, bsoncxx::array::view pipeline,
                                                        std::int64_t limit, int maxTimeMs) override
        {
            ++calls;
            maybeThrow();
            lastLimit = limit;
            lastMaxTimeMs = maxTimeMs;
            lastPipeline = bsoncxx::to_json(pipeline);
            return make(std::min<std::int64_t>(documents, limit));
        }
        std::int64_t count(const ConnectionSettings &, const QString &, const QString &,
                           bsoncxx::document::view filter, int maxTimeMs) override
        {
            ++calls;
            maybeThrow();
            lastFilter = bsoncxx::to_json(filter);
            lastMaxTimeMs = maxTimeMs;
            return 42;
        }
        bsoncxx::document::value explain(const ConnectionSettings &, const QString &,
                                         bsoncxx::document::view command) override
        {
            ++calls;
            maybeThrow();
            lastExplain = bsoncxx::to_json(command);
            return bsoncxx::from_json(R"({
                "queryPlanner": {"namespace": "samples.restaurants",
                                 "winningPlan": {"stage": "COLLSCAN", "filter": {"a": {"$eq": 1}}}},
                "executionStats": {"nReturned": 1, "totalDocsExamined": 100, "totalKeysExamined": 0,
                                   "executionTimeMillis": 3,
                                   "executionStages": {"stage": "COLLSCAN", "nReturned": 1, "docsExamined": 100}},
                "ok": 1})");
        }

    private:
        void maybeThrow()
        {
            if (!throwMessage.empty())
                throw std::runtime_error(throwMessage);
        }
        std::vector<bsoncxx::document::value> make(std::int64_t n)
        {
            std::vector<bsoncxx::document::value> docs;
            for (std::int64_t i = 0; i < n; ++i)
                docs.push_back(bsoncxx::from_json(documentJson));
            return docs;
        }
    };

    struct Fixture
    {
        ConnectionSettings local{/*isClone=*/false};
        ConnectionSettings other{/*isClone=*/false};
        ConnectionSettings hidden{/*isClone=*/false};
        McpConfig config;
        FakeBackend backend;

        Fixture()
        {
            local.setConnectionName("Local");
            local.setUuid("uuid-local");
            auto *cred = new CredentialSettings();
            cred->setUserName("app");
            cred->setUserPassword("Hunter2Secret");
            cred->setEnabled(true);
            local.addCredential(cred);
            other.setConnectionName("Other");
            other.setUuid("uuid-other");
            hidden.setConnectionName("Hidden");
            hidden.setUuid("uuid-hidden");
            config.enabled = true;
            config.allowedConnections = {"uuid-local", "uuid-other"};
            config.maxDocuments = 5;
            config.maxTimeMs = 1234;
        }

        McpServer server(bool onlyLocal = false)
        {
            if (onlyLocal)
                return McpServer(config, {&local}, backend);
            return McpServer(config, {&local, &other, &hidden}, backend);
        }
    };

    QJsonObject request(McpServer &server, const QString &method, const QJsonObject &params = {}, int id = 1)
    {
        const QJsonObject msg{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
        const QByteArray line = server.handleLine(QJsonDocument(msg).toJson(QJsonDocument::Compact));
        return QJsonDocument::fromJson(line).object();
    }

    /// tools/call → (text, isError)
    std::pair<QString, bool> call(McpServer &server, const QString &tool, const QJsonObject &args = {})
    {
        const QJsonObject response =
            request(server, "tools/call", QJsonObject{{"name", tool}, {"arguments", args}});
        const QJsonObject result = response.value("result").toObject();
        return {result.value("content").toArray().at(0).toObject().value("text").toString(),
                result.value("isError").toBool(false)};
    }

    QJsonObject parse(const QString &text)
    {
        return QJsonDocument::fromJson(text.toUtf8()).object();
    }
}

TEST(McpServerTest, initialize_negotiates_protocol_version)
{
    Fixture f;
    McpServer server = f.server();
    QJsonObject result = request(server, "initialize", {{"protocolVersion", "2025-03-26"}}).value("result").toObject();
    EXPECT_EQ(result.value("protocolVersion").toString(), QStringLiteral("2025-03-26"));
    EXPECT_TRUE(result.value("capabilities").toObject().contains("tools"));
    EXPECT_EQ(result.value("serverInfo").toObject().value("name").toString(),
              QString::fromUtf8(RagoShell::BrandInfo::productId));

    result = request(server, "initialize", {{"protocolVersion", "1999-01-01"}}).value("result").toObject();
    EXPECT_EQ(result.value("protocolVersion").toString(), McpServer::supportedProtocolVersions().first());
}

TEST(McpServerTest, json_rpc_edge_cases)
{
    Fixture f;
    McpServer server = f.server();
    // Notifications get no response.
    EXPECT_TRUE(server.handleLine(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").isEmpty());
    EXPECT_EQ(request(server, "ping").value("result").toObject(), QJsonObject());
    EXPECT_EQ(request(server, "resources/list").value("error").toObject().value("code").toInt(), -32601);
    EXPECT_EQ(request(server, "tools/call", {{"name", "drop_database"}}).value("error").toObject()
                  .value("code").toInt(), -32602);

    const QJsonObject parseError = QJsonDocument::fromJson(server.handleLine("{not json")).object();
    EXPECT_EQ(parseError.value("error").toObject().value("code").toInt(), -32700);

    const QJsonArray batch = QJsonDocument::fromJson(server.handleLine(
        R"([{"jsonrpc":"2.0","id":7,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/cancelled"}])")).array();
    ASSERT_EQ(batch.size(), 1);
    EXPECT_EQ(batch.at(0).toObject().value("id").toInt(), 7);
}

TEST(McpServerTest, tools_are_all_read_only)
{
    const QJsonArray tools = McpServer::toolDefinitions();
    QStringList names;
    for (const QJsonValue &v : tools) {
        const QJsonObject tool = v.toObject();
        names << tool.value("name").toString();
        EXPECT_TRUE(tool.value("annotations").toObject().value("readOnlyHint").toBool()) << names.last().toStdString();
        EXPECT_EQ(tool.value("inputSchema").toObject().value("type").toString(), QStringLiteral("object"));
    }
    EXPECT_EQ(names, QStringList({"list_connections", "list_databases", "list_collections", "collection_schema",
                                  "find", "aggregate", "count", "explain"}));
}

TEST(McpServerTest, exposes_only_allowed_connections_without_secrets)
{
    Fixture f;
    McpServer server = f.server();
    const auto [text, isError] = call(server, "list_connections");
    ASSERT_FALSE(isError) << text.toStdString();
    const QJsonArray list = parse(text).value("connections").toArray();
    ASSERT_EQ(list.size(), 2);
    EXPECT_EQ(list.at(0).toObject().value("name").toString(), QStringLiteral("Local"));
    EXPECT_EQ(list.at(1).toObject().value("name").toString(), QStringLiteral("Other"));
    EXPECT_FALSE(text.contains("Hidden"));
    EXPECT_FALSE(text.contains("Hunter2Secret"));
    EXPECT_FALSE(text.contains("\"app\""));

    // A connection outside the allow-list is unknown, by name or uuid.
    EXPECT_TRUE(call(server, "list_databases", {{"connection", "Hidden"}}).second);
    EXPECT_TRUE(call(server, "list_databases", {{"connection", "uuid-hidden"}}).second);
    EXPECT_EQ(f.backend.calls, 0);
    // Several exposed: the connection must be named.
    EXPECT_TRUE(call(server, "list_databases").second);
    EXPECT_FALSE(call(server, "list_databases", {{"connection", "local"}}).second);
    EXPECT_FALSE(call(server, "list_databases", {{"connection", "uuid-other"}}).second);
}

TEST(McpServerTest, disabled_config_exposes_nothing)
{
    Fixture f;
    f.config.enabled = false;
    McpServer server = f.server();
    EXPECT_TRUE(server.connections().empty());
    EXPECT_EQ(parse(call(server, "list_connections").first).value("connections").toArray().size(), 0);
    EXPECT_TRUE(call(server, "find", {{"connection", "Local"}, {"database", "d"}, {"collection", "c"}}).second);
    EXPECT_EQ(f.backend.calls, 0);
}

TEST(McpServerTest, a_changed_config_applies_to_a_running_server)
{
    Fixture f;
    McpServer server = f.server();
    ASSERT_EQ(server.connections().size(), 2u);

    // Revoked while running: gone at the next request.
    McpConfig revoked = f.config;
    revoked.allowedConnections = {"uuid-other"};
    server.applyConfig(revoked);
    ASSERT_EQ(server.connections().size(), 1u);
    EXPECT_TRUE(call(server, "find", {{"connection", "Local"}, {"database", "d"}, {"collection", "c"}}).second);
    EXPECT_EQ(f.backend.calls, 0);

    // Turned off: nothing is served, and the client is told why.
    McpConfig off = f.config;
    off.enabled = false;
    server.applyConfig(off);
    EXPECT_TRUE(server.connections().empty());
    const auto [text, isError] = call(server, "list_databases");
    EXPECT_TRUE(isError);
    EXPECT_TRUE(text.contains("turned off")) << text.toStdString();

    // Allowing more works only among the connections offered at start
    // (the host offers what was allowed then, or the one pinned connection).
    McpConfig widened = f.config;
    widened.allowedConnections = {"uuid-local", "uuid-other", "uuid-hidden"};
    server.applyConfig(widened);
    EXPECT_EQ(server.connections().size(), 3u);
    McpServer pinned = f.server(/*onlyLocal=*/true);
    pinned.applyConfig(widened);
    EXPECT_EQ(pinned.connections().size(), 1u);

    // New limits apply as well.
    McpConfig tighter = f.config;
    tighter.maxDocuments = 2;
    server.applyConfig(tighter);
    ASSERT_FALSE(call(server, "find", {{"connection", "Local"}, {"database", "d"}, {"collection", "c"},
                                       {"limit", 50}}).second);
    EXPECT_EQ(f.backend.lastLimit, 3);   // cap + 1 to detect hasMore
}

TEST(McpServerTest, find_is_capped_and_bounded)
{
    Fixture f;
    f.backend.documents = 100;
    McpServer server = f.server(/*onlyLocal=*/true);
    const auto [text, isError] = call(server, "find", {{"database", "samples"}, {"collection", "restaurants"},
                                                       {"filter", "{ _id: ObjectId('5f1d7f1c2c8f4b6a1a2b3c4d') }"},
                                                       {"limit", 500}});
    ASSERT_FALSE(isError) << text.toStdString();
    EXPECT_EQ(f.backend.lastLimit, 6);   // maxDocuments + 1 to detect more
    EXPECT_EQ(f.backend.lastMaxTimeMs, 1234);
    EXPECT_NE(f.backend.lastFilter.find("$oid"), std::string::npos) << f.backend.lastFilter;

    const QJsonObject result = parse(text);
    EXPECT_EQ(result.value("returned").toInt(), 5);
    EXPECT_EQ(result.value("documents").toArray().size(), 5);
    EXPECT_TRUE(result.value("hasMore").toBool());
    // Relaxed Extended JSON keeps ObjectId and 64-bit integers exact.
    EXPECT_TRUE(text.contains("5f1d7f1c2c8f4b6a1a2b3c4d"));
    EXPECT_TRUE(text.contains("9007199254740993"));

    EXPECT_FALSE(call(server, "find", {{"database", "samples"}, {"collection", "restaurants"},
                                       {"limit", 1e300}, {"skip", -1e300}}).second);
    EXPECT_EQ(f.backend.lastLimit, 6);

    f.backend.documents = 2;
    const QJsonObject small = parse(call(server, "find", {{"database", "samples"}, {"collection", "restaurants"},
                                                          {"limit", 3}}).first);
    EXPECT_EQ(f.backend.lastLimit, 4);
    EXPECT_EQ(small.value("returned").toInt(), 2);
    EXPECT_FALSE(small.value("hasMore").toBool());
}

TEST(McpServerTest, find_validates_input)
{
    Fixture f;
    McpServer server = f.server(true);
    EXPECT_TRUE(call(server, "find", {{"collection", "c"}}).second);
    EXPECT_TRUE(call(server, "find", {{"database", "a.b"}, {"collection", "c"}}).second);
    EXPECT_TRUE(call(server, "find", {{"database", "d"}, {"collection", "c"}, {"filter", "{ a: "}}).second);
    EXPECT_TRUE(call(server, "find", {{"database", "d"}, {"collection", "c"},
                                      {"filter", QJsonObject{{"$where", "sleep(100000)"}}}}).second);
    EXPECT_TRUE(call(server, "find", {{"database", "d"}, {"collection", "c"}, {"limit", "lots"}}).second);
    EXPECT_EQ(f.backend.calls, 0);
}

TEST(McpServerTest, aggregate_rejects_writes_and_appends_limit)
{
    Fixture f;
    McpServer server = f.server(true);
    const QJsonObject base{{"database", "samples"}, {"collection", "restaurants"}};

    QJsonObject args = base;
    args["pipeline"] = QJsonArray{QJsonObject{{"$match", QJsonObject()}}, QJsonObject{{"$out", "copy"}}};
    auto [text, isError] = call(server, "aggregate", args);
    EXPECT_TRUE(isError);
    EXPECT_TRUE(text.contains("$out")) << text.toStdString();

    args["pipeline"] = "[{ $facet: { a: [ { $merge: { into: 'copy' } } ] } }]";
    EXPECT_TRUE(call(server, "aggregate", args).second);
    EXPECT_EQ(f.backend.calls, 0);

    args["pipeline"] = "[{ $group: { _id: '$borough', n: { $sum: 1 } } }]";
    std::tie(text, isError) = call(server, "aggregate", args);
    ASSERT_FALSE(isError) << text.toStdString();
    EXPECT_EQ(f.backend.lastLimit, 6);
    EXPECT_NE(f.backend.lastPipeline.find("$group"), std::string::npos);

    args["pipeline"] = QJsonArray();
    EXPECT_FALSE(call(server, "aggregate", args).second);
}

TEST(McpServerTest, count_schema_and_explain)
{
    Fixture f;
    McpServer server = f.server(true);
    const QJsonObject base{{"database", "samples"}, {"collection", "restaurants"}};

    auto [text, isError] = call(server, "count", base);
    ASSERT_FALSE(isError) << text.toStdString();
    EXPECT_EQ(parse(text).value("count").toInt(), 42);

    QJsonObject args = base;
    args["sampleSize"] = 100000;
    std::tie(text, isError) = call(server, "collection_schema", args);
    ASSERT_FALSE(isError) << text.toStdString();
    EXPECT_EQ(f.backend.lastLimit, McpConfig::kHardMaxSampleSize);
    const QJsonObject schema = parse(text);
    EXPECT_EQ(schema.value("sampledDocuments").toInt(), 2);
    // Schema only by default: no sample values.
    EXPECT_FALSE(text.contains("topValues"));
    args["includeValues"] = true;
    EXPECT_TRUE(call(server, "collection_schema", args).first.contains("topValues"));

    args = base;
    args["filter"] = QJsonObject{{"a", 1}};
    std::tie(text, isError) = call(server, "explain", args);
    ASSERT_FALSE(isError) << text.toStdString();
    EXPECT_NE(f.backend.lastExplain.find("\"find\""), std::string::npos) << f.backend.lastExplain;
    EXPECT_NE(f.backend.lastExplain.find("maxTimeMS"), std::string::npos);
    const QJsonObject plan = parse(text);
    EXPECT_EQ(plan.value("totalDocsExamined").toInt(), 100);
    EXPECT_FALSE(plan.value("warnings").toArray().isEmpty());

    args = base;
    args["pipeline"] = QJsonArray{QJsonObject{{"$out", "x"}}};
    EXPECT_TRUE(call(server, "explain", args).second);
    args["pipeline"] = QJsonArray{QJsonObject{{"$match", QJsonObject{{"a", 1}}}}};
    EXPECT_FALSE(call(server, "explain", args).second);
    EXPECT_NE(f.backend.lastExplain.find("\"aggregate\""), std::string::npos);
    EXPECT_NE(f.backend.lastExplain.find("$limit"), std::string::npos);
}

TEST(McpServerTest, errors_never_leak_the_password)
{
    Fixture f;
    f.backend.throwMessage = "Authentication failed for mongodb://app:Hunter2Secret@db:27017";
    McpServer server = f.server(true);
    const auto [text, isError] = call(server, "list_databases");
    EXPECT_TRUE(isError);
    EXPECT_FALSE(text.contains("Hunter2Secret")) << text.toStdString();
    EXPECT_TRUE(text.contains("*****"));
}

TEST(McpServerTest, result_size_is_capped)
{
    Fixture f;
    f.config.maxDocuments = 1000;
    f.config.maxResultBytes = 4096;
    f.backend.documents = 1000;
    f.backend.documentJson = std::string(R"({"blob": ")") + std::string(1000, 'x') + "\"}";
    McpServer server = f.server(true);
    const auto [text, isError] = call(server, "find", {{"database", "d"}, {"collection", "c"}});
    ASSERT_FALSE(isError);
    const QJsonObject result = parse(text);
    EXPECT_LT(result.value("returned").toInt(), 5);
    EXPECT_TRUE(result.value("hasMore").toBool());
    EXPECT_TRUE(result.contains("note"));
    EXPECT_LT(text.size(), 6000);
}

// Live: local MongoDB (e.g. Docker mongo:4.0 on localhost:27017) through the
// real mongocxx backend, end to end through the JSON-RPC layer.
TEST(McpServerTest, live_read_only_tools_against_local_mongodb)
{
    try {
        static mongocxx::instance instance{};
    } catch (const std::exception &) {
    }
    const std::string dbName = "rago_mcp_e2e";
    try {
        mongocxx::client seed{mongocxx::uri{"mongodb://localhost:27017/?serverSelectionTimeoutMS=2000"}};
        auto db = seed[dbName];
        db.drop();
        auto items = db["items"];
        std::vector<bsoncxx::document::value> docs;
        for (int i = 0; i < 30; ++i) {
            using bsoncxx::builder::basic::kvp;
            docs.push_back(bsoncxx::builder::basic::make_document(
                kvp("n", i), kvp("kind", i % 2 ? "odd" : "even")));
        }
        items.insert_many(docs);
    } catch (const std::exception &ex) {
        std::cerr << "[ SKIP ] MongoDB not reachable on localhost:27017: " << ex.what() << "\n";
        return;
    }

    ConnectionSettings conn(/*isClone=*/false);
    conn.setConnectionName("Local Docker");
    conn.setUuid("uuid-live");
    conn.setServerHost("localhost");
    conn.setServerPort(27017);
    McpConfig config;
    config.enabled = true;
    config.allowedConnections = {"uuid-live"};
    config.maxDocuments = 10;
    config.maxTimeMs = 5000;
    MongoMcpBackend backend(DefaultEncoding, Utc);
    McpServer server(config, {&conn}, backend);

    auto [text, isError] = call(server, "list_databases");
    ASSERT_FALSE(isError) << text.toStdString();
    EXPECT_TRUE(text.contains(QString::fromStdString(dbName)));

    std::tie(text, isError) = call(server, "list_collections", {{"database", "rago_mcp_e2e"}});
    ASSERT_FALSE(isError) << text.toStdString();
    EXPECT_TRUE(text.contains("\"items\""));

    const QJsonObject base{{"database", "rago_mcp_e2e"}, {"collection", "items"}};
    QJsonObject args = base;
    args["filter"] = "{ kind: 'even' }";
    args["sort"] = QJsonObject{{"n", 1}};
    args["limit"] = 100;
    std::tie(text, isError) = call(server, "find", args);
    ASSERT_FALSE(isError) << text.toStdString();
    QJsonObject result = parse(text);
    EXPECT_EQ(result.value("returned").toInt(), 10);   // 15 match, capped at 10
    EXPECT_TRUE(result.value("hasMore").toBool());
    EXPECT_EQ(result.value("documents").toArray().at(0).toObject().value("n").toInt(), 0);

    std::tie(text, isError) = call(server, "count", base);
    ASSERT_FALSE(isError) << text.toStdString();
    EXPECT_EQ(parse(text).value("count").toInt(), 30);

    args = base;
    args["pipeline"] = "[{ $group: { _id: '$kind', total: { $sum: '$n' } } }, { $sort: { _id: 1 } }]";
    std::tie(text, isError) = call(server, "aggregate", args);
    ASSERT_FALSE(isError) << text.toStdString();
    result = parse(text);
    EXPECT_EQ(result.value("returned").toInt(), 2);
    EXPECT_FALSE(result.value("hasMore").toBool());

    args["pipeline"] = "[{ $match: {} }, { $out: 'items_copy' }]";
    EXPECT_TRUE(call(server, "aggregate", args).second);

    std::tie(text, isError) = call(server, "collection_schema", base);
    ASSERT_FALSE(isError) << text.toStdString();
    EXPECT_TRUE(text.contains("\"kind\""));

    args = base;
    args["filter"] = QJsonObject{{"n", 3}};
    std::tie(text, isError) = call(server, "explain", args);
    ASSERT_FALSE(isError) << text.toStdString();
    EXPECT_TRUE(text.contains("COLLSCAN")) << text.toStdString();

    args = base;
    args["pipeline"] = "[{ $match: { n: 3 } }]";
    std::tie(text, isError) = call(server, "explain", args);
    EXPECT_FALSE(isError) << text.toStdString();

    // $out was refused: nothing was written.
    mongocxx::client check{mongocxx::uri{"mongodb://localhost:27017"}};
    bool copyExists = false;
    for (auto &&name : check[dbName].list_collection_names())
        copyExists = copyExists || name == "items_copy";
    EXPECT_FALSE(copyExists);
    check[dbName].drop();
}
