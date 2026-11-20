// Live integration test: ScriptEngine -> the embedded shell (JsShellBackend) -> MongoDB.
//
// Requires:
//   - dev/mongo-test single node up on localhost:27017
//   - ./seed.sh seed having populated the "samples" db
// Tests are skipped when the server is unreachable.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

#include <QCoreApplication>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell/core/engine/ScriptEngine.h"
#include "ragoshell/core/mongodb/MongoUriUtils.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/utils/AggregatePaging.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/settings/ReplicaSetSettings.h"
#include "ragoshell/core/settings/SslSettings.h"

namespace RagoShell
{
namespace
{
    void ensureMongoInstance()
    {
        // One mongocxx::instance per process; another test may already own it.
        try {
            static mongocxx::instance inst{};
            (void)inst;
        } catch (const std::exception &) {
        }
    }

    bool mongoReachable()
    {
        ensureMongoInstance();
        try {
            mongocxx::uri uri("mongodb://localhost:27017/?serverSelectionTimeoutMS=2000");
            mongocxx::client client(uri);
            using bsoncxx::builder::basic::kvp;
            client.database("admin").run_command(
                bsoncxx::builder::basic::make_document(kvp("ping", 1)));
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

    QCoreApplication *ensureApp()
    {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char appName[] = "rago_unit_tests";
            static char *argv[] = { appName, nullptr };
            static QCoreApplication app(argc, argv);
            return &app;
        }
        return QCoreApplication::instance();
    }
}

class ScriptEngineLiveTest : public ::testing::Test
{
public:
    // Public: gtest 1.8 calls it from the parameterized-test registry.
    static void SetUpTestCase()
    {
        ensureApp();
        _settings.reset(new ConnectionSettings(/*isClone=*/false));
        _settings->setServerHost("localhost");
        _settings->setServerPort(27017);
        _settings->setDefaultDatabase("samples");
    }

protected:
    void SetUp() override
    {
        // gtest 1.8 has no GTEST_SKIP; report and pass.
        if (!mongoReachable()) {
            std::cerr << "[ SKIP ] MongoDB not reachable on localhost:27017\n";
            _skip = true;
        }
    }

    std::unique_ptr<ScriptEngine> makeEngine(int timeoutSec = 15)
    {
        auto engine = std::make_unique<ScriptEngine>(_settings.get(), timeoutSec);
        engine->init("", "samples");
        return engine;
    }

    bool _skip = false;
    static std::unique_ptr<ConnectionSettings> _settings;
};

std::unique_ptr<ConnectionSettings> ScriptEngineLiveTest::_settings;

TEST_F(ScriptEngineLiveTest, init_connects_via_rago_shell)
{
    if (_skip) return;
    if (_skip) return;
    auto engine = makeEngine();
    EXPECT_FALSE(engine->failedScope());
}

TEST_F(ScriptEngineLiveTest, exec_find_returns_documents_and_queryinfo)
{
    if (_skip) return;
    auto engine = makeEngine();
    ASSERT_FALSE(engine->failedScope());

    MongoShellExecResult res = engine->exec(
        "db.restaurants.find({cuisine:\"Italian\"}).limit(5)", "samples");

    ASSERT_FALSE(res.error()) << res.errorMessage();
    ASSERT_EQ(res.results().size(), 1u);
    const MongoShellResult &r = res.results().front();
    EXPECT_EQ(r.type(), "query");
    EXPECT_EQ(r.documents().size(), 5u);
    EXPECT_EQ(r.queryInfo()._info._ns.collectionName(), "restaurants");
    EXPECT_EQ(r.queryInfo()._info._ns.databaseName(), "samples");
}

TEST_F(ScriptEngineLiveTest, exec_aggregate_returns_aggrinfo)
{
    if (_skip) return;
    auto engine = makeEngine();
    ASSERT_FALSE(engine->failedScope());

    MongoShellExecResult res = engine->exec(
        "db.zips.aggregate([{$group:{_id:\"$state\",n:{$sum:1}}},{$limit:3}])",
        "samples");

    ASSERT_FALSE(res.error()) << res.errorMessage();
    const MongoShellResult &r = res.results().front();
    EXPECT_EQ(r.type(), "aggregate");
    EXPECT_EQ(r.documents().size(), 3u);
    EXPECT_TRUE(r.aggrInfo().isValid);
    EXPECT_EQ(r.aggrInfo().collectionName, "zips");
}

// The next page of an aggregate is a script built from the recorded pipeline (ShellController::loadPage). A pipeline
// with an ObjectId or a date in it used to come back as {"$oid": ...} documents and the server refused it.
TEST_F(ScriptEngineLiveTest, an_aggregate_page_keeps_object_ids_and_dates_in_its_pipeline)
{
    if (_skip) return;
    auto engine = makeEngine();
    ASSERT_FALSE(engine->failedScope());

    const auto pipeline = bsoncxx::from_json(
        R"({"0": {"$match": {"_id": {"$gt": {"$oid": "000000000000000000000000"}},
                             "cuisine": {"$regex": "^Ital"}}},
            "1": {"$addFields": {"since": {"$subtract": [{"$date": "2020-01-01T00:00:00Z"}, {"$date": "2000-01-01T00:00:00Z"}]}}},
            "2": {"$sort": {"_id": 1}}})");
    const auto options = bsoncxx::from_json("{}");

    for (const int skip : { 0, 3 }) {
        MongoShellExecResult res = engine->exec(aggregatePageScript("restaurants", pipeline.view(), options.view(), skip, 3),
                                                "samples");
        ASSERT_FALSE(res.error()) << "skip " << skip << ": " << res.errorMessage();
        ASSERT_EQ(res.results().size(), 1u);
        const MongoShellResult &r = res.results().front();
        EXPECT_EQ(r.type(), "aggregate");
        EXPECT_EQ(r.documents().size(), 3u) << "skip " << skip;
    }
}

TEST_F(ScriptEngineLiveTest, exec_print_produces_text_output)
{
    if (_skip) return;
    auto engine = makeEngine();
    ASSERT_FALSE(engine->failedScope());

    MongoShellExecResult res = engine->exec("print('hello-shell')", "samples");
    ASSERT_FALSE(res.error()) << res.errorMessage();
    const MongoShellResult &r = res.results().front();
    EXPECT_NE(r.response().find("hello-shell"), std::string::npos);
}

TEST_F(ScriptEngineLiveTest, startup_script_defines_helpers_for_every_query)
{
    if (_skip) return;
    auto engine = std::make_unique<ScriptEngine>(_settings.get(), 15);
    engine->setStartupScript(QStringLiteral("function hello() { return 'rc ok' }"));
    engine->init("", "samples");
    ASSERT_FALSE(engine->failedScope());

    // The helper is there in the first query and in the next one (each eval has a fresh sandbox).
    for (int i = 0; i < 2; ++i) {
        MongoShellExecResult res = engine->exec("print(hello())", "samples");
        ASSERT_FALSE(res.error()) << res.errorMessage();
        EXPECT_NE(res.results().front().response().find("rc ok"), std::string::npos);
    }
}

TEST_F(ScriptEngineLiveTest, startup_script_with_a_syntax_error_does_not_block_queries)
{
    if (_skip) return;
    auto engine = std::make_unique<ScriptEngine>(_settings.get(), 15);
    engine->setStartupScript(QStringLiteral("function ((("));
    engine->init("", "samples");
    ASSERT_FALSE(engine->failedScope());

    MongoShellExecResult res = engine->exec("print('still-fine')", "samples");
    ASSERT_FALSE(res.error()) << res.errorMessage();
    EXPECT_NE(res.results().front().response().find("still-fine"), std::string::npos);
}

TEST_F(ScriptEngineLiveTest, exec_error_is_reported_not_thrown)
{
    if (_skip) return;
    auto engine = makeEngine();
    ASSERT_FALSE(engine->failedScope());

    MongoShellExecResult res = engine->exec("syntax error (((", "samples");
    ASSERT_FALSE(res.error()) << res.errorMessage();
    const MongoShellResult &r = res.results().front();
    EXPECT_NE(r.response().find("SyntaxError"), std::string::npos);
    // Engine must survive an eval error (no desync).
    MongoShellExecResult res2 = engine->exec("print('still-alive')", "samples");
    ASSERT_FALSE(res2.error());
    EXPECT_NE(res2.results().front().response().find("still-alive"),
              std::string::npos);
}

TEST_F(ScriptEngineLiveTest, interrupt_marks_engine_failed)
{
    if (_skip) return;
    auto engine = makeEngine(60);
    ASSERT_FALSE(engine->failedScope());
    // Interrupt from another thread while a long-running script blocks.
    std::thread interrupter([&engine] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        engine->interrupt();
    });
    MongoShellExecResult res = engine->exec("while(true){}", "samples");
    interrupter.join();
    EXPECT_TRUE(res.error() || engine->failedScope());
}

TEST_F(ScriptEngineLiveTest, exec_getIndexes_returns_documents)
{
    if (_skip) return;
    auto engine = makeEngine();
    ASSERT_FALSE(engine->failedScope());

    MongoShellExecResult res = engine->exec(
        "db.getCollection('restaurants').getIndexes()", "samples");
    ASSERT_FALSE(res.error()) << res.errorMessage();
    ASSERT_EQ(res.results().size(), 1u);
    EXPECT_FALSE(res.results().front().documents().empty());
}

TEST(MongoUriUtilsTest, percent_encodes_special_credentials_and_honors_tls_flags)
{
    ConnectionSettings conn(/*isClone=*/false);
    conn.setServerHost("db.example.local");
    conn.setServerPort(27017);

    auto *cred = new CredentialSettings();
    cred->setEnabled(true);
    cred->setUserName("user@corp");
    cred->setUserPassword("p@ss#word:1/2?3%4");
    cred->setDatabaseName("admin");
    cred->setMechanism("MONGODB-CR"); // legacy value should be omitted
    conn.addCredential(cred);

    conn.sslSettings()->enableSSL(true);
    conn.sslSettings()->setUseAdvancedOptions(false);
    conn.sslSettings()->setAllowInvalidHostnames(true);

    const std::string uriStr = buildMongoUri(conn);
    EXPECT_NE(uriStr.find("user%40corp:p%40ss%23word%3A1%2F2%3F3%254@"), std::string::npos);
    EXPECT_EQ(uriStr.find("MONGODB-CR"), std::string::npos);
    EXPECT_NE(uriStr.find("tls=true"), std::string::npos);
    EXPECT_NE(uriStr.find("tlsAllowInvalidHostnames=true"), std::string::npos);

    // Must also parse cleanly in mongocxx::uri
    ensureMongoInstance();
    EXPECT_NO_THROW({
        mongocxx::uri parsed(uriStr);
        EXPECT_EQ(parsed.username(), "user@corp");
        EXPECT_EQ(parsed.password(), "p@ss#word:1/2?3%4");
    });
}

TEST(MongoUriUtilsTest, read_preference_and_x509_external_auth)
{
    ConnectionSettings conn(/*isClone=*/false);
    conn.setReplicaSet(true);
    conn.replicaSetSettings()->setMembers(std::vector<std::string>{"a.example:27017", "b.example:27017"});
    conn.replicaSetSettings()->setSetNameUserEntered("rs0");
    conn.replicaSetSettings()->setReadPreference(
        ReplicaSetSettings::ReadPreference::SECONDARY_PREFERRED);

    auto *cred = new CredentialSettings();
    cred->setEnabled(true);
    cred->setUserName("CN=client,OU=apps");
    cred->setUserPassword("ignored");
    cred->setDatabaseName("admin");
    cred->setMechanism("MONGODB-X509");
    conn.addCredential(cred);

    const std::string uriStr = buildMongoUri(conn);
    EXPECT_NE(uriStr.find("readPreference=secondaryPreferred"), std::string::npos) << uriStr;
    EXPECT_NE(uriStr.find("authSource=$external"), std::string::npos) << uriStr;
    EXPECT_NE(uriStr.find("authMechanism=MONGODB-X509"), std::string::npos) << uriStr;
    EXPECT_EQ(uriStr.find("ignored"), std::string::npos) << uriStr; // no password for X.509
    EXPECT_EQ(uriStr.find("authSource=admin"), std::string::npos) << uriStr;

    // Primary is the driver default and is not written.
    conn.replicaSetSettings()->setReadPreference(ReplicaSetSettings::ReadPreference::PRIMARY);
    EXPECT_EQ(buildMongoUri(conn).find("readPreference="), std::string::npos);

    ensureMongoInstance();
    EXPECT_NO_THROW({ mongocxx::uri parsed(uriStr); });
}

TEST(MongoUriUtilsTest, srv_emits_seedlist_host_only)
{
    ConnectionSettings conn(/*isClone=*/false);
    conn.setSrv(true);
    conn.setReplicaSet(true); // ignored for SRV: topology comes from DNS
    conn.replicaSetSettings()->setMembers(std::vector<std::string>{"a:1", "b:2"});
    conn.setServerHost("cluster0.example.mongodb.net");
    conn.setServerPort(27017);

    EXPECT_FALSE(conn.isReplicaSet());
    EXPECT_EQ(conn.getFullAddress(), "cluster0.example.mongodb.net");

    const std::string uriStr = buildMongoUri(conn);
    EXPECT_EQ(uriStr.rfind("mongodb+srv://cluster0.example.mongodb.net/", 0), 0u) << uriStr;
    EXPECT_EQ(uriStr.find(":27017"), std::string::npos) << uriStr;
    EXPECT_EQ(uriStr.find("replicaSet="), std::string::npos) << uriStr;

    // Round-trips through the settings file format.
    ConnectionSettings restored(/*isClone=*/false);
    restored.fromVariant(conn.toVariant().toMap());
    EXPECT_TRUE(restored.isSrv());
    EXPECT_EQ(buildMongoUri(restored), uriStr);
}

TEST(ScriptEngineCancelTest, pre_exec_and_mid_connect_interrupt_abort_immediately)
{
    ensureApp();

    // Unroutable TEST-NET-1 endpoint (RFC 5737) so connect() would block until timeout unless interrupted
    ConnectionSettings conn(/*isClone=*/false);
    conn.setServerHost("192.0.2.1");
    conn.setServerPort(27017);
    conn.setDefaultDatabase("test");

    ScriptEngine engine(&conn, /*timeoutSec=*/25);

    // 1. Pre-exec interrupt must return interrupted == true in ~0 ms without spawning or waiting
    engine.interrupt();
    const auto t0 = std::chrono::steady_clock::now();
    MongoShellExecResult preRes = engine.exec("db.coll.find({})", "test");
    const auto preMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_TRUE(preRes.error());
    EXPECT_TRUE(preRes.interrupted());
    EXPECT_LT(preMs, 500);

    // 2. Mid-connect interrupt (simulating Cancel clicked right after double-clicking a slow remote collection)
    std::thread stopper([&engine]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        engine.interrupt();
    });
    const auto t1 = std::chrono::steady_clock::now();
    MongoShellExecResult midRes = engine.exec("db.coll.find({})", "test");
    stopper.join();
    const auto midMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t1).count();

    EXPECT_TRUE(midRes.error());
    // The connect runs on a thread of its own, so the Stop is answered long before the 25 s timeout.
    EXPECT_LT(midMs, 4000);
}

TEST(ShellBackendTest, an_unreachable_server_is_a_failed_connection_not_a_crash)
{
    ensureApp();
    ensureMongoInstance();
    auto backend = createShellBackend();
    EXPECT_STREQ(backend->name(), "js");
    EXPECT_FALSE(backend->isRunning());

    // Nothing runs before a connect succeeded.
    ShellEvalRequest request;
    request.script = QStringLiteral("1 + 1");
    const ShellEvalReply early = backend->eval(request);
    EXPECT_EQ(early.status, ShellStatus::Failed);
    EXPECT_FALSE(early.message.isEmpty());

    ShellConnectOptions options;
    options.uri = QStringLiteral("mongodb://192.0.2.1:27017/"); // TEST-NET-1: nobody answers
    options.serverSelectionTimeoutMs = 500;
    const ShellReply failed = backend->connect(options);
    EXPECT_EQ(failed.status, ShellStatus::Failed);
    EXPECT_FALSE(failed.message.isEmpty());
    EXPECT_FALSE(backend->isRunning());

    // The other calls are harmless too.
    backend->use("test");
    backend->setBatchSize(20);
    EXPECT_FALSE(backend->ping());
    EXPECT_TRUE(backend->complete("db.").isEmpty());

    // An interrupt requested before a connect is taken and reported as such.
    backend->interrupt();
    const ShellReply interrupted = backend->connect(options);
    EXPECT_EQ(interrupted.status, ShellStatus::Interrupted);
}
}
