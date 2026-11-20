#include "gtest/gtest.h"
#include "ragoshell/core/utils/MongoJsonUtils.h"
#include "ragoshell/core/utils/BsonUtils.h"

#include <cmath>
#include <cstdint>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/types/bson_value/view.hpp>

using namespace RagoShell;

TEST(mongo_json_utils_tests, parse_shell_literals_and_unquoted_keys)
{
    const QString shellJson = QStringLiteral(R"(
        // Sample Mongo Shell document
        {
            _id: ObjectId("507f1f77bcf86cd799439011"),
            name: 'Ada Lovelace',
            born: ISODate("1815-12-10T00:00:00Z"),
            score: NumberInt(42),
            big: NumberLong("9007199254740993"),
            price: NumberDecimal("19.95"),
            ts: Timestamp(1700000000, 3),
            active: true,
            tags: ['math', 'computing',],
        }
    )");

    bsoncxx::document::value doc = bsoncxx::from_json("{}");
    QString err;
    ASSERT_TRUE(MongoJsonUtils::parseSingleDocument(shellJson, doc, err))
        << err.toStdString();

    const auto v = doc.view();
    ASSERT_TRUE(v["_id"]);
    EXPECT_EQ(v["_id"].type(), bsoncxx::type::k_oid);
    EXPECT_EQ(v["_id"].get_oid().value.to_string(), "507f1f77bcf86cd799439011");

    ASSERT_TRUE(v["name"]);
    EXPECT_EQ(std::string(v["name"].get_string().value), "Ada Lovelace");

    ASSERT_TRUE(v["born"]);
    EXPECT_EQ(v["born"].type(), bsoncxx::type::k_date);

    ASSERT_TRUE(v["score"]);
    EXPECT_EQ(v["score"].type(), bsoncxx::type::k_int32);
    EXPECT_EQ(v["score"].get_int32().value, 42);

    ASSERT_TRUE(v["big"]);
    EXPECT_EQ(v["big"].type(), bsoncxx::type::k_int64);
    EXPECT_EQ(v["big"].get_int64().value, 9007199254740993LL);

    ASSERT_TRUE(v["price"]);
    EXPECT_EQ(v["price"].type(), bsoncxx::type::k_decimal128);
    EXPECT_EQ(v["price"].get_decimal128().value.to_string(), "19.95");

    ASSERT_TRUE(v["ts"]);
    EXPECT_EQ(v["ts"].type(), bsoncxx::type::k_timestamp);
}

TEST(mongo_json_utils_tests, parse_multiple_documents_array_and_concatenated)
{
    const QString concatShell = QStringLiteral(R"(
        /* 1 */
        { name: "first", count: NumberInt(1) }

        /* 2 */
        { name: "second", count: NumberInt(2) }
    )");

    std::vector<bsoncxx::document::value> docs;
    QString err;
    ASSERT_TRUE(MongoJsonUtils::parseDocuments(concatShell, docs, &err))
        << err.toStdString();
    ASSERT_EQ(docs.size(), 2u);
    EXPECT_EQ(std::string(docs[0].view()["name"].get_string().value), "first");
    EXPECT_EQ(std::string(docs[1].view()["name"].get_string().value), "second");

    // Single-document parser must reject multi-doc input
    bsoncxx::document::value single = bsoncxx::from_json("{}");
    EXPECT_FALSE(MongoJsonUtils::parseSingleDocument(concatShell, single, &err));
}

TEST(mongo_json_utils_tests, update_scalar_field_at_nested_and_array_paths)
{
    bsoncxx::document::value orig = bsoncxx::from_json(
        R"({"_id":{"$oid":"507f1f77bcf86cd799439011"},"status":"draft","profile":{"age":29},"scores":[10,20,30]})");

    // 1. Update nested Int32 field
    bsoncxx::document::value updated1 = bsoncxx::from_json("{}");
    QString err;
    ASSERT_TRUE(MongoJsonUtils::updateScalarFieldAtPath(
        orig.view(), QStringLiteral("profile.age"), QStringLiteral("30"),
        QStringLiteral("Int32"), updated1, err)) << err.toStdString();

    EXPECT_EQ(updated1.view()["_id"].get_oid().value.to_string(), "507f1f77bcf86cd799439011");
    EXPECT_EQ(updated1.view()["profile"].get_document().view()["age"].get_int32().value, 30);

    // 2. Update array element at scores.1
    bsoncxx::document::value updated2 = bsoncxx::from_json("{}");
    ASSERT_TRUE(MongoJsonUtils::updateScalarFieldAtPath(
        updated1.view(), QStringLiteral("scores.1"), QStringLiteral("99"),
        QStringLiteral("Int32"), updated2, err)) << err.toStdString();
    auto arr = updated2.view()["scores"].get_array().value;
    auto it = arr.begin();
    ++it;
    EXPECT_EQ(it->get_int32().value, 99);

    // 3. Reject direct _id mutation
    bsoncxx::document::value rejected = bsoncxx::from_json("{}");
    EXPECT_FALSE(MongoJsonUtils::updateScalarFieldAtPath(
        orig.view(), QStringLiteral("_id"), QStringLiteral("507f1f77bcf86cd799439099"),
        QStringLiteral("ObjectId"), rejected, err));
}

TEST(mongo_json_utils_tests, extract_iso_date_from_object_id)
{
    const QString iso = MongoJsonUtils::extractIsoDateFromObjectId(
        QStringLiteral("ObjectId(\"507f1f77bcf86cd799439011\")"));
    EXPECT_EQ(iso, QStringLiteral("ISODate(\"2012-10-17T21:13:27Z\")"));
    EXPECT_TRUE(MongoJsonUtils::extractIsoDateFromObjectId(QStringLiteral("not-an-oid")).isEmpty());
}

TEST(mongo_json_utils_tests, format_json_documents_preserves_all_distinct_fields_and_array_items)
{
    const QString input = QStringLiteral(R"({
        _id: ObjectId("63b290af205c547df0e08646"),
        title: "Agenda Item 1",
        status: "published",
        views: NumberInt(125),
        meta: { author: "editor", priority: NumberInt(2) },
        tags: ["news", "economy", "tech"]
    })");

    QString formatted;
    QString err;
    ASSERT_TRUE(MongoJsonUtils::formatJsonDocuments(input, formatted, err))
        << err.toStdString();

    // Ensure every field and array element is serialized distinctly (no repeated first-element bug)
    EXPECT_EQ(formatted.count(QStringLiteral("\"_id\"")), 1);
    EXPECT_TRUE(formatted.contains(QStringLiteral("\"title\" : \"Agenda Item 1\"")));
    EXPECT_TRUE(formatted.contains(QStringLiteral("\"status\" : \"published\"")));
    EXPECT_TRUE(formatted.contains(QStringLiteral("\"views\" : 125")));
    EXPECT_TRUE(formatted.contains(QStringLiteral("\"author\" : \"editor\"")));
    EXPECT_TRUE(formatted.contains(QStringLiteral("\"priority\" : 2")));
    EXPECT_TRUE(formatted.contains(QStringLiteral("\"news\"")));
    EXPECT_TRUE(formatted.contains(QStringLiteral("\"economy\"")));
    EXPECT_TRUE(formatted.contains(QStringLiteral("\"tech\"")));

    // Round-trip formatted output back to BSON
    bsoncxx::document::value reparsed = bsoncxx::from_json("{}");
    ASSERT_TRUE(MongoJsonUtils::parseSingleDocument(formatted, reparsed, err))
        << err.toStdString();
    EXPECT_EQ(reparsed.view()["_id"].get_oid().value.to_string(), "63b290af205c547df0e08646");
    EXPECT_EQ(std::string(reparsed.view()["title"].get_string().value), "Agenda Item 1");
    EXPECT_EQ(std::string(reparsed.view()["status"].get_string().value), "published");
    EXPECT_EQ(reparsed.view()["views"].get_int32().value, 125);
}

TEST(mongo_json_utils_tests, parse_and_build_find_query_chain_two_way_sync)
{
    // 1. Standard getCollection(...).find(...) chain with sort, skip, limit, and explain
    const QString script1 = QStringLiteral(
        "// Query bar sync test\n"
        "db.getCollection('agendaitems').find({\"agendaId\": 4}, {title: 1}).sort({_id: -1}).skip(10).limit(25).explain('executionStats');");
    const auto q1 = MongoJsonUtils::parseFindQueryChain(script1);
    ASSERT_TRUE(q1.valid);
    EXPECT_EQ(q1.collection, QStringLiteral("agendaitems"));
    EXPECT_TRUE(q1.useGetCollection);
    EXPECT_EQ(q1.filter, QStringLiteral("{\"agendaId\": 4}"));
    EXPECT_EQ(q1.projection, QStringLiteral("{title: 1}"));
    EXPECT_EQ(q1.sort, QStringLiteral("{_id: -1}"));
    EXPECT_EQ(q1.skip, 10);
    EXPECT_EQ(q1.limit, 25);
    EXPECT_TRUE(q1.explain);
    EXPECT_EQ(q1.explainArg, QStringLiteral("'executionStats'"));

    const QString rebuilt1 = MongoJsonUtils::buildFindQueryChain(q1);
    EXPECT_EQ(rebuilt1, QStringLiteral(
        "db.getCollection('agendaitems').find({\"agendaId\": 4}, {title: 1}).sort({_id: -1}).skip(10).limit(25).explain('executionStats')"));

    // 2. Direct db.users.find(...) style preservation + strings containing method-like tokens
    const QString script2 = QStringLiteral(
        "db.users.find({ note: \"a).limit(999)\" }).limit(15)");
    auto q2 = MongoJsonUtils::parseFindQueryChain(script2);
    ASSERT_TRUE(q2.valid);
    EXPECT_EQ(q2.collection, QStringLiteral("users"));
    EXPECT_FALSE(q2.useGetCollection);
    EXPECT_EQ(q2.limit, 15);

    q2.sort = QStringLiteral("{ createdAt: -1 }");
    const QString rebuilt2 = MongoJsonUtils::buildFindQueryChain(q2);
    EXPECT_EQ(rebuilt2, QStringLiteral(
        "db.users.find({ note: \"a).limit(999)\" }).sort({ createdAt: -1 }).limit(15)"));

    // 3. Non-find commands and multi-statement scripts must return valid == false
    EXPECT_FALSE(MongoJsonUtils::parseFindQueryChain(
        QStringLiteral("db.getCollection('agendaitems').stats()")).valid);
    EXPECT_FALSE(MongoJsonUtils::parseFindQueryChain(
        QStringLiteral("db.getCollection('agendaitems').getIndexes()")).valid);
    EXPECT_FALSE(MongoJsonUtils::parseFindQueryChain(
        QStringLiteral("db.agendaitems.aggregate([{ $match: {} }])")).valid);
    EXPECT_FALSE(MongoJsonUtils::parseFindQueryChain(
        QStringLiteral("db.a.find({}); db.b.find({})")).valid);
}

TEST(mongo_json_utils_tests, detect_destructive_script_operations)
{
    // Safe read/query commands should return empty list
    EXPECT_TRUE(MongoJsonUtils::detectDestructiveScriptOperations(
        QStringLiteral("db.getCollection('orders').find({ status: 'paid' }).limit(50)")).isEmpty());

    // Strings and comments containing destructive method names must NOT false-positive
    // Comments and string literals must NOT trigger false positives
    EXPECT_TRUE(MongoJsonUtils::detectDestructiveScriptOperations(
        QStringLiteral("// db.dropDatabase()\n"
                       "/* db.orders.deleteMany({}); db.orders.updateMany({}, {$set:{a:1}}); db.orders.insertOne({a:1}); */\n"
                       "db.logs.find({ message: \"User ran db.users.drop(), insertOne({}) and updateOne({})\" })")).isEmpty());

    // Read-only aggregation (without $out / $merge) must NOT trigger warning
    EXPECT_TRUE(MongoJsonUtils::detectDestructiveScriptOperations(
        QStringLiteral("db.orders.aggregate([{ $match: { status: 'paid' } }, { $group: { _id: '$region', n: { $sum: 1 } } }])")).isEmpty());

    // Explain wrappers over remove() and update() are read-only and must NOT trigger warning
    EXPECT_TRUE(MongoJsonUtils::detectDestructiveScriptOperations(
        QStringLiteral("db.orders.explain('executionStats').remove({ archived: true })")).isEmpty());
    EXPECT_TRUE(MongoJsonUtils::detectDestructiveScriptOperations(
        QStringLiteral("db.orders.explain('executionStats').update({ active: false }, { $set: { flag: 1 } })")).isEmpty());

    // Delete / Drop shell calls must be detected
    const QStringList ops1 = MongoJsonUtils::detectDestructiveScriptOperations(
        QStringLiteral("db.getCollection('orders').deleteMany({ expired: true });"));
    ASSERT_EQ(ops1.size(), 1);
    EXPECT_EQ(ops1.first(), QStringLiteral("deleteMany()"));

    const QStringList ops2 = MongoJsonUtils::detectDestructiveScriptOperations(
        QStringLiteral("db.temp.drop();\ndb.dropDatabase();"));
    EXPECT_TRUE(ops2.contains(QStringLiteral("drop()")));
    EXPECT_TRUE(ops2.contains(QStringLiteral("dropDatabase()")));

    // Update / Modify shell calls (including $merge / $out aggregation stages) must be detected
    const QStringList opsUpdate = MongoJsonUtils::detectDestructiveScriptOperations(
        QStringLiteral("db.users.updateOne({ _id: 1 }, { $set: { role: 'admin' } });\n"
                       "db.users.replaceOne({ _id: 2 }, { name: 'Ada' });\n"
                       "db.sales.aggregate([{ $match: {} }, { $merge: 'monthly_totals' }]);"));
    EXPECT_TRUE(opsUpdate.contains(QStringLiteral("updateOne()")));
    EXPECT_TRUE(opsUpdate.contains(QStringLiteral("replaceOne()")));
    EXPECT_TRUE(opsUpdate.contains(QStringLiteral("aggregate($merge)")));

    // Insert / Create shell calls must be detected
    const QStringList opsInsert = MongoJsonUtils::detectDestructiveScriptOperations(
        QStringLiteral("db.items.insertOne({ sku: 'A-1' });\n"
                       "db.items.insertMany([{ sku: 'A-2' }]);"));
    EXPECT_TRUE(opsInsert.contains(QStringLiteral("insertOne()")));
    EXPECT_TRUE(opsInsert.contains(QStringLiteral("insertMany()")));

    // Raw runCommand({ dropDatabase / update / insert }) must also be caught
    const QStringList ops3 = MongoJsonUtils::detectDestructiveScriptOperations(
        QStringLiteral("db.runCommand({ \"dropDatabase\": 1 }); db.runCommand({ insert: 'logs', documents: [{}] });"));
    EXPECT_TRUE(ops3.contains(QStringLiteral("runCommand(dropDatabase)")));
    EXPECT_TRUE(ops3.contains(QStringLiteral("runCommand(insert)")));
}

TEST(mongo_json_utils_tests, escapes_c1_control_codes_without_corrupting_turkish_utf8_on_round_trip)
{
    // Exact document reported by user: "filename" contains U+00C3 ('Ã') + U+009C (C1 ST)
    // alongside valid Turkish UTF-8 strings.
    const std::string properTrUtf8 =
        "\xC3\x9CLKE TV & AK\xC4\xB0T TV \xC5\x9E\xC4\x9E\xC3\x87\xC3\x96\xC4\xB1";
    const QString input = QString::fromUtf8(
        ("{\n"
         "    \"_id\" : \"a24c12d1-359c-4469-a543-1d66d487684e\",\n"
         "    \"docId\" : 1997031673,\n"
         "    \"viewDate\" : ISODate(\"2024-12-15T00:00:00.000Z\"),\n"
         "    \"filename\" : \"\xC3\x83\xC2\x9CLKE TV_20241215_0.jpeg\",\n"
         "    \"properTurkish\" : \"" + properTrUtf8 + "\"\n"
         "}").c_str());

    QString formatted;
    QString err;
    ASSERT_TRUE(MongoJsonUtils::formatJsonDocuments(input, formatted, err))
        << err.toStdString();

    // U+009C (ST) must be escaped as \u009c so Scintilla never renders a raw [ST] badge
    EXPECT_FALSE(formatted.contains(QChar(0x009C)));
    const QString expectedEscaped =
        QStringLiteral("\"") + QChar(0x00C3) + QStringLiteral("\\u009cLKE TV_20241215_0.jpeg\"");
    EXPECT_TRUE(formatted.contains(expectedEscaped));
    // Valid Turkish UTF-8 ('Ü' = 0xC3 0x9C, 'İ' = 0xC4 0xB0, etc.) must NOT be escaped
    EXPECT_TRUE(formatted.contains(QString::fromUtf8(("\"" + properTrUtf8 + "\"").c_str())));

    // Round-trip back through parseSingleDocument must preserve exact original bytes
    bsoncxx::document::value reparsed = bsoncxx::from_json("{}");
    ASSERT_TRUE(MongoJsonUtils::parseSingleDocument(formatted, reparsed, err))
        << err.toStdString();
    const auto fnView = reparsed.view()["filename"].get_string().value;
    EXPECT_EQ(std::string(fnView.data(), fnView.size()),
              std::string("\xC3\x83\xC2\x9CLKE TV_20241215_0.jpeg"));
    const auto trView = reparsed.view()["properTurkish"].get_string().value;
    EXPECT_EQ(std::string(trView.data(), trView.size()), properTrUtf8);
}

TEST(mongo_json_utils_tests, duplicate_template_for_object_id_vs_non_object_id_types)
{
    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;

    // 1. ObjectId _id -> _id is stripped so MongoDB assigns a fresh ObjectId
    const auto oidDoc = make_document(
        kvp("_id", bsoncxx::oid{"507f1f77bcf86cd799439011"}),
        kvp("name", "İstanbul"));
    const auto oidTpl = MongoJsonUtils::buildDuplicateDocumentTemplate(oidDoc.view());
    EXPECT_FALSE(oidTpl.json.contains(QStringLiteral("\"_id\"")));
    EXPECT_TRUE(oidTpl.json.contains(QStringLiteral("\"name\" : \"İstanbul\"")));
    EXPECT_EQ(oidTpl.caretUtf16Offset, -1);

    // 2. Document containing ONLY ObjectId _id -> indented empty block with caret inside
    const auto oidOnlyDoc = make_document(kvp("_id", bsoncxx::oid{"507f1f77bcf86cd799439011"}));
    const auto oidOnlyTpl = MongoJsonUtils::buildDuplicateDocumentTemplate(oidOnlyDoc.view());
    EXPECT_EQ(oidOnlyTpl.json, QStringLiteral("{\n    \n}"));
    EXPECT_EQ(oidOnlyTpl.caretUtf16Offset, 6);

    // 3. String _id -> "_id" : "" generated and caret placed strictly between ""
    const auto strIdDoc = make_document(
        kvp("_id", "a24c12d1-359c-4469-a543-1d66d487684e"),
        kvp("city", "Çanakkale"),
        kvp("count", 7));
    const auto strTpl = MongoJsonUtils::buildDuplicateDocumentTemplate(strIdDoc.view());
    ASSERT_GT(strTpl.caretUtf16Offset, 0);
    ASSERT_LT(strTpl.caretUtf16Offset, strTpl.json.size());
    EXPECT_EQ(strTpl.json[strTpl.caretUtf16Offset - 1], QLatin1Char('"'));
    EXPECT_EQ(strTpl.json[strTpl.caretUtf16Offset], QLatin1Char('"'));

    // Typing a new _id at caretUtf16Offset must yield a valid BSON document with that _id
    QString edited = strTpl.json;
    edited.insert(strTpl.caretUtf16Offset, QStringLiteral("custom-tr-17"));
    bsoncxx::document::value parsed = bsoncxx::from_json("{}");
    QString err;
    ASSERT_TRUE(MongoJsonUtils::parseSingleDocument(edited, parsed, err)) << err.toStdString();
    EXPECT_EQ(std::string(parsed.view()["_id"].get_string().value), "custom-tr-17");
    EXPECT_EQ(std::string(parsed.view()["city"].get_string().value), "Çanakkale");
    EXPECT_EQ(parsed.view()["count"].get_int32().value, 7);

    // 4. Int32 & Int64 _id -> also generate "_id" : "" with caret between ""
    const auto intIdDoc = make_document(kvp("_id", int32_t{42}), kvp("code", "X"));
    const auto intTpl = MongoJsonUtils::buildDuplicateDocumentTemplate(intIdDoc.view());
    ASSERT_GT(intTpl.caretUtf16Offset, 0);
    EXPECT_EQ(intTpl.json[intTpl.caretUtf16Offset - 1], QLatin1Char('"'));
    EXPECT_EQ(intTpl.json[intTpl.caretUtf16Offset], QLatin1Char('"'));
}

TEST(mongo_json_utils_tests, string_literals_with_shell_constructor_syntax_are_never_mutated)
{
    const QString docText = QString::fromUtf8(R"json({
        _id: "doc-1",
        noteOid: "Use ObjectId(\"507f1f77bcf86cd799439011\") or ObjectId()",
        noteDate: "Call Date() or ISODate(\"2026-01-01T00:00:00Z\")",
        noteNums: "NumberInt(7), NumberLong(\"99\"), NumberDecimal(\"1.5\")",
        noteKeys: "MinKey() .. MaxKey() .. UUID(\"00112233-4455-6677-8899-aabbccddeeff\")",
        noteSpecial: "NaN Infinity -Infinity undefined"
    })json");

    bsoncxx::document::value doc = bsoncxx::from_json("{}");
    QString err;
    ASSERT_TRUE(MongoJsonUtils::parseSingleDocument(docText, doc, err)) << err.toStdString();
    const auto v = doc.view();
    EXPECT_EQ(std::string(v["noteOid"].get_string().value),
              "Use ObjectId(\"507f1f77bcf86cd799439011\") or ObjectId()");
    EXPECT_EQ(std::string(v["noteDate"].get_string().value),
              "Call Date() or ISODate(\"2026-01-01T00:00:00Z\")");
    EXPECT_EQ(std::string(v["noteNums"].get_string().value),
              "NumberInt(7), NumberLong(\"99\"), NumberDecimal(\"1.5\")");
    EXPECT_EQ(std::string(v["noteKeys"].get_string().value),
              "MinKey() .. MaxKey() .. UUID(\"00112233-4455-6677-8899-aabbccddeeff\")");
    EXPECT_EQ(std::string(v["noteSpecial"].get_string().value),
              "NaN Infinity -Infinity undefined");
}

TEST(mongo_json_utils_tests, legacy_and_standard_uuid_round_trip_preserves_subtype_and_bytes)
{
    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;

    static const uint8_t rawBytes[16] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe
    };

    // Standard UUID (subtype 04)
    {
        bsoncxx::types::b_binary bin4{bsoncxx::binary_sub_type::k_uuid, 16, rawBytes};
        const auto orig = make_document(kvp("_id", bin4), kvp("v", 1));
        const QString json = QString::fromStdString(
            BsonUtils::jsonString(orig.view(), TenGen, 1, DefaultEncoding, Utc));
        bsoncxx::document::value reparsed = bsoncxx::from_json("{}");
        QString err;
        ASSERT_TRUE(MongoJsonUtils::parseSingleDocument(json, reparsed, err)) << err.toStdString();
        EXPECT_EQ(orig.view()["_id"].get_value(), reparsed.view()["_id"].get_value());
    }

    // Legacy UUID (subtype 03) across all 4 legacy encodings (LUUID, JUUID, NUUID, PYUUID)
    const UUIDEncoding encodings[] = {DefaultEncoding, JavaLegacy, CSharpLegacy, PythonLegacy};
    for (UUIDEncoding enc : encodings) {
        bsoncxx::types::b_binary bin3{bsoncxx::binary_sub_type::k_uuid_deprecated, 16, rawBytes};
        const auto orig = make_document(kvp("_id", bin3), kvp("v", 2));
        const QString json = QString::fromStdString(
            BsonUtils::jsonString(orig.view(), TenGen, 1, enc, Utc));
        bsoncxx::document::value reparsed = bsoncxx::from_json("{}");
        QString err;
        ASSERT_TRUE(MongoJsonUtils::parseSingleDocument(json, reparsed, err))
            << "encoding=" << static_cast<int>(enc) << " json=" << json.toStdString()
            << " err=" << err.toStdString();
        EXPECT_EQ(orig.view()["_id"].get_value(), reparsed.view()["_id"].get_value());
    }
}

TEST(mongo_json_utils_tests, extended_shell_literals_epoch_date_hexdata_dbref_nan_infinity_undefined)
{
    const QString input = QString::fromUtf8(R"json({
        _id: 1,
        preEpoch: Date(-86400000),
        hexBin: HexData(0, "616263"),
        ptr: DBRef("users", "507f1f77bcf86cd799439011"),
        nanVal: NaN,
        posInf: Infinity,
        negInf: -Infinity,
        undefVal: undefined
    })json");

    bsoncxx::document::value doc = bsoncxx::from_json("{}");
    QString err;
    ASSERT_TRUE(MongoJsonUtils::parseSingleDocument(input, doc, err)) << err.toStdString();
    const auto v = doc.view();
    EXPECT_EQ(v["preEpoch"].get_date().value.count(), -86400000LL);
    ASSERT_EQ(v["hexBin"].type(), bsoncxx::type::k_binary);
    EXPECT_EQ(v["hexBin"].get_binary().size, 3u);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(v["hexBin"].get_binary().bytes), 3), "abc");
    EXPECT_EQ(v["ptr"].type(), bsoncxx::type::k_dbpointer);
    EXPECT_TRUE(std::isnan(v["nanVal"].get_double().value));
    EXPECT_TRUE(std::isinf(v["posInf"].get_double().value) && v["posInf"].get_double().value > 0);
    EXPECT_TRUE(std::isinf(v["negInf"].get_double().value) && v["negInf"].get_double().value < 0);
    EXPECT_EQ(v["undefVal"].type(), bsoncxx::type::k_undefined);
}

TEST(mongo_json_utils_tests, inline_scalar_string_preserves_boolean_null_words_quotes_and_backslashes)
{
    const bsoncxx::document::value orig = bsoncxx::from_json(R"({"_id": 1, "label": "initial"})");
    const QString cases[] = {
        QStringLiteral("true"),
        QStringLiteral("false"),
        QStringLiteral("null"),
        QStringLiteral("\"quoted\""),
        QStringLiteral("'single'"),
        QStringLiteral("C:\\work\\nraw")
    };
    for (const QString &val : cases) {
        bsoncxx::document::value updated = bsoncxx::from_json("{}");
        QString err;
        ASSERT_TRUE(MongoJsonUtils::updateScalarFieldAtPath(
            orig.view(), QStringLiteral("label"), val, QStringLiteral("String"), updated, err))
            << err.toStdString();
        ASSERT_EQ(updated.view()["label"].type(), bsoncxx::type::k_string);
        EXPECT_EQ(QString::fromStdString(std::string(updated.view()["label"].get_string().value)), val);
    }
}
