#include <gtest/gtest.h>

#include <iostream>

#include <bsoncxx/json.hpp>
#include <mongocxx/instance.hpp>

#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/utils/SchemaAnalysis.h"

using namespace RagoShell;

namespace
{
    const SchemaField *field(const SchemaResult &r, const char *path)
    {
        for (const auto &f : r.fields)
            if (f.path == QLatin1String(path))
                return &f;
        return nullptr;
    }

    qint64 typeCount(const SchemaField &f, const char *type)
    {
        for (const auto &t : f.types)
            if (t.first == QLatin1String(type))
                return t.second;
        return 0;
    }
}

TEST(SchemaAnalysisTest, presence_types_nesting_values_and_ranges)
{
    SchemaAccumulator acc;
    acc.add(bsoncxx::from_json(R"({ "name": "a", "age": 30, "tags": ["x", "y"],
        "address": { "city": "Ankara" }, "grades": [ { "score": 5 }, { "score": 9 } ] })").view());
    acc.add(bsoncxx::from_json(R"({ "name": "b", "age": 41.5, "address": { "city": "Izmir", "zip": "35" } })").view());
    acc.add(bsoncxx::from_json(R"({ "name": "a", "age": "unknown", "when": { "$date": "2024-01-02T00:00:00Z" } })").view());
    acc.add(bsoncxx::from_json(R"({ "name": "c", "when": { "$date": "2023-05-06T00:00:00Z" } })").view());

    const SchemaResult r = acc.result();
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.sampledDocuments, 4);

    const SchemaField *name = field(r, "name");
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->presentCount, 4);
    EXPECT_DOUBLE_EQ(name->presence, 1.0);
    ASSERT_FALSE(name->topValues.empty());
    EXPECT_EQ(name->topValues[0].first, QStringLiteral("\"a\""));   // shell-style string
    EXPECT_EQ(name->topValues[0].second, 2);

    // Mixed types are counted per type; the number range ignores the string.
    const SchemaField *age = field(r, "age");
    ASSERT_NE(age, nullptr);
    EXPECT_DOUBLE_EQ(age->presence, 0.75);
    EXPECT_EQ(typeCount(*age, "Int32"), 1);
    EXPECT_EQ(typeCount(*age, "Double"), 1);
    EXPECT_EQ(typeCount(*age, "String"), 1);
    EXPECT_EQ(age->minValue, QStringLiteral("30"));
    EXPECT_EQ(age->maxValue, QStringLiteral("41.5"));

    // Dates get a range too; earliest first.
    const SchemaField *when = field(r, "when");
    ASSERT_NE(when, nullptr);
    EXPECT_TRUE(when->minValue.contains("2023-05-06")) << when->minValue.toStdString();
    EXPECT_TRUE(when->maxValue.contains("2024-01-02")) << when->maxValue.toStdString();

    // Embedded documents: dotted paths, presence relative to all documents.
    const SchemaField *city = field(r, "address.city");
    ASSERT_NE(city, nullptr);
    EXPECT_EQ(city->depth, 1);
    EXPECT_EQ(city->presentCount, 2);
    EXPECT_EQ(field(r, "address.zip")->presentCount, 1);

    // Arrays: element types, and fields of documents inside the array counted once per document.
    const SchemaField *tags = field(r, "tags");
    ASSERT_NE(tags, nullptr);
    ASSERT_EQ(tags->itemTypes.size(), 1u);
    EXPECT_EQ(tags->itemTypes[0].first, QStringLiteral("String"));
    EXPECT_EQ(tags->itemTypes[0].second, 2);
    const SchemaField *score = field(r, "grades.score");
    ASSERT_NE(score, nullptr);
    EXPECT_EQ(score->presentCount, 1);
    EXPECT_EQ(typeCount(*score, "Int32"), 2);
    EXPECT_EQ(score->minValue, QStringLiteral("5"));
    EXPECT_EQ(score->maxValue, QStringLiteral("9"));

    // Parents come right before their children.
    int addressIdx = -1, cityIdx = -1;
    for (int i = 0; i < static_cast<int>(r.fields.size()); ++i) {
        if (r.fields[i].path == "address") addressIdx = i;
        if (r.fields[i].path == "address.city") cityIdx = i;
    }
    EXPECT_EQ(cityIdx, addressIdx + 1);
}

TEST(SchemaAnalysisTest, caps_tracked_distinct_values)
{
    SchemaAccumulator acc;
    for (int i = 0; i < SchemaAccumulator::kMaxTrackedValues + 10; ++i)
        acc.add(bsoncxx::from_json(QStringLiteral("{ \"n\": %1 }").arg(i).toStdString()).view());
    const SchemaResult r = acc.result();
    const SchemaField *n = field(r, "n");
    ASSERT_NE(n, nullptr);
    EXPECT_TRUE(n->valuesTruncated);
    EXPECT_EQ(static_cast<int>(n->topValues.size()), SchemaAccumulator::kTopValues);
    // The range still covers every document.
    EXPECT_EQ(n->maxValue, QString::number(SchemaAccumulator::kMaxTrackedValues + 9));
}

TEST(SchemaAnalysisTest, categories_are_listed_completely_and_letter_case_is_noted)
{
    SchemaAccumulator acc;
    const char *statuses[] = {"SHIPPED", "PENDING", "CANCELLED"};
    for (int i = 0; i < 30; ++i)
        acc.add(bsoncxx::from_json(QStringLiteral(R"({ "status": "%1", "city": "%2", "name": "Customer %3" })")
                                       .arg(statuses[i % 3], i % 2 ? "izmir" : "ankara").arg(i)
                                       .toStdString()).view());
    const SchemaResult r = acc.result();

    const SchemaField *status = field(r, "status");
    ASSERT_NE(status, nullptr);
    EXPECT_TRUE(status->categorical);
    EXPECT_EQ(status->distinctValues, 3);
    EXPECT_EQ(status->topValues.size(), 3u);
    EXPECT_EQ(status->letterCase, QStringLiteral("upper"));

    // Two values only: not enough to call the case.
    const SchemaField *city = field(r, "city");
    ASSERT_NE(city, nullptr);
    EXPECT_TRUE(city->categorical);
    EXPECT_TRUE(city->letterCase.isEmpty());

    // Every value different: not a category, top values only.
    const SchemaField *name = field(r, "name");
    ASSERT_NE(name, nullptr);
    EXPECT_FALSE(name->categorical);
    EXPECT_EQ(static_cast<int>(name->topValues.size()), SchemaAccumulator::kTopValues);
    EXPECT_TRUE(name->letterCase.isEmpty());   // "Customer 1": mixed case
}

TEST(SchemaAnalysisTest, many_values_are_not_a_category)
{
    SchemaAccumulator acc;
    for (int i = 0; i < 200; ++i)
        acc.add(bsoncxx::from_json(QStringLiteral(R"({ "code": "c%1" })").arg(i % 40).toStdString()).view());
    const SchemaResult r = acc.result();
    const SchemaField *code = field(r, "code");
    ASSERT_NE(code, nullptr);
    EXPECT_EQ(code->distinctValues, 40);
    EXPECT_FALSE(code->categorical);   // more than kMaxCategoryValues
    EXPECT_EQ(code->letterCase, QStringLiteral("lower"));
}

TEST(SchemaAnalysisTest, fields_seen_only_in_the_newest_documents)
{
    SchemaAccumulator acc;
    for (int i = 0; i < 5; ++i)
        acc.add(bsoncxx::from_json(R"({ "name": "old", "legacy": 1 })").view());
    for (int i = 0; i < 3; ++i)
        acc.add(bsoncxx::from_json(R"({ "name": "new", "tags": ["a"] })").view(), /*recent=*/true);
    const SchemaResult r = acc.result();
    EXPECT_EQ(r.sampledDocuments, 8);
    EXPECT_EQ(r.recentDocuments, 3);
    EXPECT_TRUE(field(r, "tags")->onlyRecent);
    EXPECT_DOUBLE_EQ(field(r, "tags")->presence, 1.0);   // share among the newest
    EXPECT_FALSE(field(r, "name")->onlyRecent);
    EXPECT_FALSE(field(r, "legacy")->onlyRecent);

    // Statistics come from the random documents only: the newest ones lack
    // "legacy" and say "new", but neither shows.
    EXPECT_DOUBLE_EQ(field(r, "legacy")->presence, 1.0);
    const SchemaField *name = field(r, "name");
    ASSERT_EQ(name->topValues.size(), 1u);
    EXPECT_EQ(name->topValues[0].first, QStringLiteral("\"old\""));
    EXPECT_EQ(name->presentCount, 5);

    // Without random documents there is nothing to compare with.
    SchemaAccumulator recentOnly;
    recentOnly.add(bsoncxx::from_json(R"({ "x": 1 })").view(), /*recent=*/true);
    EXPECT_FALSE(field(recentOnly.result(), "x")->onlyRecent);
}

namespace
{
    // Live tests: dev/mongo-test MongoDB with the seeded "samples" db; skipped when absent.
    bool liveResult(const char *collection, int sampleSize, SchemaResult &r)
    {
        try {
            static mongocxx::instance instance{};
        } catch (const std::exception &) {
        }
        ConnectionSettings conn(/*isClone=*/false);
        conn.setServerHost("localhost");
        conn.setServerPort(27017);
        r = SchemaAnalysis::analyzeCollection(conn, "samples", collection, sampleSize, 10000, DefaultEncoding, Utc);
        // The driver reports a missing server either way, depending on timing.
        if (!r.ok && (r.error.contains("No suitable servers", Qt::CaseInsensitive)
                      || r.error.contains("No servers yet eligible", Qt::CaseInsensitive))) {
            std::cerr << "[ SKIP ] MongoDB not reachable on localhost:27017\n";
            return false;
        }
        EXPECT_TRUE(r.ok) << r.error.toStdString();
        return r.ok && r.sampledDocuments > 0;   // false: collection not seeded
    }
}

TEST(SchemaAnalysisTest, live_restaurants_sample)
{
    SchemaResult r;
    if (!liveResult("restaurants", 200, r))
        return;
    // 25k documents: 60 newest (ObjectId _id) plus a random sample.
    EXPECT_FALSE(r.fullScan);
    EXPECT_GT(r.totalDocuments, SchemaAnalysis::kFullScanLimit);
    EXPECT_LE(r.sampledDocuments, 200);
    EXPECT_EQ(r.recentDocuments, 60);
    EXPECT_TRUE(r.analyzedAt.isValid());
    const SchemaField *id = field(r, "_id");
    ASSERT_NE(id, nullptr);
    EXPECT_DOUBLE_EQ(id->presence, 1.0);
    EXPECT_NE(field(r, "address.zipcode"), nullptr);
    EXPECT_NE(field(r, "grades.score"), nullptr);
    const SchemaField *borough = field(r, "borough");
    ASSERT_NE(borough, nullptr);
    EXPECT_TRUE(borough->categorical);
}

TEST(SchemaAnalysisTest, live_small_collection_is_read_completely)
{
    SchemaResult r;
    if (!liveResult("books", 100, r))
        return;
    EXPECT_TRUE(r.fullScan);
    EXPECT_EQ(r.sampledDocuments, r.totalDocuments);   // 431, more than the sample size
    EXPECT_EQ(r.recentDocuments, 0);
    const SchemaField *status = field(r, "status");   // "PUBLISH", "MEAP"
    ASSERT_NE(status, nullptr);
    EXPECT_TRUE(status->categorical);
}

TEST(SchemaAnalysisTest, live_string_ids_are_sampled_randomly_only)
{
    SchemaResult r;
    if (!liveResult("zips", 300, r))
        return;
    EXPECT_FALSE(r.fullScan);
    EXPECT_EQ(r.recentDocuments, 0);   // "_id": "01001", not time-ordered
    EXPECT_GT(r.sampledDocuments, 250);
    const SchemaField *city = field(r, "city");
    ASSERT_NE(city, nullptr);
    EXPECT_EQ(city->letterCase, QStringLiteral("upper"));
}
