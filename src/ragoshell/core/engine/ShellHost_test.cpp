// docs/Shell.md: the BSON <-> JavaScript converter (no server needed) and the
// host object that gives JavaScript access to MongoDB (live tests: dev/mongo-test on localhost:27017
// with the "samples" database; they are skipped when the server is unreachable).

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

#include <QCoreApplication>
#include <QJSEngine>

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

#include "ragoshell/core/domain/MongoDocument.h"
#include "ragoshell/core/engine/BsonJsConverter.h"
#include "ragoshell/core/engine/ShellHost.h"

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
}
