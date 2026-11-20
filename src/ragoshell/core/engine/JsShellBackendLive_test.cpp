// Live tests for the embedded JavaScript shell backend (ScriptEngine / JsShellBackend).
// Requires MongoDB on localhost:27017 with the 'samples' database; skipped when unreachable.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

#include <QCoreApplication>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell/core/engine/ScriptEngine.h"
#include "ragoshell/core/mongodb/MongoUriUtils.h"
#include "ragoshell/core/settings/ConnectionSettings.h"

namespace RagoShell
{
namespace
{
    void ensureApp()
    {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char appName[] = "rago_unit_tests";
            static char *argv[] = { appName, nullptr };
            static QCoreApplication app(argc, argv);
        }
    }

    bool mongoReachable()
    {
        try {
            static mongocxx::instance inst{};
            (void)inst;
        } catch (const std::exception &) {
        }
        try {
            mongocxx::client client(mongocxx::uri("mongodb://localhost:27017/?serverSelectionTimeoutMS=2000"));
            using bsoncxx::builder::basic::kvp;
            client.database("admin").run_command(bsoncxx::builder::basic::make_document(kvp("ping", 1)));
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

}

// ---- what only the JS backend does (or does better) --------------------------------------------

class ShellJsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        if (!mongoReachable()) {
            std::cerr << "[ SKIP ] MongoDB not reachable on localhost:27017\n";
            _skip = true;
            return;
        }
        _settings.reset(new ConnectionSettings(/*isClone=*/false));
        _settings->setServerHost("localhost");
        _settings->setServerPort(27017);
        _settings->setDefaultDatabase("samples");
        _engine = std::make_unique<ScriptEngine>(_settings.get(), 15);
        _engine->init("", "samples");
        ASSERT_FALSE(_engine->failedScope()) << _engine->lastInitError();
    }

    void TearDown() override
    {
        if (_engine && !_skip)
            _engine->exec("db.getSiblingDB('rago_js_only').dropDatabase()", "samples");
    }

    MongoShellExecResult run(const std::string &script, const std::string &db = "samples")
    {
        return _engine->exec(script, db);
    }

    std::string output(const std::string &script, const std::string &db = "samples")
    {
        const MongoShellExecResult res = run(script, db);
        EXPECT_FALSE(res.error()) << res.errorMessage();
        return res.results().empty() ? std::string() : res.results().back().response();
    }

    bool _skip = false;
    std::unique_ptr<ConnectionSettings> _settings;
    std::unique_ptr<ScriptEngine> _engine;
};

TEST_F(ShellJsTest, a_server_result_can_be_used_inside_the_same_statement)
{
    if (_skip) return;
    // The old Node shell only ran server calls after the statement had been evaluated; here they are
    // synchronous, as in the legacy mongo shell.
    EXPECT_EQ(output("var seen = []; for (var i = 0; i < 3; i++) { var d = db.restaurants.findOne({}, {cuisine: 1}); seen.push(d.cuisine !== undefined) } seen.join(',')"),
              "\"true,true,true\"");
    EXPECT_EQ(output("var n = db.restaurants.count({cuisine: 'Italian'}); n > 0 ? 'some' : 'none'"), "\"some\"");
    EXPECT_EQ(output("var c = db.restaurants.find({}).sort({_id: 1}).limit(3); var names = []; while (c.hasNext()) { names.push(c.next()._id.toString()) } names.length"), "3");
    EXPECT_EQ(output("var total = 0; db.restaurants.find({}).limit(5).forEach(function (d) { total += 1 }); total"), "5");
}

TEST_F(ShellJsTest, print_and_printjson_write_what_they_are_given_and_the_statement_shows_nothing_more)
{
    if (_skip) return;
    // The old Node shell showed an extra "1" after every print statement (print returned the line count).
    auto outputs = [&](const std::string &script) {
        std::vector<std::string> list;
        const MongoShellExecResult res = run(script);
        EXPECT_FALSE(res.error()) << res.errorMessage();
        for (const MongoShellResult &r : res.results())
            list.push_back(r.response());
        return list;
    };
    EXPECT_EQ(outputs("print('hello'); print({a:1,b:[1,2]}); 1+1"),
              (std::vector<std::string>{ "hello", "{\"a\":1,\"b\":[1,2]}", "2" }));
    EXPECT_EQ(outputs("print(1, 'two', [3]); console.log('via console', 5)"),
              (std::vector<std::string>{ "1 two [3]", "via console 5" }));
    EXPECT_EQ(outputs("const x = 3;\nlet y = x + 1;\nprintjson({x: x, y: y})"),
              (std::vector<std::string>{ "{\"x\":3,\"y\":4}" }));
    EXPECT_EQ(outputs("print('before'); undefinedThing; print('after')").size(), 2u); // "before", then the ReferenceError
    EXPECT_EQ(outputs("printjson({a: ISODate('2020-01-02T03:04:05Z'), c: ObjectId('507f1f77bcf86cd799439011'), d: NumberDecimal('0.1'), e: /ab+/i, f: Timestamp(1,2), g: UUID('123e4567-e89b-12d3-a456-426614174000'), h: BinData(0,'AAEC'), i: MinKey(), j: MaxKey(), k: DBRef('c', ObjectId('507f1f77bcf86cd799439011')), l: NumberLong(42), m: NumberInt(7), n: 1.5, o: [1, 'x', null, true]})"),
              (std::vector<std::string>{ "{\"a\":{\"$date\":\"2020-01-02T03:04:05Z\"},\"c\":{\"$oid\":\"507f1f77bcf86cd799439011\"},\"d\":{\"$numberDecimal\":\"0.1\"},\"e\":{\"$regularExpression\":{\"pattern\":\"ab+\",\"options\":\"i\"}},\"f\":{\"$timestamp\":{\"t\":1,\"i\":2}},\"g\":{\"$binary\":{\"base64\":\"Ej5FZ+ibEtOkVkJmFBdAAA==\",\"subType\":\"04\"}},\"h\":{\"$binary\":{\"base64\":\"AAEC\",\"subType\":\"00\"}},\"i\":{\"$minKey\":1},\"j\":{\"$maxKey\":1},\"k\":{\"$ref\":\"c\",\"$id\":{\"$oid\":\"507f1f77bcf86cd799439011\"}},\"l\":42,\"m\":7,\"n\":1.5,\"o\":[1,\"x\",null,true]}" }));
}

TEST_F(ShellJsTest, results_can_be_used_as_values_in_the_statement)
{
    if (_skip) return;
    // In the old Node shell these compared or indexed a pending call; here they are the values.
    const std::vector<std::pair<std::string, std::string>> cases = {
        { R"(db.restaurants.estimatedDocumentCount() > 0)", "true" },
        { R"(db.restaurants.distinct('cuisine').length > 10)", "true" },
        { R"(db.restaurants.aggregate([{$limit:2}]).toArray().length)", "2" },
        { R"(db.getCollectionNames().length > 0)", "true" },
        { R"(db.getCollectionNames().indexOf('restaurants') >= 0)", "true" },
        { R"(db.restaurants.stats().ns)", R"("samples.restaurants")" },
        { R"(db.stats().db)", R"("samples")" },
        { R"(db.runCommand({count: 'restaurants', query: {cuisine:'Italian'}}).n > 0)", "true" },
        { R"(db.getMongo().getDBNames().indexOf('samples') >= 0)", "true" },
        { R"(db.version().length > 0)", "true" },
        { R"(db.restaurants.find({cuisine:'Italian'}).limit(1).explain().queryPlanner !== undefined)", "true" },
        { R"(db.hello().ok)", "1" },
        { R"(var docs = db.restaurants.find({cuisine:'Italian'}).limit(2).sort({_id:1}).toArray(); docs.length)", "2" },
        { R"(db.restaurants.getIndexes().length)", "1" },
        { R"(db.restaurants.findOne({cuisine:'Italian'}).cuisine)", R"("Italian")" },
        { R"(NumberLong('5') + NumberLong('6'))", "11" },
        { R"(typeof NumberLong('5'))", R"("object")" },
        { R"(ObjectId('507f1f77bcf86cd799439011') instanceof ObjectId)", "true" },
        { R"(new ObjectId('507f1f77bcf86cd799439011').toString())", R"("507f1f77bcf86cd799439011")" },
    };
    for (const auto &c : cases)
        EXPECT_EQ(output(c.first), c.second) << c.first;
}

TEST_F(ShellJsTest, the_show_commands_and_helpers_answer)
{
    if (_skip) return;
    // Each of these must produce a result, not an error (the dev server has no authentication).
    for (const char *script : { "show dbs", "show databases", "show collections", "show tables", "show users",
                                "show roles", "show profile", "show logs", "show log global", "use samples",
                                "db.getUsers()", "db.getRoles()", "db.getProfilingStatus()", "db.serverStatus().ok",
                                "db.hostInfo().ok", "db.serverBuildInfo().ok", "db.listCommands().ok",
                                "db.currentOp().ok", "db.getCollectionInfos().length", "db.isMaster().ok",
                                "db.restaurants.latencyStats().length", "db.restaurants.storageSize() > 0",
                                "db.restaurants.dataSize() > 0", "db.restaurants.totalSize() > 0",
                                "db.restaurants.isCapped()", "db.restaurants.validate().ok",
                                "db.restaurants.getIndexKeys().length", "db.printCollectionStats().length",
                                "db.restaurants.explain().find({cuisine: 'Italian'}).queryPlanner !== undefined",
                                "db.restaurants.explain('executionStats').aggregate([{$limit: 1}]).ok",
                                "db.restaurants.aggregate([{$limit: 1}]).explain().ok",
                                "db.getMongo().host.length > 0", "db.getMongo().getDBs().ok" }) {
        const MongoShellExecResult res = run(script);
        ASSERT_FALSE(res.error()) << script << ": " << res.errorMessage();
        ASSERT_FALSE(res.results().empty()) << script;
        EXPECT_NE(res.results().back().type(), "error") << script << ": " << res.results().back().response();
    }
    // `show dbs` shows the databases as documents for the grid.
    const MongoShellExecResult dbs = run("show dbs");
    EXPECT_EQ(dbs.results().back().type(), "custom");
    EXPECT_GE(dbs.results().back().documents().size(), 1u);
    EXPECT_EQ(output("show collections").substr(0, 1), "[");
    EXPECT_EQ(output("use admin"), "\"switched to db admin\"");
}

TEST_F(ShellJsTest, bad_constructor_arguments_are_errors_not_silent_zeros)
{
    if (_skip) return;
    for (const char *script : { R"(NumberLong('abc'))", R"(ObjectId('xyz'))", R"(NumberLong('9223372036854775808'))",
                                R"(UUID('nope'))" }) {
        const MongoShellExecResult res = run(script);
        ASSERT_FALSE(res.results().empty()) << script;
        EXPECT_EQ(res.results().back().type(), "error") << script << ": " << res.results().back().response();
    }
}

TEST_F(ShellJsTest, sixty_four_bit_integers_keep_every_digit)
{
    if (_skip) return;
    output("db.getSiblingDB('rago_js_only').t.insertOne({_id: 8, n: NumberLong('9007199254740993')})");
    const MongoShellExecResult res = run("db.getSiblingDB('rago_js_only').t.findOne({_id: 8})");
    ASSERT_FALSE(res.error()) << res.errorMessage();
    ASSERT_EQ(res.results().back().documents().size(), 1u);
    const auto n = res.results().back().documents().front()->bsonObj()["n"];
    EXPECT_EQ(n.type(), bsoncxx::type::k_int64);
    EXPECT_EQ(n.get_int64().value, 9007199254740993LL);
    // The text keeps them too (Extended JSON: a number when it is exact, else $numberLong).
    EXPECT_NE(res.results().back().response().find("$numberLong"), std::string::npos);
    // Arithmetic and comparisons use the nearest number (valueOf), the stored value stays exact.
    EXPECT_EQ(output("NumberLong('9007199254740993') > 9007199254740000"), "true");
    EXPECT_EQ(output("NumberLong('9007199254740993').toString()"), "\"9007199254740993\"");
}

TEST_F(ShellJsTest, every_script_has_its_own_scope_and_its_statements_share_one)
{
    if (_skip) return;
    EXPECT_EQ(output("var kept = 41;\nconst more = 1;\nkept + more"), "42");
    // The next script does not see them (a fresh sandbox per eval, as in the old Node shell).
    EXPECT_EQ(output("typeof kept + ' ' + typeof more"), "\"undefined undefined\"");
    // `use` changes the database for the rest of the script and the next ones.
    EXPECT_EQ(output("use admin\ndb.getName()"), "\"admin\"");
}

TEST_F(ShellJsTest, the_startup_script_runs_in_every_script_and_its_declarations_are_visible)
{
    if (_skip) return;
    _engine->setStartupScript(QStringLiteral("const greet = (n) => 'hi ' + n;\nvar fromStartup = 7;"));
    _engine->init("", "samples");
    ASSERT_FALSE(_engine->failedScope()) << _engine->lastInitError();
    EXPECT_EQ(output("greet('rago') + fromStartup"), "\"hi rago7\"");
    EXPECT_EQ(output("greet('again')"), "\"hi again\"");
}

TEST_F(ShellJsTest, cursor_pages_are_limited_to_the_batch_size_and_close_cleanly)
{
    if (_skip) return;
    _engine->setBatchSize(7);
    const MongoShellExecResult res = run("db.restaurants.find({}).sort({_id: 1})");
    ASSERT_FALSE(res.error()) << res.errorMessage();
    EXPECT_EQ(res.results().back().documents().size(), 7u);
    EXPECT_EQ(res.results().back().queryInfo()._batchSize, 7);
    // And again: the cursor of the first run was closed, a new one opens without trouble.
    const MongoShellExecResult again = run("db.restaurants.find({}).sort({_id: 1}).limit(3)");
    EXPECT_EQ(again.results().back().documents().size(), 3u);
}

TEST_F(ShellJsTest, unicode_text_round_trips)
{
    if (_skip) return;
    output("db.getSiblingDB('rago_js_only').t.insertOne({_id: 1, s: 'caf\\u00e9 \\u20ac \\ud83d\\ude00'})");
    EXPECT_EQ(output("db.getSiblingDB('rago_js_only').t.findOne({_id: 1}).s"), "\"caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80\"");
}

TEST_F(ShellJsTest, the_script_can_be_stopped_while_it_loops)
{
    if (_skip) return;
    std::thread stopper([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        _engine->interrupt();
    });
    const MongoShellExecResult res = run("var n = 0; while (true) { n++ }");
    stopper.join();
    EXPECT_TRUE(res.error());
    EXPECT_TRUE(res.interrupted());
    // The engine is usable again after a new connect.
    _engine->clearInterrupt();
    _engine->init("", "samples");
    EXPECT_EQ(output("1 + 1"), "2");
}

// ---- stopping a script, its time limit and its errors ---------------------------------

namespace
{
    // How many operations of the shell (client name "<product id>-shell-...") are running on the server.
    int shellOperationsOnServer()
    {
        using bsoncxx::builder::basic::kvp;
        mongocxx::client client(mongocxx::uri("mongodb://localhost:27017/?serverSelectionTimeoutMS=2000&appName=rago-test-observer"));
        const auto reply = client.database("admin").run_command(
            bsoncxx::builder::basic::make_document(kvp("currentOp", 1), kvp("active", true)));
        int n = 0;
        for (const auto &op : reply.view()["inprog"].get_array().value) {
            const auto app = op.get_document().value["appName"];
            if (app && app.type() == bsoncxx::type::k_string
                && std::string(app.get_string().value).rfind(clientAppName("shell-"), 0) == 0)
                ++n;
        }
        return n;
    }

    bool serverIsQuietSoon()
    {
        for (int i = 0; i < 30; ++i) {
            if (shellOperationsOnServer() == 0)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    // Runs far longer than any test waits, until the server is told to stop it.
    const char *const kSlowServerCall = "db.getSiblingDB('rago_js_only').t.find({$where: 'sleep(30000) || true'}).toArray()";
}

TEST_F(ShellJsTest, stop_ends_a_long_server_operation_within_a_second_and_leaves_nothing_behind)
{
    if (_skip) return;
    output("db.getSiblingDB('rago_js_only').t.insertOne({_id: 1})");
    EXPECT_EQ(shellOperationsOnServer(), 0);

    std::thread stopper([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        _engine->interrupt();
    });
    QElapsedTimer timer;
    timer.start();
    const MongoShellExecResult res = run(kSlowServerCall);
    const qint64 ms = timer.elapsed();
    stopper.join();

    EXPECT_TRUE(res.error());
    EXPECT_TRUE(res.interrupted());
    EXPECT_FALSE(res.timeoutReached());
    EXPECT_LT(ms, 1500) << "the Stop took " << (ms - 500) << " ms after it was pressed";
    EXPECT_TRUE(serverIsQuietSoon()) << "currentOp still shows an operation of the shell";

    // The shell works again (it reconnects).
    EXPECT_EQ(output("1 + 1"), "2");
}

TEST_F(ShellJsTest, a_script_that_runs_past_the_time_limit_ends_with_timeout_reached)
{
    if (_skip) return;
    ScriptEngine engine(_settings.get(), /*timeoutSec=*/1);
    engine.init("", "samples");
    ASSERT_FALSE(engine.failedScope()) << engine.lastInitError();

    // JavaScript that loops.
    QElapsedTimer timer;
    timer.start();
    const MongoShellExecResult loop = engine.exec("var n = 0; while (true) { n++ }", "samples");
    EXPECT_TRUE(loop.error());
    EXPECT_TRUE(loop.timeoutReached());
    EXPECT_FALSE(loop.interrupted());
    EXPECT_GE(timer.elapsed(), 900);
    EXPECT_LT(timer.elapsed(), 3000);
    EXPECT_TRUE(engine.failedScope());

    // A server call that does not come back.
    engine.exec("db.getSiblingDB('rago_js_only').t.insertOne({_id: 1})", "samples");
    timer.restart();
    const MongoShellExecResult slow = engine.exec(kSlowServerCall, "samples");
    EXPECT_TRUE(slow.error());
    EXPECT_TRUE(slow.timeoutReached()) << slow.errorMessage();
    EXPECT_LT(timer.elapsed(), 3500);
    EXPECT_TRUE(serverIsQuietSoon()) << "currentOp still shows an operation of the shell";

    // And the shell works again.
    const MongoShellExecResult again = engine.exec("1 + 1", "samples");
    ASSERT_FALSE(again.error()) << again.errorMessage();
    EXPECT_EQ(again.results().back().response(), "2");
}

TEST_F(ShellJsTest, a_script_that_catches_every_exception_is_still_stopped_at_the_time_limit)
{
    if (_skip) return;
    ScriptEngine engine(_settings.get(), /*timeoutSec=*/1);
    engine.init("", "samples");
    ASSERT_FALSE(engine.failedScope()) << engine.lastInitError();
    QElapsedTimer timer;
    timer.start();
    const MongoShellExecResult res = engine.exec("var n = 0; while (true) { try { n++ } catch (e) { } }", "samples");
    EXPECT_TRUE(res.timeoutReached());
    EXPECT_LT(timer.elapsed(), 3000);
}

TEST_F(ShellJsTest, errors_are_results_and_leave_the_shell_ready)
{
    if (_skip) return;
    const struct { const char *script; const char *name; } cases[] = {
        { "this is (not valid", "SyntaxError" },
        { "undefined.x", "TypeError" },
        { "undefinedThing", "ReferenceError" },
        { "throw new Error('boom')", "Error" },
        { "db.t.find({$badoperator: 1}).toArray()", "MongoServerError" },
        { "db.runCommand({nosuchcommand: 1})", "MongoServerError" },
        { "db.t.insertOne({_id: 1}); db.t.insertOne({_id: 1})", "MongoServerError" },
    };
    for (const auto &c : cases) {
        const MongoShellExecResult res = run(c.script, "rago_js_only");
        ASSERT_FALSE(res.error()) << c.script << ": " << res.errorMessage();
        ASSERT_FALSE(res.results().empty()) << c.script;
        const MongoShellResult &r = res.results().back();
        EXPECT_EQ(r.type(), "error") << c.script;
        EXPECT_EQ(r.response().rfind(std::string(c.name) + ":", 0), 0u) << c.script << " -> " << r.response();
        // A script error is not a failed engine: no reconnect is needed.
        EXPECT_FALSE(_engine->failedScope()) << c.script;
    }
    EXPECT_EQ(output("1 + 1"), "2");
}

// JavaScript newer than Qt's engine (pasted from mongosh or the web) fails with the engine's own words; a hint says why.
TEST_F(ShellJsTest, javascript_the_engine_lacks_gets_a_hint_and_the_shell_stays_ready)
{
    if (_skip) return;
    const struct { const char *script; const char *name; const char *feature; } cases[] = {
        { "var o = {a: 1}; var p = {...o}", "SyntaxError", "object spread" },
        { "await Promise.resolve(1)", "SyntaxError", "async / await" },
        { "10n", "SyntaxError", "BigInt" },
        { "var x; x ??= 1", "SyntaxError", "logical assignment" },
        { "try { 1 } catch { 2 }", "SyntaxError", "catch clause" },
        { "'abc'.replaceAll('a', 'b')", "TypeError", "replaceAll" },
        { "[[1]].flat()", "TypeError", "flat" },
        { "Object.fromEntries([])", "TypeError", "fromEntries" },
        { "[1].at(0)", "TypeError", "at method" },
        { "globalThis", "ReferenceError", "globalThis" },
        { "setTimeout(function () {}, 1)", "ReferenceError", "setTimeout" },
    };
    for (const auto &c : cases) {
        const MongoShellExecResult res = run(c.script, "rago_js_only");
        ASSERT_FALSE(res.error()) << c.script << ": " << res.errorMessage();
        ASSERT_FALSE(res.results().empty()) << c.script;
        const MongoShellResult &r = res.results().back();
        EXPECT_EQ(r.type(), "error") << c.script;
        EXPECT_EQ(r.response().rfind(std::string(c.name) + ":", 0), 0u) << c.script << " -> " << r.response();
        EXPECT_NE(r.response().find("\nHint: "), std::string::npos) << c.script << " -> " << r.response();
        EXPECT_NE(r.response().find(c.feature), std::string::npos) << c.script << " -> " << r.response();
    }

    // Ordinary mistakes keep their plain message, and what works in the engine is not flagged.
    for (const char *script : { "undefined.x", "undefinedThing", "this is (not valid", "var a = [...[1, 2]]; a?.length ?? 0" }) {
        const MongoShellExecResult res = run(script, "rago_js_only");
        ASSERT_FALSE(res.results().empty()) << script;
        EXPECT_EQ(res.results().back().response().find("Hint: "), std::string::npos) << script;
    }
    EXPECT_EQ(output("1 + 1"), "2");
}
}
