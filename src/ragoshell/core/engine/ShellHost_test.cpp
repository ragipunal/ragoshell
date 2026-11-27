// docs/Shell.md: the BSON <-> JavaScript converter (no server needed) and the
// host object that gives JavaScript access to MongoDB (live tests: dev/mongo-test on localhost:27017
// with the "samples" database; they are skipped when the server is unreachable).

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJSEngine>
#include <QTemporaryDir>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/decimal128.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/EventBus.h"
#include "ragoshell/core/domain/MongoDocument.h"
#include "ragoshell/core/engine/BsonJsConverter.h"
#include "ragoshell/core/engine/ScriptEngine.h"
#include "ragoshell/core/engine/ShellHost.h"
#include "ragoshell/core/events/MongoShellEvents.h"
#include "ragoshell/core/mongodb/MongoWorker.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/transport/GatewaySession.h"
#include "ragoshell/core/transport/InMemoryDemoTransport.h"
#include "ragoshell/core/transport/WebSocketGatewayTransport.h"
#include "ragoshell/core/utils/AggregationPipeline.h"
#include "ragoshell/core/utils/CollectionTransfer.h"
#include "ragoshell/core/utils/SchemaAnalysis.h"

#include <QWebSocketServer>

namespace RagoShell
{
namespace
{
    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;

    void ensureApp()
    {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char appName[] = "rago_unit_tests";
            static char *argv[] = { appName, nullptr };
            static QCoreApplication app(argc, argv);
        }
    }

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
            mongocxx::client client(mongocxx::uri("mongodb://localhost:27017/?serverSelectionTimeoutMS=2000"));
            client.database("admin").run_command(make_document(kvp("ping", 1)));
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

    std::string json(bsoncxx::document::view doc)
    {
        return bsoncxx::to_json(doc, bsoncxx::ExtendedJsonMode::k_canonical);
    }

    std::string keysOf(bsoncxx::document::view doc)
    {
        std::string keys;
        for (const auto &element : doc)
            keys += std::string(element.key()) + ",";
        return keys;
    }
}

// ---- the converter ----------------------------------------------------------------------------

class BsonJsConverterTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        _engine = std::make_unique<QJSEngine>();
        _bson = std::make_unique<BsonJsConverter>(_engine.get());
    }

    QJSValue eval(const char *code)
    {
        QJSValue result = _engine->evaluate(QString::fromUtf8(code));
        EXPECT_FALSE(result.isError()) << result.toString().toStdString();
        return result;
    }

    std::unique_ptr<QJSEngine> _engine;
    std::unique_ptr<BsonJsConverter> _bson;
};

TEST_F(BsonJsConverterTest, every_bson_type_survives_a_trip_through_javascript)
{
    using namespace bsoncxx::types;
    const unsigned char bytes[] = { 0, 1, 2, 250, 255 };
    const unsigned char uuid[16] = { 0x12, 0x3e, 0x45, 0x67, 0xe8, 0x9b, 0x12, 0xd3,
                                     0xa4, 0x56, 0x42, 0x66, 0x14, 0x17, 0x40, 0x00 };

    bsoncxx::builder::basic::document builder;
    builder.append(
        kvp("double", 1.5),
        kvp("string", "caf\xc3\xa9 \xe2\x82\xac"),
        kvp("document", make_document(kvp("inner", make_document(kvp("deep", 1))))),
        kvp("array", bsoncxx::builder::basic::make_array(1, "two", make_document(kvp("three", 3)), 4.5)),
        kvp("binary", b_binary{ bsoncxx::binary_sub_type::k_binary, sizeof(bytes), bytes }),
        kvp("uuid", b_binary{ bsoncxx::binary_sub_type::k_uuid, sizeof(uuid), uuid }),
        kvp("oid", bsoncxx::oid("507f1f77bcf86cd799439011")),
        kvp("true", true),
        kvp("false", false),
        kvp("date", b_date{ std::chrono::milliseconds(1700000000123) }),
        kvp("before1970", b_date{ std::chrono::milliseconds(-86400000123) }),
        kvp("null", b_null{}),
        kvp("regex", b_regex{ "^a.*b$", "i" }),
        kvp("regexX", b_regex{ "a b", "ix" }),
        kvp("dbpointer", b_dbpointer{ "things", bsoncxx::oid("507f1f77bcf86cd799439012") }),
        kvp("code", b_code{ "function () { return 1; }" }),
        kvp("symbol", b_symbol{ "sym" }),
        kvp("codeWithScope", b_codewscope{ "x + 1", make_document(kvp("x", 41)) }),
        kvp("int32", 2147483647),
        kvp("int32min", static_cast<std::int32_t>(-2147483647 - 1)),
        kvp("timestamp", b_timestamp{ 6, 5 }),
        kvp("int64", static_cast<std::int64_t>(9007199254740993LL)),
        kvp("int64min", static_cast<std::int64_t>(-9223372036854775807LL - 1)),
        kvp("int64max", static_cast<std::int64_t>(9223372036854775807LL)),
        kvp("decimal", b_decimal128{ bsoncxx::decimal128("0.1") }),
        kvp("decimalBig", b_decimal128{ bsoncxx::decimal128("1.234567890123456789012345678901234E+6000") }),
        kvp("minkey", b_minkey{}),
        kvp("maxkey", b_maxkey{}),
        kvp("emptyDoc", make_document()),
        kvp("emptyArray", bsoncxx::builder::basic::make_array()));
    const bsoncxx::document::value original = builder.extract();

    const QJSValue js = _bson->toJs(original.view());
    const bsoncxx::document::value back = _bson->toDocument(js);

    EXPECT_TRUE(back.view() == original.view()) << "before: " << json(original.view()) << "\nafter:  " << json(back.view());
}

TEST_F(BsonJsConverterTest, int64_never_becomes_a_number_and_keeps_every_digit)
{
    const bsoncxx::document::value doc = make_document(kvp("big", static_cast<std::int64_t>(9007199254740993LL)));
    _engine->globalObject().setProperty(QStringLiteral("doc"), _bson->toJs(doc.view()));

    EXPECT_EQ(eval("doc.big.value").toString().toStdString(), "9007199254740993");
    EXPECT_EQ(eval("typeof doc.big").toString().toStdString(), "object");
    // valueOf() gives a number for arithmetic and comparisons.
    EXPECT_DOUBLE_EQ(eval("doc.big + 1").toNumber(), 9007199254740992.0 + 2);
    EXPECT_TRUE(eval("doc.big > 5").toBool());
    // The Node driver would have rounded this; the text form is exact.
    EXPECT_EQ(eval("new __bson.Long('9223372036854775807').toString()").toString().toStdString(), "9223372036854775807");
}

TEST_F(BsonJsConverterTest, javascript_values_become_the_matching_bson_types)
{
    const QJSValue js = eval(R"JS((function () {
        return {
            a: 1, b: 1.5, c: "x", d: true, e: null, f: undefined,
            g: new Date(1700000000123),
            h: [1, [2, 3], { k: 4 }, null],
            i: /ab+c/gi,
            j: new __bson.Long("-9223372036854775808"),
            k: new __bson.Decimal128("0.1"),
            l: new __bson.ObjectId("507F1F77BCF86CD799439011"),
            m: new __bson.Timestamp(5, 6),
            n: new __bson.Int32(7),
            o: new __bson.Double(8),
            p: function () { return 1; },
            q: new __bson.Binary(4, "Ej5FZ+ibEtOkVkJmFBdAAA=="),
            r: new __bson.Code("x + 1", { x: 41 }),
            s: new __bson.MinKey(), t: new __bson.MaxKey(),
            u: { $ref: "things", $id: new __bson.ObjectId("507f1f77bcf86cd799439011"), $db: "samples" }
        };
    })())JS");
    const bsoncxx::document::value doc = _bson->toDocument(js);
    const bsoncxx::document::view v = doc.view();

    EXPECT_EQ(keysOf(v), "a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,q,r,s,t,u,"); // p is a function: left out
    EXPECT_EQ(v["a"].type(), bsoncxx::type::k_int32);
    EXPECT_EQ(v["b"].type(), bsoncxx::type::k_double);
    EXPECT_EQ(v["c"].get_string().value, "x");
    EXPECT_EQ(v["d"].type(), bsoncxx::type::k_bool);
    EXPECT_EQ(v["e"].type(), bsoncxx::type::k_null);
    EXPECT_EQ(v["f"].type(), bsoncxx::type::k_null); // undefined is stored as null, as the Node driver does
    EXPECT_EQ(v["g"].get_date().value.count(), 1700000000123LL);
    EXPECT_EQ(v["h"].type(), bsoncxx::type::k_array);
    EXPECT_EQ(std::distance(v["h"].get_array().value.begin(), v["h"].get_array().value.end()), 4);
    EXPECT_EQ(v["i"].get_regex().regex, "ab+c");
    EXPECT_EQ(v["i"].get_regex().options, "i"); // g means nothing to a query
    EXPECT_EQ(v["j"].get_int64().value, std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(v["k"].get_decimal128().value.to_string(), "0.1");
    EXPECT_EQ(v["l"].get_oid().value.to_string(), "507f1f77bcf86cd799439011");
    EXPECT_EQ(v["m"].get_timestamp().timestamp, 5u);
    EXPECT_EQ(v["m"].get_timestamp().increment, 6u);
    EXPECT_EQ(v["n"].type(), bsoncxx::type::k_int32);
    EXPECT_EQ(v["o"].type(), bsoncxx::type::k_double); // an explicit Double stays a double
    EXPECT_EQ(v["q"].get_binary().size, 16u);
    EXPECT_EQ(v["q"].get_binary().sub_type, bsoncxx::binary_sub_type::k_uuid);
    EXPECT_EQ(v["r"].type(), bsoncxx::type::k_codewscope);
    EXPECT_EQ(v["s"].type(), bsoncxx::type::k_minkey);
    EXPECT_EQ(v["t"].type(), bsoncxx::type::k_maxkey);
    EXPECT_EQ(v["u"].type(), bsoncxx::type::k_document); // a DBRef is an ordinary document
}

TEST_F(BsonJsConverterTest, field_order_is_kept)
{
    const bsoncxx::document::value doc = make_document(kvp("zebra", 1), kvp("apple", 2), kvp("mango", make_document(kvp("y", 1), kvp("b", 2))));
    const bsoncxx::document::value back = _bson->toDocument(_bson->toJs(doc.view()));
    EXPECT_EQ(keysOf(back.view()), "zebra,apple,mango,");
    EXPECT_EQ(keysOf(back.view()["mango"].get_document().value), "y,b,");
}

TEST_F(BsonJsConverterTest, numbers_become_int32_when_they_can_else_double)
{
    const QJSValue js = eval("({ i: 5, max: 2147483647, over: 2147483648, min: -2147483648, under: -2147483649,"
                             " frac: 0.5, big: 1e10, nz: -0, nan: NaN, inf: Infinity, zero: 0 })");
    const bsoncxx::document::value doc = _bson->toDocument(js);
    const bsoncxx::document::view v = doc.view();
    EXPECT_EQ(v["i"].type(), bsoncxx::type::k_int32);
    EXPECT_EQ(v["max"].type(), bsoncxx::type::k_int32);
    EXPECT_EQ(v["over"].type(), bsoncxx::type::k_double);
    EXPECT_EQ(v["min"].type(), bsoncxx::type::k_int32);
    EXPECT_EQ(v["under"].type(), bsoncxx::type::k_double);
    EXPECT_EQ(v["frac"].type(), bsoncxx::type::k_double);
    EXPECT_EQ(v["big"].type(), bsoncxx::type::k_double);
    EXPECT_EQ(v["nz"].type(), bsoncxx::type::k_double);
    EXPECT_EQ(v["nan"].type(), bsoncxx::type::k_double);
    EXPECT_EQ(v["inf"].type(), bsoncxx::type::k_double);
    EXPECT_EQ(v["zero"].type(), bsoncxx::type::k_int32);
}

TEST_F(BsonJsConverterTest, a_bson_regex_that_javascript_cannot_hold_stays_a_bson_regex)
{
    const bsoncxx::document::value doc = make_document(
        kvp("plain", bsoncxx::types::b_regex{ "^a", "im" }),
        kvp("extended", bsoncxx::types::b_regex{ "a b # comment", "x" }));
    _engine->globalObject().setProperty(QStringLiteral("doc"), _bson->toJs(doc.view()));
    EXPECT_TRUE(eval("doc.plain instanceof RegExp").toBool());
    EXPECT_EQ(eval("doc.plain.flags").toString().toStdString(), "im");
    EXPECT_EQ(eval("doc.extended._bsontype").toString().toStdString(), "BsonRegExp");
}

TEST_F(BsonJsConverterTest, bson_undefined_reads_as_undefined)
{
    bsoncxx::builder::basic::document builder;
    builder.append(kvp("u", bsoncxx::types::b_undefined{}));
    const bsoncxx::document::value doc = builder.extract();
    _engine->globalObject().setProperty(QStringLiteral("doc"), _bson->toJs(doc.view()));
    EXPECT_TRUE(eval("doc.u === undefined && 'u' in doc").toBool());
}

TEST_F(BsonJsConverterTest, what_bson_cannot_hold_is_refused_with_a_clear_message)
{
    auto message = [&](const char *code) -> std::string {
        try {
            _bson->toDocument(eval(code));
        } catch (const BsonConversionError &e) {
            return e.what();
        }
        return "no error";
    };

    EXPECT_NE(message("({ a: new __bson.Long('9223372036854775808') })").find("64-bit"), std::string::npos);
    EXPECT_NE(message("({ a: new __bson.Decimal128('not a number') })").find("Decimal128"), std::string::npos);
    EXPECT_NE(message("({ a: /x/y })").find("'y'"), std::string::npos);
    EXPECT_NE(message("({ a: new Date(NaN) })").find("Invalid Date"), std::string::npos);
    EXPECT_NE(message("({ a: new __bson.Binary(300, '') })").find("0 to 255"), std::string::npos);
    EXPECT_NE(message("(function () { var o = {}; o.self = o; return o; })()").find("refers to itself"), std::string::npos);
    EXPECT_NE(message("5").find("plain object"), std::string::npos);
    EXPECT_NE(message("[1, 2]").find("plain object"), std::string::npos);
    EXPECT_NE(message("new Date()").find("plain object"), std::string::npos);
}

TEST_F(BsonJsConverterTest, nested_documents_up_to_the_bson_limit_work)
{
    const QJSValue ok = eval("(function () { var o = {}, c = o; for (var i = 0; i < 90; i++) { c.x = {}; c = c.x; } return o; })()");
    EXPECT_NO_THROW(_bson->toDocument(ok));
    const QJSValue tooDeep = eval("(function () { var o = {}, c = o; for (var i = 0; i < 150; i++) { c.x = {}; c = c.x; } return o; })()");
    EXPECT_THROW(_bson->toDocument(tooDeep), BsonConversionError);
}

TEST_F(BsonJsConverterTest, extended_json_text_comes_in_both_flavours)
{
    const bsoncxx::document::value doc = make_document(kvp("n", static_cast<std::int64_t>(5)), kvp("s", "x"));
    EXPECT_EQ(BsonJsConverter::toJson(doc.view(), true), "{ \"n\" : 5, \"s\" : \"x\" }");
    EXPECT_NE(BsonJsConverter::toJson(doc.view(), false).find("$numberLong"), std::string::npos);
}

// ---- the host ---------------------------------------------------------------------------------

class ShellHostLiveTest : public ::testing::Test
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
        _engine = std::make_unique<QJSEngine>();
        _host = std::make_unique<ShellHost>(_engine.get());
        _host->install();

        ShellConnectOptions options;
        options.uri = QStringLiteral("mongodb://localhost:27017/");
        options.directConnection = true;
        options.serverSelectionTimeoutMs = 3000;
        QString error;
        ASSERT_TRUE(_host->connect(options, &error)) << error.toStdString();
    }

    void TearDown() override
    {
        if (_host && _host->isConnected()) {
            _host->runCommand(QStringLiteral("rago_js_host_test"), _host->converter().toJs(make_document(kvp("dropDatabase", 1)).view()));
        }
    }

    QJSValue eval(const char *code)
    {
        QJSValue result = _engine->evaluate(QString::fromUtf8(code));
        EXPECT_FALSE(result.isError()) << result.toString().toStdString();
        return result;
    }

    std::unique_ptr<QJSEngine> _engine;
    std::unique_ptr<ShellHost> _host;
    bool _skip = false;
};

TEST_F(ShellHostLiveTest, find_gives_a_cursor_whose_documents_reach_javascript_and_cpp)
{
    if (_skip) return;

    const int forJs = eval("__host.find('samples', 'restaurants', { cuisine: 'Italian' }, { limit: 5, sort: { _id: 1 } })").toInt();
    ASSERT_GT(forJs, 0);
    EXPECT_TRUE(eval(QString("__host.cursorHasNext(%1)").arg(forJs).toUtf8().constData()).toBool());
    const QJSValue docs = eval(QString("__host.cursorNext(%1, 10)").arg(forJs).toUtf8().constData());
    ASSERT_EQ(docs.property("length").toInt(), 5);
    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(docs.property(i).property("cuisine").toString().toStdString(), "Italian");
    EXPECT_FALSE(eval(QString("__host.cursorHasNext(%1)").arg(forJs).toUtf8().constData()).toBool());

    const int forCpp = eval("__host.find('samples', 'restaurants', { cuisine: 'Italian' }, { limit: 5, sort: { _id: 1 } })").toInt();
    bool exhausted = false;
    const std::vector<MongoDocumentPtr> viaCpp = _host->takeDocuments(forCpp, 0, &exhausted);
    ASSERT_EQ(viaCpp.size(), 5u);
    EXPECT_TRUE(exhausted);
    for (int i = 0; i < 5; ++i) {
        // The same documents, from BSON straight to MongoDocument and through JavaScript.
        const auto id = viaCpp[static_cast<size_t>(i)]->bsonObj()["_id"];
        EXPECT_EQ(id.get_oid().value.to_string(), docs.property(i).property("_id").property("hex").toString().toStdString());
        EXPECT_EQ(std::string(viaCpp[static_cast<size_t>(i)]->bsonObj()["name"].get_string().value),
                  docs.property(i).property("name").toString().toStdString());
    }
}

TEST_F(ShellHostLiveTest, a_cursor_hands_out_documents_in_batches_and_can_be_closed)
{
    if (_skip) return;
    const int id = eval("__host.find('samples', 'restaurants', {}, { limit: 7, batchSize: 2 })").toInt();
    EXPECT_EQ(eval(QString("__host.cursorNext(%1, 3).length").arg(id).toUtf8().constData()).toInt(), 3);
    EXPECT_EQ(eval(QString("__host.cursorNext(%1, 3).length").arg(id).toUtf8().constData()).toInt(), 3);
    EXPECT_EQ(eval(QString("__host.cursorNext(%1, 3).length").arg(id).toUtf8().constData()).toInt(), 1);

    const int other = eval("__host.find('samples', 'restaurants', {})").toInt();
    eval(QString("__host.cursorClose(%1)").arg(other).toUtf8().constData());
    EXPECT_EQ(eval(QString("(function () { try { __host.cursorNext(%1, 1); return 'no error'; }"
                           " catch (e) { return e.message; } })()").arg(other).toUtf8().constData())
                  .toString().toStdString(),
              "unknown or closed cursor");
}

TEST_F(ShellHostLiveTest, server_errors_become_javascript_exceptions_with_code_and_name)
{
    if (_skip) return;
    EXPECT_EQ(eval("(function () { try { __host.runCommand('admin', { nosuchcommand: 1 }); return 'no error'; }"
                   " catch (e) { return e.code + ':' + e.codeName; } })()").toString().toStdString(),
              "59:CommandNotFound");
    EXPECT_EQ(eval("(function () { try { __host.find('samples', 'restaurants', { $badoperator: 1 }); "
                   "__host.cursorNext(__host.find('samples', 'restaurants', { $badoperator: 1 }), 1); return 'no error'; }"
                   " catch (e) { return typeof e.code; } })()").toString().toStdString(),
              "number");
    EXPECT_EQ(eval("(function () { try { __host.runCommand('admin', 5); return 'no error'; }"
                   " catch (e) { return e.message; } })()").toString().toStdString().find("plain object") != std::string::npos,
              true);
}

TEST_F(ShellHostLiveTest, aggregate_count_distinct_and_listings)
{
    if (_skip) return;
    const int agg = eval("__host.aggregate('samples', 'restaurants', [{ $group: { _id: '$cuisine', n: { $sum: 1 } } }, { $sort: { n: -1 } }, { $limit: 3 }])").toInt();
    const QJSValue groups = eval(QString("__host.cursorNext(%1, 0)").arg(agg).toUtf8().constData());
    EXPECT_EQ(groups.property("length").toInt(), 3);
    EXPECT_TRUE(groups.property(0).property("n").toNumber() >= groups.property(1).property("n").toNumber());

    EXPECT_GT(eval("__host.countDocuments('samples', 'restaurants', { cuisine: 'Italian' })").toNumber(), 0);
    EXPECT_EQ(eval("__host.countDocuments('samples', 'restaurants', { cuisine: 'Italian' }, { limit: 2 })").toNumber(), 2);
    EXPECT_GT(eval("__host.estimatedDocumentCount('samples', 'restaurants')").toNumber(), 0);
    EXPECT_TRUE(eval("__host.distinct('samples', 'restaurants', 'cuisine').indexOf('Italian') >= 0").toBool());
    EXPECT_TRUE(eval("__host.distinct('samples', 'restaurants', 'cuisine', { cuisine: 'Italian' }).length === 1").toBool());
    EXPECT_TRUE(eval("__host.listCollections('samples').some(function (c) { return c.name === 'restaurants'; })").toBool());
    EXPECT_TRUE(eval("__host.listIndexes('samples', 'restaurants').some(function (i) { return i.name === '_id_'; })").toBool());
    EXPECT_FALSE(eval("__host.serverVersion()").toString().isEmpty());
}

TEST_F(ShellHostLiveTest, int64_decimal_and_dates_survive_a_write_and_a_read)
{
    if (_skip) return;
    eval("__host.runCommand('rago_js_host_test', { insert: 't', documents: [{"
         " _id: 1, big: new __bson.Long('9007199254740993'), dec: new __bson.Decimal128('0.1'),"
         " when: new Date(1700000000123), oid: new __bson.ObjectId('507f1f77bcf86cd799439011'),"
         " bin: new __bson.Binary(0, 'AAECA/r/') }] })");
    const int id = eval("__host.find('rago_js_host_test', 't', { _id: 1 })").toInt();
    const std::vector<MongoDocumentPtr> docs = _host->takeDocuments(id, 0);
    ASSERT_EQ(docs.size(), 1u);
    const bsoncxx::document::view d = docs.front()->bsonObj();
    EXPECT_EQ(d["big"].type(), bsoncxx::type::k_int64);
    EXPECT_EQ(d["big"].get_int64().value, 9007199254740993LL);
    EXPECT_EQ(d["dec"].get_decimal128().value.to_string(), "0.1");
    EXPECT_EQ(d["when"].get_date().value.count(), 1700000000123LL);
    EXPECT_EQ(d["oid"].get_oid().value.to_string(), "507f1f77bcf86cd799439011");
    EXPECT_EQ(d["bin"].get_binary().size, 6u);

    // And the way back into JavaScript keeps the digits.
    const int again = eval("__host.find('rago_js_host_test', 't', { _id: 1 })").toInt();
    const QJSValue js = eval(QString("__host.cursorNext(%1, 1)[0]").arg(again).toUtf8().constData());
    EXPECT_EQ(js.property("big").property("value").toString().toStdString(), "9007199254740993");
    EXPECT_EQ(js.property("dec").property("value").toString().toStdString(), "0.1");
}

TEST_F(ShellHostLiveTest, extended_json_text_of_any_value)
{
    if (_skip) return;
    EXPECT_EQ(eval("__host.toEjson({ a: 1, b: [1, 2] })").toString().toStdString(), "{ \"a\" : 1, \"b\" : [ 1, 2 ] }");
    EXPECT_EQ(eval("__host.toEjson([1, 'x'])").toString().toStdString(), "[ 1, \"x\" ]");
    EXPECT_EQ(eval("__host.toEjson('text')").toString().toStdString(), "\"text\"");
    EXPECT_EQ(eval("__host.toEjson(5)").toString().toStdString(), "5");
    EXPECT_NE(eval("__host.toEjson({ n: new __bson.Long('5') }, false)").toString().toStdString().find("$numberLong"), std::string::npos);
    EXPECT_EQ(eval("__host.toEjson({ n: new __bson.Long('5') })").toString().toStdString(), "{ \"n\" : 5 }");
}

TEST(ShellHostTest, a_new_object_id_is_24_hex_characters_and_unconnected_calls_fail_cleanly)
{
    ensureApp();
    QJSEngine engine;
    ShellHost host(&engine);
    host.install();
    EXPECT_FALSE(host.isConnected());
    EXPECT_TRUE(engine.evaluate("/^[0-9a-f]{24}$/.test(__host.newObjectId())").toBool());
    EXPECT_TRUE(engine.evaluate("/^[0-9a-f]{24}$/.test(new __bson.ObjectId().hex)").toBool());
    EXPECT_EQ(engine.evaluate("(function () { try { __host.find('a', 'b', {}); return 'no error'; }"
                              " catch (e) { return e.message; } })()").toString().toStdString(),
              "not connected to a MongoDB server");
}

TEST(ShellHostTest, a_failed_connection_reports_why_and_stays_disconnected)
{
    ensureApp();
    ensureMongoInstance();
    QJSEngine engine;
    ShellHost host(&engine);

    ShellConnectOptions options;
    options.uri = QStringLiteral("mongodb://192.0.2.1:27017/"); // TEST-NET-1: nobody answers
    options.serverSelectionTimeoutMs = 600;
    QString error;
    EXPECT_FALSE(host.connect(options, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(host.isConnected());
}

// ---- transport-backed shell (W4, docs/TransportShell.md) --------------------------------------

class ShellTransportTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        _transport = std::make_unique<InMemoryDemoTransport>();
        ASSERT_TRUE(_transport->connect(QStringLiteral("{}")).ok);
        _conn = std::make_unique<ConnectionSettings>(true);
        _conn->setDefaultDatabase("catalog");
        _conn->setTransportKind("memory");
        _shell = std::make_unique<ScriptEngine>(_conn.get(), 10);
        _shell->setTransport(_transport.get());
        _shell->init("", "catalog");
        ASSERT_FALSE(_shell->failedScope()) << _shell->lastInitError();
    }

    std::unique_ptr<InMemoryDemoTransport> _transport;
    std::unique_ptr<ConnectionSettings> _conn;
    std::unique_ptr<ScriptEngine> _shell;
};

TEST_F(ShellTransportTest, find_sort_projection_and_paging_items)
{
    const MongoShellExecResult res = _shell->exec(
        "db.products.find({ price: { $gt: 50 } }, { _id: 0, sku: 1, price: 1 }).sort({ price: -1 })",
        "catalog");
    ASSERT_FALSE(res.error()) << res.errorMessage();
    ASSERT_EQ(res.results().size(), 1u);
    const auto &docs = res.results()[0].documents();
    ASSERT_EQ(docs.size(), 2u);
    EXPECT_EQ(std::string(docs[0]->bsonObj()["sku"].get_string().value), "MN-27");
    EXPECT_EQ(std::string(docs[1]->bsonObj()["sku"].get_string().value), "KB-01");
    EXPECT_FALSE(docs[0]->bsonObj()["_id"]);
    EXPECT_EQ(res.currentServer(), "demo");
    EXPECT_EQ(res.currentDatabase(), "catalog");
}

TEST_F(ShellTransportTest, crud_methods_bulkwrite_and_duplicate_key_error)
{
    const MongoShellExecResult writeRes = _shell->exec(R"JS(
        var ins = db.products.insertOne({ _id: 10, sku: "WC-4K", name: "4K Webcam", price: 149.0, stock: 8, tags: ["video"] });
        var upd = db.products.updateOne({ _id: 10 }, { $inc: { stock: 4 }, $push: { tags: "usb" } });
        var after = db.products.findOneAndUpdate({ _id: 10 }, { $set: { active: true } }, { returnNewDocument: true });
        var bw = db.products.bulkWrite([
            { insertOne: { document: { _id: 11, sku: "MC-01", name: "Studio Mic", price: 119.0, stock: 5 } } },
            { deleteOne: { filter: { _id: 11 } } }
        ]);
        ({ insertedId: ins.insertedId, modified: upd.modifiedCount, stock: after.stock, bwInserted: bw.insertedCount, bwDeleted: bw.deletedCount })
    )JS", "catalog");
    ASSERT_FALSE(writeRes.error()) << writeRes.errorMessage();
    ASSERT_EQ(writeRes.results().size(), 1u);
    ASSERT_EQ(writeRes.results()[0].documents().size(), 1u);
    const auto summary = writeRes.results()[0].documents()[0]->bsonObj();
    EXPECT_EQ(summary["insertedId"].get_int32().value, 10);
    EXPECT_EQ(summary["modified"].get_int32().value, 1);
    EXPECT_EQ(summary["stock"].get_int32().value, 12);
    EXPECT_EQ(summary["bwInserted"].get_int32().value, 1);
    EXPECT_EQ(summary["bwDeleted"].get_int32().value, 1);

    // Duplicate key surfaces as MongoServerError with code 11000 in JS catch.
    const MongoShellExecResult dupRes = _shell->exec(R"JS(
        (function () {
            try {
                db.products.insertOne({ _id: 1, sku: "OTHER" });
                return "unexpected";
            } catch (e) {
                return e.name + ":" + e.code;
            }
        })()
    )JS", "catalog");
    ASSERT_FALSE(dupRes.error());
    ASSERT_EQ(dupRes.results().size(), 1u);
    EXPECT_NE(dupRes.results()[0].response().find("MongoServerError:11000"), std::string::npos)
        << dupRes.results()[0].response();
}

TEST_F(ShellTransportTest, aggregation_distinct_count_indexes_explain_and_completion)
{
    const MongoShellExecResult aggRes = _shell->exec(R"JS(
        db.orders.aggregate([
            { $group: { _id: "$customerId", revenue: { $sum: "$total" }, count: { $sum: 1 } } },
            { $sort: { revenue: -1 } }
        ])
    )JS", "catalog");
    ASSERT_FALSE(aggRes.error()) << aggRes.errorMessage();
    ASSERT_EQ(aggRes.results().size(), 1u);
    ASSERT_EQ(aggRes.results()[0].documents().size(), 2u);
    EXPECT_EQ(std::string(aggRes.results()[0].documents()[0]->bsonObj()["_id"].get_string().value), "c1");

    const MongoShellExecResult metaRes = _shell->exec(R"JS(
        db.products.createIndex({ category: 1 });
        ({
            c1Orders: db.orders.countDocuments({ customerId: "c1" }),
            estProducts: db.products.estimatedDocumentCount(),
            tagCount: db.products.distinct("tags").length,
            idxCount: db.products.getIndexes().length,
            ver: db.version(),
            planOk: db.products.find({ sku: "KB-01" }).explain().ok
        })
    )JS", "catalog");
    ASSERT_FALSE(metaRes.error()) << metaRes.errorMessage();
    ASSERT_EQ(metaRes.results().size(), 2u);
    EXPECT_EQ(metaRes.results()[0].response(), "\"category_1\"");
    ASSERT_EQ(metaRes.results()[1].documents().size(), 1u);
    const auto meta = metaRes.results()[1].documents()[0]->bsonObj();
    EXPECT_EQ(meta["c1Orders"].get_int32().value, 2);
    EXPECT_EQ(meta["estProducts"].get_int32().value, 3);
    EXPECT_EQ(meta["tagCount"].get_int32().value, 4);
    EXPECT_EQ(meta["idxCount"].get_int32().value, 3); // _id_, sku_1, category_1
    EXPECT_EQ(std::string(meta["ver"].get_string().value), "8.0.0-demo");
    EXPECT_EQ(meta["planOk"].get_int32().value, 1);

    const QStringList completions = _shell->complete("db.", AutocompleteAll);
    EXPECT_TRUE(completions.contains(QStringLiteral("db.products")));
    EXPECT_TRUE(completions.contains(QStringLiteral("db.customers")));
    EXPECT_TRUE(completions.contains(QStringLiteral("db.orders")));
}

TEST_F(ShellTransportTest, int64_decimal128_and_dates_roundtrip_without_precision_loss)
{
    const MongoShellExecResult res = _shell->exec(R"JS(
        db.ledger.insertOne({
            _id: ObjectId("507f1f77bcf86cd799439011"),
            big: NumberLong("9007199254740993"),
            dec: NumberDecimal("124.50"),
            at: ISODate("2026-11-26T12:00:00.123Z")
        });
        db.ledger.find({ big: NumberLong("9007199254740993") })
    )JS", "catalog");
    ASSERT_FALSE(res.error()) << res.errorMessage();
    ASSERT_EQ(res.results().size(), 2u);
    const auto &docs = res.results()[1].documents();
    ASSERT_EQ(docs.size(), 1u);
    const auto d = docs[0]->bsonObj();
    EXPECT_EQ(d["_id"].get_oid().value.to_string(), "507f1f77bcf86cd799439011");
    EXPECT_EQ(d["big"].type(), bsoncxx::type::k_int64);
    EXPECT_EQ(d["big"].get_int64().value, 9007199254740993LL);
    EXPECT_EQ(d["dec"].type(), bsoncxx::type::k_decimal128);
    EXPECT_EQ(d["dec"].get_decimal128().value.to_string(), "124.50");
    EXPECT_EQ(d["at"].type(), bsoncxx::type::k_date);
}

namespace
{
    class PagedMockTransport final : public IMongoTransport
    {
    public:
        Result connect(const QString &) override { return { true, R"({"ok":1})", {}, 0, {} }; }
        Result listDatabases() override { return { true, R"([])", {}, 0, {} }; }
        Result listCollections(const QString &) override { return { true, R"({"cursor":{"firstBatch":[],"id":0},"ok":1})", {}, 0, {} }; }
        Result ping() override { return { true, R"({"ok":1})", {}, 0, {} }; }
        Result killOp(const QString &) override { return { true, R"({"ok":1})", {}, 0, {} }; }
        void disconnect() override {}
        bool isLive() const override { return true; }

        Result runCommand(const QString &, const QString &ejsonCommand) override
        {
            const auto cmd = bsoncxx::from_json(ejsonCommand.toStdString());
            if (cmd.view()["count"]) {
                return { true, R"({"n":3,"ok":1})", {}, 0, {} };
            }
            if (cmd.view()["find"]) {
                return { true, R"({"cursor":{"firstBatch":[{"_id":1},{"_id":2}],"id":{"$numberLong":"77"},"ns":"test.items"},"ok":1})", {}, 0, {} };
            }
            if (cmd.view()["getMore"]) {
                ++getMoreCalls;
                return { true, R"({"cursor":{"nextBatch":[{"_id":3}],"id":{"$numberLong":"0"},"ns":"test.items"},"ok":1})", {}, 0, {} };
            }
            if (cmd.view()["killCursors"]) {
                ++killCursorCalls;
                return { true, R"({"cursorsKilled":[{"$numberLong":"77"}],"ok":1})", {}, 0, {} };
            }
            return { true, R"({"ok":1})", {}, 0, {} };
        }

        int getMoreCalls = 0;
        int killCursorCalls = 0;
    };
}

TEST(ShellHostTransportCursorTest, getmore_drains_batches_and_close_sends_killcursors)
{
    ensureApp();
    PagedMockTransport mock;
    QJSEngine engine;
    ShellHost host(&engine);
    host.install();

    ShellConnectOptions opts;
    opts.transport = &mock;
    opts.transportAddress = QStringLiteral("gateway");
    QString err;
    ASSERT_TRUE(host.connect(opts, &err)) << err.toStdString();

    const int cid1 = engine.evaluate("__host.find('test', 'items', {})").toInt();
    ASSERT_GT(cid1, 0);
    const auto all = host.takeDocuments(cid1, 0);
    EXPECT_EQ(all.size(), 3u);
    EXPECT_EQ(mock.getMoreCalls, 1);
    EXPECT_EQ(mock.killCursorCalls, 0); // cursor already exhausted (id == 0)
    host.cursorClose(cid1);

    // Closing an unexhausted remote cursor sends killCursors.
    const int cid2 = engine.evaluate("__host.find('test', 'items', {})").toInt();
    ASSERT_GT(cid2, 0);
    const auto partial = host.takeDocuments(cid2, 1);
    EXPECT_EQ(partial.size(), 1u);
    host.cursorClose(cid2);
    EXPECT_EQ(mock.killCursorCalls, 1);
}

TEST(ShellHostTransportSecurityTest, fail_closed_without_transport_and_schema_analysis_over_demo)
{
    ensureApp();
    ConnectionSettings desktopMem(true);
    desktopMem.setServerHost("127.0.0.1");
    desktopMem.setServerPort(27017);
    desktopMem.setTransportKind("memory");

    // Without an explicitly bound IMongoTransport, ScriptEngine must refuse
    // rather than falling back to mongocxx on localhost:27017 (T1/T2).
    ScriptEngine engine(&desktopMem, 5);
    engine.init("", "catalog");
    EXPECT_TRUE(engine.failedScope());
    const auto execRes = engine.exec("db.products.find({})", "catalog");
    EXPECT_TRUE(execRes.error());

    // SchemaAnalysis on desktop refuses hasTransport() ConnectionSettings
    // without touching TCP, while the explicit IMongoTransport overload works.
    const SchemaResult denied = SchemaAnalysis::analyzeCollection(
        desktopMem, QStringLiteral("catalog"), QStringLiteral("products"), 50, 2000,
        DefaultEncoding, Utc);
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(SchemaAnalysis::estimatedDocumentCount(
                  desktopMem, QStringLiteral("catalog"), QStringLiteral("products"), 2000),
              -1);

    InMemoryDemoTransport demo;
    ASSERT_TRUE(demo.connect(QStringLiteral("{}")).ok);
    const auto listed = SchemaAnalysis::listCollections(demo, QStringLiteral("catalog"), 2000);
    ASSERT_TRUE(listed.ok) << listed.error.toStdString();
    EXPECT_EQ(listed.collections.size(), 3u);

    const SchemaResult analyzed = SchemaAnalysis::analyzeCollection(
        demo, QStringLiteral("catalog"), QStringLiteral("products"), 50, 2000);
    ASSERT_TRUE(analyzed.ok) << analyzed.error.toStdString();
    EXPECT_EQ(analyzed.totalDocuments, 3);
    EXPECT_TRUE(analyzed.fullScan);
    EXPECT_TRUE(analyzed.indexes.contains(QStringLiteral("{ \"sku\" : 1 }")));

    // SchemaAnalysis over paged IMongoTransport drains getMore batches (`L1.2`).
    PagedMockTransport paged;
    const SchemaResult pagedSchema = SchemaAnalysis::analyzeCollection(
        paged, QStringLiteral("test"), QStringLiteral("items"), 50, 2000);
    ASSERT_TRUE(pagedSchema.ok) << pagedSchema.error.toStdString();
    EXPECT_EQ(pagedSchema.sampledDocuments, 3);
    EXPECT_GE(paged.getMoreCalls, 1);

    // CollectionTransfer & AggregationPipeline fail closed on `hasTransport()` (`H2`).
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString transferErr;
    EXPECT_EQ(CollectionTransfer::exportCollectionToFile(
                  desktopMem, QStringLiteral("catalog"), QStringLiteral("products"),
                  tmpDir.filePath(QStringLiteral("out.json")),
                  CollectionTransfer::Format::JsonArray, QString(), 0, nullptr, nullptr, &transferErr),
              -1);
    EXPECT_FALSE(transferErr.isEmpty());

    transferErr.clear();
    EXPECT_EQ(CollectionTransfer::importFileToCollection(
                  desktopMem, QStringLiteral("catalog"), QStringLiteral("products"),
                  tmpDir.filePath(QStringLiteral("in.json")),
                  CollectionTransfer::Format::JsonArray, false, false, nullptr, nullptr, &transferErr),
              -1);
    EXPECT_FALSE(transferErr.isEmpty());

    const auto previewDenied = AggregationPipeline::runPreview(
        desktopMem, QStringLiteral("catalog"), QStringLiteral("products"), {}, 10, 1000);
    EXPECT_FALSE(previewDenied.ok);
    EXPECT_FALSE(previewDenied.error.isEmpty());

    // Non-finite / extreme JS numeric options on ShellHost do not trap or UB (`H3`),
    // and URI userinfo is redacted from transport error messages (`L1.3`).
    class CredErrorMockTransport final : public IMongoTransport
    {
    public:
        Result connect(const QString &) override { return { true, R"({"ok":1})", {}, 0, {} }; }
        Result listDatabases() override { return { true, R"([])", {}, 0, {} }; }
        Result listCollections(const QString &) override { return { true, R"({"cursor":{"firstBatch":[],"id":0},"ok":1})", {}, 0, {} }; }
        Result ping() override { return { true, R"({"ok":1})", {}, 0, {} }; }
        Result killOp(const QString &) override { return { true, R"({"ok":1})", {}, 0, {} }; }
        void disconnect() override {}
        bool isLive() const override { return false; }
        Result runCommand(const QString &, const QString &) override
        {
            return { false, {}, QStringLiteral("upstream failed for mongodb://admin:SuperSecret123@10.0.0.5:27017/admin"), 6, QStringLiteral("HostUnreachable") };
        }
    };

    QJSEngine jsNumEngine;
    ShellHost jsNumHost(&jsNumEngine);
    jsNumHost.install();
    ShellConnectOptions demoOpts;
    demoOpts.transport = &demo;
    demoOpts.transportAddress = QStringLiteral("demo");
    ASSERT_TRUE(jsNumHost.connect(demoOpts, nullptr));
    const int infCid = jsNumEngine.evaluate(
        "__host.find('catalog', 'products', {}, { limit: Infinity, skip: NaN, maxTimeMS: 1e30 })").toInt();
    ASSERT_GT(infCid, 0);
    EXPECT_EQ(jsNumHost.takeDocuments(infCid, 0).size(), 3u);
    jsNumHost.cursorClose(infCid);

    const int minCid = jsNumEngine.evaluate(
        "__host.find('catalog', 'products', {}, { limit: -9223372036854775808 })").toInt();
    ASSERT_GT(minCid, 0);
    EXPECT_EQ(jsNumHost.takeDocuments(minCid, 0).size(), 3u);
    jsNumHost.cursorClose(minCid);

    EXPECT_DOUBLE_EQ(
        jsNumEngine.evaluate("__host.countDocuments('catalog', 'products', {}, { limit: Infinity, skip: NaN })").toNumber(),
        3.0);

    CredErrorMockTransport credMock;
    ShellConnectOptions credOpts;
    credOpts.transport = &credMock;
    ASSERT_TRUE(jsNumHost.connect(credOpts, nullptr));
    const QString caughtErr = jsNumEngine.evaluate(
        "(function(){ try { __host.runCommand('admin', { hello: 1 }); return ''; } catch (e) { return e.message; } })()")
        .toString();
    EXPECT_FALSE(caughtErr.contains(QStringLiteral("SuperSecret123"))) << caughtErr.toStdString();
    EXPECT_TRUE(caughtErr.contains(QStringLiteral("mongodb://***@")));
}

TEST(ShellHostTransportGatewayTest, script_engine_executes_over_loopback_websocket_gateway)
{
    ensureApp();
    constexpr const char *kTok = "shell-gw-token-0123456789abcdef0123456789abcdef";
    QWebSocketServer server(QStringLiteral("shell-gw"), QWebSocketServer::NonSecureMode);
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));
    const QString reqHost = QStringLiteral("127.0.0.1:%1").arg(server.serverPort());
    const QStringList allowed{QString::fromUtf8(BrandInfo::productUrl),
                              QStringLiteral("http://localhost:")};
    QObject::connect(&server, &QWebSocketServer::newConnection, &server, [&] {
        auto *s = new GatewaySession(
            server.nextPendingConnection(), allowed, reqHost, QString::fromUtf8(kTok),
            [] { return std::make_unique<InMemoryDemoTransport>(); });
        QObject::connect(s, &GatewaySession::finished, &server, [s] { s->deleteLater(); });
    });

    const QString wsUrl = QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort());
    WebSocketGatewayTransport wsClient(wsUrl, QString::fromUtf8(kTok));
    ASSERT_TRUE(wsClient.connect(QStringLiteral("{}")).ok);

    ConnectionSettings conn(true);
    conn.setDefaultDatabase("catalog");
    conn.setTransportKind(wsUrl.toStdString());

    ScriptEngine shell(&conn, 10);
    shell.setTransport(&wsClient);
    shell.init("", "catalog");
    ASSERT_FALSE(shell.failedScope()) << shell.lastInitError();

    const MongoShellExecResult res = shell.exec(
        "db.products.insertOne({ _id: 55, sku: 'GW-01', name: 'Gateway Hub', price: 59.0 });"
        "db.products.find({ sku: 'GW-01' })",
        "catalog");
    ASSERT_FALSE(res.error()) << res.errorMessage();
    ASSERT_EQ(res.results().size(), 2u);
    ASSERT_EQ(res.results()[1].documents().size(), 1u);
    EXPECT_EQ(std::string(res.results()[1].documents()[0]->bsonObj()["name"].get_string().value),
              "Gateway Hub");
    wsClient.disconnect();
}

class ShellScriptReplyCatcher : public QObject
{
    Q_OBJECT

public:
    bool gotReply = false;
    bool isError = false;
    std::string errorMsg;
    std::optional<MongoShellExecResult> execResult;

public Q_SLOTS:
    void handle(ExecuteScriptResponse *resp)
    {
        isError = resp->isError();
        if (isError)
            errorMsg = resp->error().errorMessage();
        else
            execResult.emplace(resp->result);
        gotReply = true;
    }
};

TEST(ShellHostTransportGatewayTest, mongo_worker_secondary_executes_script_over_websocket_gateway)
{
    ensureApp();
    constexpr const char *kTok = "worker-gw-token-0123456789abcdef0123456789abcdef";
    QWebSocketServer server(QStringLiteral("worker-gw"), QWebSocketServer::NonSecureMode);
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));
    const QString reqHost = QStringLiteral("127.0.0.1:%1").arg(server.serverPort());
    const QStringList allowed{QString::fromUtf8(BrandInfo::productUrl),
                              QStringLiteral("http://localhost:")};
    QObject::connect(&server, &QWebSocketServer::newConnection, &server, [&] {
        auto *s = new GatewaySession(
            server.nextPendingConnection(), allowed, reqHost, QString::fromUtf8(kTok),
            [] { return std::make_unique<InMemoryDemoTransport>(); });
        QObject::connect(s, &GatewaySession::finished, &server, [s] { s->deleteLater(); });
    });

    const QString wsUrl = QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort());
    auto *conn = new ConnectionSettings(true);
    conn->setServerHost("127.0.0.1");
    conn->setServerPort(27017);
    conn->setDefaultDatabase("catalog");
    conn->setTransportKind(wsUrl.toStdString());
    conn->setTransportToken(kTok);

    // Secondary-style worker: never receives EstablishConnectionRequest, yet
    // ExecuteScriptRequest must lazily connect `_ownedTransport` on the worker
    // thread (`C1`) and tear down `_scriptEngine` before `_ownedTransport` (`C2`).
    auto *worker = new MongoWorker(conn, QString(), 50, 5.0, 10);
    bool workerDestroyed = false;
    QObject::connect(worker, &QObject::destroyed, [&workerDestroyed]() { workerDestroyed = true; });

    ShellScriptReplyCatcher catcher;
    AppRegistry::instance().bus()->send(
        worker,
        new ExecuteScriptRequest(
            &catcher,
            "db.products.insertOne({ _id: 88, sku: 'WK-88', name: 'Worker Switch', price: 79.0 });"
            "db.products.find({ _id: 88 })",
            "catalog"));

    QElapsedTimer timer;
    timer.start();
    while (!catcher.gotReply && timer.elapsed() < 8000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    worker->stopAndDelete();
    QElapsedTimer stopTimer;
    stopTimer.start();
    while (!workerDestroyed && stopTimer.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    ASSERT_TRUE(catcher.gotReply);
    ASSERT_FALSE(catcher.isError) << catcher.errorMsg;
    ASSERT_TRUE(catcher.execResult.has_value());
    ASSERT_EQ(catcher.execResult->results().size(), 2u);
    ASSERT_EQ(catcher.execResult->results()[1].documents().size(), 1u);
    EXPECT_EQ(std::string(catcher.execResult->results()[1].documents()[0]->bsonObj()["sku"].get_string().value),
              "WK-88");
}
}

#include "ShellHost_test.moc"
