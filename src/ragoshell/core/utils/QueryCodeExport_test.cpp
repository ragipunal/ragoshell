#include <gtest/gtest.h>

#include <QFile>

#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

#include "ragoshell/core/utils/MongoJsonUtils.h"
#include "ragoshell/core/utils/QueryCodeExport.h"

using namespace RagoShell;
using Lang = QueryCodeExport::Language;

namespace
{
    QString exportAs(const QString &script, Lang lang)
    {
        const auto r = QueryCodeExport::exportScript(script, "samples", "mongodb://localhost:27017", lang);
        EXPECT_TRUE(r.ok) << r.error.toStdString();
        return r.code;
    }

    const char *kFind =
        "db.getCollection('zips').find({ state: 'NY', pop: { $gt: NumberLong(1000) },"
        " _id: { $ne: ObjectId(\"5c8eccc1caa187d17ca6ed16\") } }, { city: 1, pop: 1 })"
        ".sort({ pop: -1 }).skip(5).limit(10)";
}

TEST(QueryCodeExportTest, parse_aggregate_chain_variants)
{
    auto a = MongoJsonUtils::parseAggregateChain("db.zips.aggregate([{ $match: {} }]);");
    EXPECT_TRUE(a.valid);
    EXPECT_EQ(a.collection, QStringLiteral("zips"));
    EXPECT_EQ(a.pipeline, QStringLiteral("[{ $match: {} }]"));

    a = MongoJsonUtils::parseAggregateChain("db.getCollection('my coll').aggregate([], { allowDiskUse: true })");
    EXPECT_TRUE(a.valid);
    EXPECT_EQ(a.collection, QStringLiteral("my coll"));

    EXPECT_TRUE(MongoJsonUtils::parseAggregateChain("db.system.users.aggregate([])").valid);
    EXPECT_FALSE(MongoJsonUtils::parseAggregateChain("db.zips.aggregate([]).toArray()").valid);
    EXPECT_FALSE(MongoJsonUtils::parseAggregateChain("db.zips.find({})").valid);
}

TEST(QueryCodeExportTest, python_keeps_bson_types_and_pymongo_sort)
{
    const QString code = exportAs(kFind, Lang::Python);
    EXPECT_TRUE(code.contains("from pymongo import MongoClient"));
    EXPECT_TRUE(code.contains("from bson import Int64, ObjectId")) << code.toStdString();
    EXPECT_TRUE(code.contains("client[\"samples\"][\"zips\"]"));
    EXPECT_TRUE(code.contains("Int64(1000)"));
    EXPECT_TRUE(code.contains("ObjectId(\"5c8eccc1caa187d17ca6ed16\")"));
    EXPECT_TRUE(code.contains("projection={\"city\": 1, \"pop\": 1}"));
    EXPECT_TRUE(code.contains("sort=[(\"pop\", -1)]"));
    EXPECT_TRUE(code.contains("skip=5"));
    EXPECT_TRUE(code.contains("limit=10"));
}

TEST(QueryCodeExportTest, node_java_csharp_find)
{
    const QString node = exportAs(kFind, Lang::NodeJs);
    EXPECT_TRUE(node.contains("const { MongoClient, Long, ObjectId } = require(\"mongodb\");")) << node.toStdString();
    EXPECT_TRUE(node.contains("Long.fromString(\"1000\")"));
    EXPECT_TRUE(node.contains("new ObjectId(\"5c8eccc1caa187d17ca6ed16\")"));
    EXPECT_TRUE(node.contains("sort: { pop: -1 }"));
    EXPECT_TRUE(node.contains("limit: 10"));

    const QString java = exportAs(kFind, Lang::Java);
    EXPECT_TRUE(java.contains("import com.mongodb.client.FindIterable;"));
    EXPECT_TRUE(java.contains(".sort(Document.parse(\"{ \\\"pop\\\" : -1 }\"))")) << java.toStdString();
    EXPECT_TRUE(java.contains("\\\"$oid\\\""));
    EXPECT_TRUE(java.contains(".limit(10);"));

    const QString cs = exportAs(kFind, Lang::CSharp);
    EXPECT_TRUE(cs.contains("using MongoDB.Driver;"));
    EXPECT_TRUE(cs.contains(".Sort(BsonDocument.Parse(@\"{ \"\"pop\"\" : -1 }\"))")) << cs.toStdString();
    EXPECT_TRUE(cs.contains(".ToList();"));
}

TEST(QueryCodeExportTest, aggregate_and_special_values)
{
    const QString script =
        "db.events.aggregate([ { $match: { when: { $gte: ISODate(\"2024-01-02T00:00:00Z\") },"
        " name: /^ab/i, ratio: 5.0, ok: true, gone: null } }, { $limit: 3 } ])";

    const QString py = exportAs(script, Lang::Python);
    EXPECT_TRUE(py.contains("collection.aggregate(["));
    EXPECT_TRUE(py.contains("datetime.fromisoformat(\"2024-01-02T00:00:00.000+00:00\")")) << py.toStdString();
    EXPECT_TRUE(py.contains("Regex(\"^ab\", \"i\")"));
    EXPECT_TRUE(py.contains("\"ratio\": 5.0"));
    EXPECT_TRUE(py.contains("\"ok\": True"));
    EXPECT_TRUE(py.contains("\"gone\": None"));
    EXPECT_TRUE(py.contains("{\"$limit\": 3}"));

    const QString node = exportAs(script, Lang::NodeJs);
    EXPECT_TRUE(node.contains("new Date(\"2024-01-02T00:00:00.000Z\")")) << node.toStdString();
    EXPECT_TRUE(node.contains("new BSONRegExp(\"^ab\", \"i\")"));
    EXPECT_TRUE(node.contains("{ $limit: 3 }"));

    EXPECT_TRUE(exportAs(script, Lang::Java).contains("collection.aggregate(Arrays.asList("));
    EXPECT_TRUE(exportAs(script, Lang::CSharp).contains("collection.Aggregate<BsonDocument>(pipeline)"));
}

TEST(QueryCodeExportTest, shell_json_regex_literals)
{
    bsoncxx::document::value doc = bsoncxx::from_json("{}");
    QString err;
    // A '//' inside the regex is not a comment; '/' inside strings is left alone.
    ASSERT_TRUE(MongoJsonUtils::parseSingleDocument(
        "{ url: /^https?:\\/\\/[^/]+\\/x/mi, path: 'a/b', list: [ /c/ ] } // trailing comment",
        doc, err)) << err.toStdString();
    const auto url = doc.view()["url"];
    ASSERT_EQ(url.type(), bsoncxx::type::k_regex);
    EXPECT_EQ(std::string(url.get_regex().regex), "^https?:\\/\\/[^/]+\\/x");
    EXPECT_EQ(std::string(url.get_regex().options), "im");   // sorted
    EXPECT_EQ(std::string(doc.view()["path"].get_string().value), "a/b");
    EXPECT_EQ(doc.view()["list"].get_array().value[0].type(), bsoncxx::type::k_regex);
}

// Writes generated samples for running them with real drivers:
// RAGO_EXPORT_SAMPLES_DIR=<dir> rago_unit_tests --gtest_filter=*write_samples*
TEST(QueryCodeExportTest, write_samples_when_requested)
{
    const QString dir = qEnvironmentVariable("RAGO_EXPORT_SAMPLES_DIR");
    if (dir.isEmpty())
        return;
    const struct { const char *name; const char *script; } queries[] = {
        {"find", kFind},
        {"aggregate", "db.zips.aggregate([ { $match: { state: 'NY', city: /^BRO/i } },"
                      " { $group: { _id: '$city', pop: { $sum: '$pop' } } }, { $sort: { pop: -1 } }, { $limit: 3 } ])"},
    };
    const struct { Lang lang; const char *ext; } langs[] = {
        {Lang::Python, "py"}, {Lang::NodeJs, "js"}, {Lang::Java, "java"}, {Lang::CSharp, "cs"},
    };
    for (const auto &q : queries) {
        for (const auto &l : langs) {
            QFile f(QStringLiteral("%1/%2.%3").arg(dir, q.name, l.ext));
            ASSERT_TRUE(f.open(QIODevice::WriteOnly));
            f.write(exportAs(q.script, l.lang).toUtf8());
        }
    }
}

TEST(QueryCodeExportTest, rejects_other_statements)
{
    for (const char *script : {"db.zips.deleteMany({})", "show dbs", "db.zips.find({ a: 1 "}) {
        const auto r = QueryCodeExport::exportScript(script, "samples", "", Lang::Python);
        EXPECT_FALSE(r.ok) << script;
        EXPECT_FALSE(r.error.isEmpty());
    }
}
