#include <gtest/gtest.h>

#include <iostream>

#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/instance.hpp>

#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/utils/AggregationPipeline.h"

using namespace RagoShell;

TEST(AggregationPipelineTest, parse_stage_accepts_shell_json_and_scalars)
{
    bsoncxx::document::value doc = bsoncxx::from_json("{}");
    QString error;

    ASSERT_TRUE(AggregationPipeline::parseStage(
        {"$match", "{ borough: 'Brooklyn', _id: { $gt: ObjectId(\"000000000000000000000001\") } }"}, doc, error))
        << error.toStdString();
    EXPECT_TRUE(doc.view()["$match"]);

    ASSERT_TRUE(AggregationPipeline::parseStage({"$limit", "5"}, doc, error)) << error.toStdString();
    EXPECT_EQ(doc.view()["$limit"].get_int32().value, 5);

    ASSERT_TRUE(AggregationPipeline::parseStage({"$unwind", "\"$grades\""}, doc, error));
    EXPECT_TRUE(doc.view()["$unwind"]);
}

TEST(AggregationPipelineTest, parse_stage_rejects_invalid_input)
{
    bsoncxx::document::value doc = bsoncxx::from_json("{}");
    QString error;
    EXPECT_FALSE(AggregationPipeline::parseStage({"match", "{}"}, doc, error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(AggregationPipeline::parseStage({"$match", "  "}, doc, error));
    EXPECT_FALSE(AggregationPipeline::parseStage({"$match", "{ a: 1 "}, doc, error));
    // A stray "}, {" would smuggle in a second operator.
    EXPECT_FALSE(AggregationPipeline::parseStage({"$match", "{ a: 1 }, $limit: 2"}, doc, error));
}

TEST(AggregationPipelineTest, script_skips_disabled_stages_and_quotes_collection)
{
    std::vector<PipelineStage> stages = {
        {"$match", "{ state: 'NY' }", true},
        {"$sort", "{ pop: -1 }", false},
        {"$limit", "3", true},
    };
    EXPECT_EQ(AggregationPipeline::pipelineScript("zips", stages),
              QStringLiteral("db.getCollection('zips').aggregate([\n"
                             "    { $match: { state: 'NY' } },\n"
                             "    { $limit: 3 }\n"
                             "])"));
    EXPECT_EQ(AggregationPipeline::pipelineScript("it's", {}),
              QStringLiteral("db.getCollection('it\\'s').aggregate([])"));

    EXPECT_TRUE(AggregationPipeline::isWriteStage("$out"));
    EXPECT_TRUE(AggregationPipeline::isWriteStage("$merge"));
    EXPECT_FALSE(AggregationPipeline::isWriteStage("$group"));
    EXPECT_TRUE(AggregationPipeline::commonOperators().contains("$lookup"));
    EXPECT_EQ(AggregationPipeline::stageTemplate("$limit"), QStringLiteral("10"));
}

// Live: dev/mongo-test MongoDB with the seeded "samples" db.
TEST(AggregationPipelineTest, live_preview_against_samples)
{
    // One mongocxx::instance per process; another test may already own it.
    try {
        static mongocxx::instance instance{};
    } catch (const std::exception &) {
    }
    ConnectionSettings conn(/*isClone=*/false);
    conn.setServerHost("localhost");
    conn.setServerPort(27017);

    bsoncxx::document::value match = bsoncxx::from_json("{}");
    bsoncxx::document::value group = bsoncxx::from_json("{}");
    QString error;
    ASSERT_TRUE(AggregationPipeline::parseStage({"$match", "{ state: 'NY' }"}, match, error));
    ASSERT_TRUE(AggregationPipeline::parseStage(
        {"$group", "{ _id: '$city', pop: { $sum: '$pop' } }"}, group, error));

    std::vector<bsoncxx::document::value> stages;
    stages.push_back(match);
    stages.push_back(group);
    const auto result = AggregationPipeline::runPreview(conn, "samples", "zips", stages, 5, 10000);
    if (!result.ok && result.error.contains("No suitable servers", Qt::CaseInsensitive)) {
        // gtest 1.8 has no GTEST_SKIP; report and pass.
        std::cerr << "[ SKIP ] MongoDB not reachable on localhost:27017\n";
        return;
    }
    ASSERT_TRUE(result.ok) << result.error.toStdString();
    // samples.zips may be missing on a fresh container; then 0 documents is valid.
    EXPECT_LE(result.documents.size(), 5u);
    for (const auto &doc : result.documents) {
        EXPECT_TRUE(doc.view()["_id"]);
        EXPECT_TRUE(doc.view()["pop"]);
    }
}
