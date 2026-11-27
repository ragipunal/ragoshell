#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/decimal128.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>

#include "ragoshell/core/transport/InMemoryDemoTransport.h"
#include "ragoshell/core/utils/ExplainPlan.h"

using namespace RagoShell;

namespace
{
    IMongoTransport::Result cmd(InMemoryDemoTransport &t, const QString &db,
                                const QString &ejson)
    {
        return t.runCommand(db, ejson);
    }

    QJsonObject toRelaxedObj(const IMongoTransport::Result &r)
    {
        const auto doc = bsoncxx::from_json(r.ejson.toStdString());
        const std::string relaxed = bsoncxx::to_json(
            doc.view(), bsoncxx::ExtendedJsonMode::k_relaxed);
        return QJsonDocument::fromJson(QByteArray::fromStdString(relaxed)).object();
    }

    QJsonArray firstBatch(const IMongoTransport::Result &r)
    {
        return toRelaxedObj(r).value("cursor").toObject().value("firstBatch").toArray();
    }
}

TEST(InMemoryDemoTransportTest, connect_and_ping)
{
    InMemoryDemoTransport t;
    EXPECT_FALSE(t.isLive());
    auto r = t.connect(QStringLiteral("{}"));
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(t.ping().ok);
}

TEST(InMemoryDemoTransportTest, list_catalog)
{
    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);
    auto dbs = t.listDatabases();
    ASSERT_TRUE(dbs.ok);
    EXPECT_TRUE(dbs.ejson.contains(QStringLiteral("catalog")));
    auto colls = t.listCollections(QStringLiteral("catalog"));
    ASSERT_TRUE(colls.ok);
    const QJsonArray c = firstBatch(colls);
    QStringList names;
    for (const QJsonValue &v : c)
        names << v.toObject().value("name").toString();
    EXPECT_TRUE(names.contains("products") && names.contains("orders")
                && names.contains("customers"));
}

TEST(InMemoryDemoTransportTest, find_eq)
{
    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);
    auto r = cmd(t, "catalog",
                 R"({"find":"products","filter":{"sku":"MS-07"}})");
    ASSERT_TRUE(r.ok);
    const QJsonArray docs = firstBatch(r);
    ASSERT_EQ(docs.size(), 1);
    EXPECT_EQ(docs[0].toObject().value("name").toString(),
              QStringLiteral("Wireless Mouse"));
}

TEST(InMemoryDemoTransportTest, find_gt)
{
    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);
    auto r2 = cmd(t, "catalog",
                  R"({"find":"products","filter":{"price":{"$gt":80}}})");
    ASSERT_TRUE(r2.ok);
    EXPECT_EQ(firstBatch(r2).size(), 2);
}

TEST(InMemoryDemoTransportTest, find_limit)
{
    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);
    auto r3 = cmd(t, "catalog",
                  R"({"find":"products","filter":{},"limit":1})");
    ASSERT_TRUE(r3.ok);
    EXPECT_EQ(firstBatch(r3).size(), 1);
}

TEST(InMemoryDemoTransportTest, aggregate_match_and_count)
{
    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);
    auto r = cmd(t, "catalog",
                 R"({"aggregate":"orders","pipeline":[{"$match":{"customerId":"c1"}}]})");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(firstBatch(r).size(), 2);

    auto r2 = cmd(t, "catalog",
                  R"({"aggregate":"orders","pipeline":[{"$count":"n"}]})");
    ASSERT_TRUE(r2.ok);
    const QJsonArray docs = firstBatch(r2);
    ASSERT_EQ(docs.size(), 1);
    EXPECT_EQ(docs[0].toObject().value("n").toInteger(), 3);
}

TEST(InMemoryDemoTransportTest, errors_are_ejson_shaped)
{
    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);

    // Nonexistent collection on find returns empty cursor (matches mongod),
    // while drop/listIndexes return NamespaceNotFound (code 26).
    auto emptyFind = cmd(t, "catalog", R"({"find":"nonexistent"})");
    ASSERT_TRUE(emptyFind.ok);
    EXPECT_EQ(firstBatch(emptyFind).size(), 0);

    auto dropMissing = cmd(t, "catalog", R"({"drop":"nonexistent"})");
    EXPECT_FALSE(dropMissing.ok);
    EXPECT_EQ(dropMissing.code, 26);

    auto unknown = cmd(t, "catalog", R"({"unknownCmd":1})");
    EXPECT_FALSE(unknown.ok);
    EXPECT_EQ(unknown.code, 59);

    auto badJson = cmd(t, "catalog", R"({broken)");
    EXPECT_FALSE(badJson.ok);
    EXPECT_EQ(badJson.code, 9);

    InMemoryDemoTransport t2;
    auto disconnected = t2.runCommand(QStringLiteral("catalog"), QStringLiteral(R"({"ping":1})"));
    EXPECT_FALSE(disconnected.ok);
}

TEST(InMemoryDemoTransportTest, crud_insert_update_delete_and_find_and_modify)
{
    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);

    // Insert with auto-generated ObjectId _id and explicit _id.
    auto ins = cmd(t, "catalog",
                   R"({"insert":"products","documents":[{"sku":"HD-99","name":"USB-C Dock","category":"peripherals","price":129.0,"stock":15,"tags":["usb","dock"]}]})");
    ASSERT_TRUE(ins.ok) << ins.error.toStdString();
    EXPECT_EQ(toRelaxedObj(ins).value("n").toInt(), 1);

    // Update with $inc, $set, $addToSet, $pull.
    auto upd = cmd(t, "catalog",
                   R"({"update":"products","updates":[{"q":{"sku":"HD-99"},"u":{"$inc":{"stock":5},"$set":{"featured":true},"$addToSet":{"tags":"pd"}},"multi":false}]})");
    ASSERT_TRUE(upd.ok) << upd.error.toStdString();
    EXPECT_EQ(toRelaxedObj(upd).value("nModified").toInt(), 1);

    auto check = firstBatch(cmd(t, "catalog", R"({"find":"products","filter":{"sku":"HD-99"}})"));
    ASSERT_EQ(check.size(), 1);
    EXPECT_EQ(check[0].toObject().value("stock").toInt(), 20);
    EXPECT_TRUE(check[0].toObject().value("featured").toBool());
    EXPECT_EQ(check[0].toObject().value("tags").toArray().size(), 3);

    // Modifying immutable _id reports code 66 in writeErrors.
    auto immut = cmd(t, "catalog",
                     R"({"update":"products","updates":[{"q":{"_id":1},"u":{"$set":{"_id":999}}}]})");
    ASSERT_TRUE(immut.ok);
    const QJsonArray immutErrs = toRelaxedObj(immut).value("writeErrors").toArray();
    ASSERT_EQ(immutErrs.size(), 1);
    EXPECT_EQ(immutErrs[0].toObject().value("code").toInt(), 66);

    // findAndModify with new:true and upsert.
    auto fam = cmd(t, "catalog",
                   R"({"findAndModify":"customers","query":{"_id":"c4"},"update":{"$set":{"name":"Barbara Liskov","tier":"platinum"}},"new":true,"upsert":true})");
    ASSERT_TRUE(fam.ok) << fam.error.toStdString();
    EXPECT_EQ(toRelaxedObj(fam).value("value").toObject().value("name").toString(),
              QStringLiteral("Barbara Liskov"));

    // Delete single document.
    auto del = cmd(t, "catalog",
                   R"({"delete":"products","deletes":[{"q":{"sku":"HD-99"},"limit":1}]})");
    ASSERT_TRUE(del.ok);
    EXPECT_EQ(toRelaxedObj(del).value("n").toInt(), 1);
    EXPECT_EQ(firstBatch(cmd(t, "catalog", R"({"find":"products"})")).size(), 3);
}

TEST(InMemoryDemoTransportTest, duplicate_key_enforced_on_id_and_unique_secondary_index)
{
    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);

    // Duplicate _id.
    auto dupId = cmd(t, "catalog",
                     R"({"insert":"products","documents":[{"_id":1,"sku":"NEW-1"}]})");
    ASSERT_TRUE(dupId.ok);
    const QJsonArray idErrs = toRelaxedObj(dupId).value("writeErrors").toArray();
    ASSERT_EQ(idErrs.size(), 1);
    EXPECT_EQ(idErrs[0].toObject().value("code").toInt(), 11000);

    // Duplicate sku on seeded unique `sku_1` index.
    auto dupSku = cmd(t, "catalog",
                      R"({"insert":"products","documents":[{"_id":99,"sku":"KB-01"}]})");
    ASSERT_TRUE(dupSku.ok);
    const QJsonArray skuErrs = toRelaxedObj(dupSku).value("writeErrors").toArray();
    ASSERT_EQ(skuErrs.size(), 1);
    EXPECT_EQ(skuErrs[0].toObject().value("code").toInt(), 11000);

    // Unordered batch inserts valid entries around a duplicate.
    auto unordered = cmd(t, "catalog",
                         R"({"insert":"customers","ordered":false,"documents":[{"_id":"c1","name":"Dup"},{"_id":"c9","name":"Alan Turing"}]})");
    ASSERT_TRUE(unordered.ok);
    EXPECT_EQ(toRelaxedObj(unordered).value("n").toInt(), 1);
    EXPECT_EQ(toRelaxedObj(unordered).value("writeErrors").toArray().size(), 1);
    EXPECT_EQ(firstBatch(cmd(t, "catalog", R"({"find":"customers","filter":{"_id":"c9"}})")).size(), 1);
}

TEST(InMemoryDemoTransportTest, rich_filters_sort_projection_and_distinct)
{
    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);

    // Array element match + regex + sort + exclusion/inclusion projection.
    auto res = cmd(t, "catalog",
                   R"({"find":"products","filter":{"tags":"input","name":{"$regex":"mouse|keyboard","$options":"i"}},"sort":{"price":1},"projection":{"_id":0,"sku":1,"price":1}})");
    ASSERT_TRUE(res.ok) << res.error.toStdString();
    const QJsonArray docs = firstBatch(res);
    ASSERT_EQ(docs.size(), 2);
    EXPECT_EQ(docs[0].toObject().value("sku").toString(), QStringLiteral("MS-07"));
    EXPECT_EQ(docs[1].toObject().value("sku").toString(), QStringLiteral("KB-01"));
    EXPECT_FALSE(docs[0].toObject().contains("_id"));

    // distinct flattens array elements and sorts deduplicated values.
    auto dist = cmd(t, "catalog", R"({"distinct":"products","key":"tags"})");
    ASSERT_TRUE(dist.ok);
    const QJsonArray tags = toRelaxedObj(dist).value("values").toArray();
    EXPECT_EQ(tags.size(), 4); // display, input, usb, wireless
}

TEST(InMemoryDemoTransportTest, aggregation_group_project_unwind_and_sample)
{
    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);

    auto grp = cmd(t, "catalog", R"({
        "aggregate": "orders",
        "pipeline": [
            {"$group": {"_id": "$customerId", "revenue": {"$sum": "$total"}, "orders": {"$sum": 1}}},
            {"$sort": {"revenue": -1}}
        ]
    })");
    ASSERT_TRUE(grp.ok) << grp.error.toStdString();
    const QJsonArray groups = firstBatch(grp);
    ASSERT_EQ(groups.size(), 2);
    EXPECT_EQ(groups[0].toObject().value("_id").toString(), QStringLiteral("c1"));
    EXPECT_NEAR(groups[0].toObject().value("revenue").toDouble(), 338.90, 0.001);
    EXPECT_EQ(groups[0].toObject().value("orders").toInt(), 2);

    auto unw = cmd(t, "catalog", R"({
        "aggregate": "products",
        "pipeline": [
            {"$unwind": "$tags"},
            {"$sortByCount": "$tags"}
        ]
    })");
    ASSERT_TRUE(unw.ok) << unw.error.toStdString();
    const QJsonArray tagCounts = firstBatch(unw);
    ASSERT_EQ(tagCounts.size(), 4);
    EXPECT_EQ(tagCounts[0].toObject().value("_id").toString(), QStringLiteral("input"));
    EXPECT_EQ(tagCounts[0].toObject().value("count").toInt(), 2);

    auto samp = cmd(t, "catalog",
                    R"({"aggregate":"products","pipeline":[{"$sample":{"size":2}}]})");
    ASSERT_TRUE(samp.ok);
    EXPECT_EQ(firstBatch(samp).size(), 2);
}

TEST(InMemoryDemoTransportTest, indexes_stats_and_explain_plan)
{
    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);

    // Unique index creation on non-unique field `tier` fails with 11000.
    auto badUnique = cmd(t, "catalog",
                         R"({"createIndexes":"customers","indexes":[{"key":{"tier":1},"name":"tier_unique","unique":true}]})");
    EXPECT_FALSE(badUnique.ok);
    EXPECT_EQ(badUnique.code, 11000);

    // Non-unique index succeeds and appears in listIndexes.
    auto okIdx = cmd(t, "catalog",
                     R"({"createIndexes":"customers","indexes":[{"key":{"tier":1},"name":"tier_1"}]})");
    ASSERT_TRUE(okIdx.ok) << okIdx.error.toStdString();
    EXPECT_EQ(firstBatch(cmd(t, "catalog", R"({"listIndexes":"customers"})")).size(), 2);

    // Dropping `_id_` is refused with code 72.
    auto dropId = cmd(t, "catalog", R"({"dropIndexes":"customers","index":"_id_"})");
    EXPECT_FALSE(dropId.ok);
    EXPECT_EQ(dropId.code, 72);

    // collStats & dbStats return valid numbers.
    auto cs = cmd(t, "catalog", R"({"collStats":"products"})");
    ASSERT_TRUE(cs.ok);
    EXPECT_EQ(toRelaxedObj(cs).value("count").toInt(), 3);

    auto ds = cmd(t, "catalog", R"({"dbStats":1})");
    ASSERT_TRUE(ds.ok);
    EXPECT_EQ(toRelaxedObj(ds).value("collections").toInt(), 3);

    // explain on indexed field uses IXSCAN and parses cleanly in ExplainPlanParser.
    auto exp = cmd(t, "catalog",
                   R"({"explain":{"find":"products","filter":{"sku":"KB-01"}}})");
    ASSERT_TRUE(exp.ok) << exp.error.toStdString();
    const auto expDoc = bsoncxx::from_json(exp.ejson.toStdString());
    ASSERT_TRUE(ExplainPlanParser::isExplain(expDoc.view()));
    const ExplainPlan plan = ExplainPlanParser::parse(expDoc.view());
    EXPECT_TRUE(plan.valid);
    EXPECT_EQ(plan.nReturned, 1);
    EXPECT_TRUE(plan.indexesUsed.contains(QStringLiteral("sku_1")));
}

TEST(InMemoryDemoTransportTest, lossless_bson_types_survive_roundtrip)
{
    namespace basic = bsoncxx::builder::basic;
    using basic::kvp;
    using basic::make_array;
    using basic::make_document;

    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);

    const auto insertCmd = make_document(
        kvp("insert", "ledger"),
        kvp("documents", make_array(make_document(
            kvp("_id", bsoncxx::oid("507f1f77bcf86cd799439011")),
            kvp("seqA", static_cast<std::int64_t>(9007199254740992LL)),
            kvp("seqB", static_cast<std::int64_t>(9007199254740993LL)),
            kvp("amount", bsoncxx::types::b_decimal128{bsoncxx::decimal128("124.50")}),
            kvp("at", bsoncxx::types::b_date{std::chrono::milliseconds(1700000000123LL)})))));
    const auto insRes = t.runCommand(
        QStringLiteral("catalog"),
        QString::fromStdString(bsoncxx::to_json(insertCmd.view(), bsoncxx::ExtendedJsonMode::k_canonical)));
    ASSERT_TRUE(insRes.ok) << insRes.error.toStdString();

    // Exact 64-bit integer filter distinguishes 2^53 from 2^53 + 1.
    const auto missCmd = make_document(
        kvp("find", "ledger"),
        kvp("filter", make_document(kvp("seqB", static_cast<std::int64_t>(9007199254740992LL)))));
    const auto missRes = t.runCommand(
        QStringLiteral("catalog"),
        QString::fromStdString(bsoncxx::to_json(missCmd.view(), bsoncxx::ExtendedJsonMode::k_canonical)));
    ASSERT_TRUE(missRes.ok);
    EXPECT_EQ(firstBatch(missRes).size(), 0);

    const auto hitCmd = make_document(
        kvp("find", "ledger"),
        kvp("filter", make_document(kvp("seqB", static_cast<std::int64_t>(9007199254740993LL)))));
    const auto hitRes = t.runCommand(
        QStringLiteral("catalog"),
        QString::fromStdString(bsoncxx::to_json(hitCmd.view(), bsoncxx::ExtendedJsonMode::k_canonical)));
    ASSERT_TRUE(hitRes.ok);
    const auto replyDoc = bsoncxx::from_json(hitRes.ejson.toStdString());
    const auto batch = replyDoc.view()["cursor"]["firstBatch"].get_array().value;
    ASSERT_NE(batch.begin(), batch.end());
    const auto d = batch.begin()->get_document().value;
    EXPECT_EQ(d["_id"].get_oid().value.to_string(), "507f1f77bcf86cd799439011");
    EXPECT_EQ(d["seqB"].get_int64().value, 9007199254740993LL);
    EXPECT_EQ(d["amount"].get_decimal128().value.to_string(), "124.50");
    EXPECT_EQ(d["at"].get_date().to_int64(), 1700000000123LL);
}

TEST(InMemoryDemoTransportTest, shared_store_visibility_reset_and_resource_caps)
{
    InMemoryDemoTransport::resetSharedStore();
    InMemoryDemoTransport explorer(InMemoryDemoTransport::sharedStore());
    InMemoryDemoTransport shell(InMemoryDemoTransport::sharedStore());
    InMemoryDemoTransport isolated;
    ASSERT_TRUE(explorer.connect(QStringLiteral("{}")).ok);
    ASSERT_TRUE(shell.connect(QStringLiteral("{}")).ok);
    ASSERT_TRUE(isolated.connect(QStringLiteral("{}")).ok);

    ASSERT_TRUE(cmd(shell, "catalog", R"({"create":"audit_log"})").ok);
    EXPECT_TRUE(explorer.listCollections(QStringLiteral("catalog")).ejson.contains(
        QStringLiteral("audit_log")));
    EXPECT_FALSE(isolated.listCollections(QStringLiteral("catalog")).ejson.contains(
        QStringLiteral("audit_log")));

    InMemoryDemoTransport::resetSharedStore();
    EXPECT_FALSE(explorer.listCollections(QStringLiteral("catalog")).ejson.contains(
        QStringLiteral("audit_log")));

    // Database cap enforcement (`kMaxDatabases`).
    for (std::size_t i = 1; i < InMemoryDemoTransport::kMaxDatabases; ++i) {
        auto r = cmd(isolated, QStringLiteral("db_%1").arg(i), R"({"create":"c"})");
        ASSERT_TRUE(r.ok) << r.error.toStdString();
    }
    auto overDb = cmd(isolated, QStringLiteral("db_overflow"), R"({"create":"c"})");
    EXPECT_FALSE(overDb.ok);
    EXPECT_EQ(overDb.code, 8000);
}

TEST(InMemoryDemoTransportTest, security_hardening_bounds_and_invariants)
{
    namespace basic = bsoncxx::builder::basic;
    using basic::kvp;
    using basic::make_array;
    using basic::make_document;

    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);

    // 1. Subpath `_id.sub` and `$inc` on `_id` must be rejected with code 66 (ImmutableField).
    auto idSub = cmd(t, "catalog",
                     R"({"update":"products","updates":[{"q":{"_id":1},"u":{"$set":{"_id.sub":1}}}]})");
    ASSERT_TRUE(idSub.ok);
    ASSERT_EQ(toRelaxedObj(idSub).value("writeErrors").toArray().size(), 1);
    EXPECT_EQ(toRelaxedObj(idSub).value("writeErrors").toArray()[0].toObject().value("code").toInt(), 66);

    auto idInc = cmd(t, "catalog",
                     R"({"update":"products","updates":[{"q":{"_id":1},"u":{"$inc":{"_id":1}}}]})");
    ASSERT_TRUE(idInc.ok);
    ASSERT_EQ(toRelaxedObj(idInc).value("writeErrors").toArray().size(), 1);
    EXPECT_EQ(toRelaxedObj(idInc).value("writeErrors").toArray()[0].toObject().value("code").toInt(), 66);

    // 2. Empty path segments (`"a..b"`, `".a"`) -> 56; deep path (> 64 segments) -> 2.
    auto emptySeg = cmd(t, "catalog",
                        R"({"update":"products","updates":[{"q":{"_id":1},"u":{"$set":{"a..b":1}}}]})");
    ASSERT_TRUE(emptySeg.ok);
    EXPECT_EQ(toRelaxedObj(emptySeg).value("writeErrors").toArray()[0].toObject().value("code").toInt(), 56);

    QStringList deepSegs;
    for (int i = 0; i < 70; ++i)
        deepSegs << QStringLiteral("p%1").arg(i);
    const QString deepPath = deepSegs.join(QLatin1Char('.'));
    auto deepUpd = cmd(t, "catalog",
                       QStringLiteral(R"({"update":"products","updates":[{"q":{"_id":1},"u":{"$set":{"%1":1}}}]})")
                           .arg(deepPath));
    ASSERT_TRUE(deepUpd.ok);
    EXPECT_EQ(toRelaxedObj(deepUpd).value("writeErrors").toArray()[0].toObject().value("code").toInt(), 2);

    // Deeply nested JSON (> 100 levels) is rejected by libbson parser (code 9), while
    // 68-level nested `$not` (< 100 JSON levels, > kMaxDepth=64) stops safely without stack overflow.
    QString nestedFilter = QStringLiteral(R"({"sku":"KB-01"})");
    for (int i = 0; i < 80; ++i)
        nestedFilter = QStringLiteral(R"({"$and":[%1]})").arg(nestedFilter);
    auto deepFind = cmd(t, "catalog",
                        QStringLiteral(R"({"find":"products","filter":%1})").arg(nestedFilter));
    EXPECT_FALSE(deepFind.ok);
    EXPECT_EQ(deepFind.code, 9);

    QString deepNot = QStringLiteral(R"({"$eq":"KB-01"})");
    for (int i = 0; i < 68; ++i)
        deepNot = QStringLiteral(R"({"$not":%1})").arg(deepNot);
    auto deepNotFind = cmd(t, "catalog",
                           QStringLiteral(R"({"find":"products","filter":{"sku":%1}})").arg(deepNot));
    ASSERT_TRUE(deepNotFind.ok) << deepNotFind.error.toStdString();

    // 3. INT64_MAX `$inc: 1` and INT64_MIN `$mul: -1` promote cleanly to double without signed UB.
    const auto seedExtreme = make_document(
        kvp("insert", "counters"),
        kvp("documents", make_array(
            make_document(kvp("_id", 1), kvp("v", INT64_MAX)),
            make_document(kvp("_id", 2), kvp("v", INT64_MIN)))));
    ASSERT_TRUE(t.runCommand("catalog",
        QString::fromStdString(bsoncxx::to_json(seedExtreme.view(), bsoncxx::ExtendedJsonMode::k_canonical))).ok);
    ASSERT_TRUE(cmd(t, "catalog",
                    R"({"update":"counters","updates":[{"q":{"_id":1},"u":{"$inc":{"v":1}}},{"q":{"_id":2},"u":{"$mul":{"v":-1}}}]})").ok);
    const auto cntReply = bsoncxx::from_json(
        cmd(t, "catalog", R"({"find":"counters","sort":{"_id":1}})").ejson.toStdString());
    const auto cntBatch = cntReply.view()["cursor"]["firstBatch"].get_array().value;
    ASSERT_EQ(std::distance(cntBatch.begin(), cntBatch.end()), 2);
    EXPECT_EQ(cntBatch.begin()->get_document().value["v"].type(), bsoncxx::type::k_double);

    // 4. `findAndModify` and `aggregate` `$addFields` enforce `kMaxDocumentBytes` (10334).
    const std::string bigChunk(150 * 1024, 'x');
    ASSERT_TRUE(t.runCommand("catalog", QString::fromStdString(bsoncxx::to_json(
        make_document(kvp("insert", "blobs"),
                      kvp("documents", make_array(make_document(kvp("_id", 1), kvp("a", bigChunk))))).view(),
        bsoncxx::ExtendedJsonMode::k_canonical))).ok);
    const auto famOver = t.runCommand("catalog", QString::fromStdString(bsoncxx::to_json(
        make_document(kvp("findAndModify", "blobs"),
                      kvp("query", make_document(kvp("_id", 1))),
                      kvp("update", make_document(kvp("$set", make_document(kvp("b", bigChunk))))),
                      kvp("new", true)).view(),
        bsoncxx::ExtendedJsonMode::k_canonical)));
    EXPECT_FALSE(famOver.ok);
    EXPECT_EQ(famOver.code, 10334);

    const auto aggOver = cmd(t, "catalog",
        R"({"aggregate":"blobs","pipeline":[{"$addFields":{"b":{"$concat":["$a","$a"]}}}]})");
    EXPECT_FALSE(aggOver.ok);
    EXPECT_EQ(aggOver.code, 10334);

    // 5. `renameCollection` preserves source on invalid/reserved/self target and updates index `ns`.
    EXPECT_FALSE(cmd(t, "admin", R"({"renameCollection":"catalog.customers","to":"admin.customers"})").ok);
    EXPECT_FALSE(cmd(t, "admin", R"({"renameCollection":"catalog.customers","to":"catalog."})").ok);
    EXPECT_FALSE(cmd(t, "admin", R"({"renameCollection":"catalog.customers","to":"catalog.customers"})").ok);
    EXPECT_EQ(firstBatch(cmd(t, "catalog", R"({"find":"customers"})")).size(), 3);

    ASSERT_TRUE(cmd(t, "admin", R"({"renameCollection":"catalog.customers","to":"catalog.clients"})").ok);
    EXPECT_EQ(firstBatch(cmd(t, "catalog", R"({"find":"clients"})")).size(), 3);
    EXPECT_EQ(firstBatch(cmd(t, "catalog", R"({"listIndexes":"clients"})"))[0]
                  .toObject().value("ns").toString(),
              QStringLiteral("catalog.clients"));

    // `createIndexes` reports `createdCollectionAutomatically` accurately.
    auto autoCreated = cmd(t, "catalog",
                           R"({"createIndexes":"brand_new","indexes":[{"key":{"x":1},"name":"x_1"}]})");
    ASSERT_TRUE(autoCreated.ok);
    EXPECT_TRUE(toRelaxedObj(autoCreated).value("createdCollectionAutomatically").toBool());

    // 6. Catastrophic nested-quantifier `$regex` is rejected cleanly without hanging.
    const std::string longA(40, 'a');
    ASSERT_TRUE(t.runCommand("catalog", QString::fromStdString(bsoncxx::to_json(
        make_document(kvp("insert", "redos"),
                      kvp("documents", make_array(make_document(kvp("s", longA + "!"))))).view(),
        bsoncxx::ExtendedJsonMode::k_canonical))).ok);
    auto redosRes = cmd(t, "catalog", R"({"find":"redos","filter":{"s":{"$regex":"^(a+)+$"}}})");
    ASSERT_TRUE(redosRes.ok);
    EXPECT_EQ(firstBatch(redosRes).size(), 0);
}
