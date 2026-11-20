#include <gtest/gtest.h>

#include <bsoncxx/json.hpp>

#include "ragoshell/core/utils/ExplainPlan.h"

using namespace RagoShell;

namespace
{
    // Trimmed real output of MongoDB 4.0.28:
    // db.restaurants.find({borough:"Brooklyn"}).sort({name:1}).limit(5).explain("executionStats")
    const char *kCollScanSortExplain = R"({
        "queryPlanner": { "namespace": "samples.restaurants",
            "winningPlan": { "stage": "SORT", "sortPattern": { "name": 1 }, "limitAmount": 5,
                "inputStage": { "stage": "SORT_KEY_GENERATOR",
                    "inputStage": { "stage": "COLLSCAN", "filter": { "borough": { "$eq": "Brooklyn" } } } } } },
        "executionStats": { "nReturned": 5, "executionTimeMillis": 13,
            "totalKeysExamined": 0, "totalDocsExamined": 25359,
            "executionStages": { "stage": "SORT", "nReturned": 5, "executionTimeMillisEstimate": 1,
                "sortPattern": { "name": 1 }, "limitAmount": 5,
                "inputStage": { "stage": "SORT_KEY_GENERATOR", "nReturned": 6086,
                    "inputStage": { "stage": "COLLSCAN", "nReturned": 6086, "docsExamined": 25359,
                        "filter": { "borough": { "$eq": "Brooklyn" } } } } } }
    })";
}

TEST(ExplainPlanTest, collscan_with_in_memory_sort)
{
    const auto doc = bsoncxx::from_json(kCollScanSortExplain);
    ASSERT_TRUE(ExplainPlanParser::isExplain(doc.view()));

    const ExplainPlan plan = ExplainPlanParser::parse(doc.view());
    ASSERT_TRUE(plan.valid);
    EXPECT_TRUE(plan.hasExecutionStats);
    EXPECT_EQ(plan.ns, QStringLiteral("samples.restaurants"));
    EXPECT_EQ(plan.nReturned, 5);
    EXPECT_EQ(plan.totalDocsExamined, 25359);
    EXPECT_EQ(plan.executionTimeMs, 13);
    EXPECT_TRUE(plan.indexesUsed.isEmpty());

    ASSERT_EQ(plan.stages.size(), 3u);
    EXPECT_EQ(plan.stages[0].stage, QStringLiteral("SORT"));
    EXPECT_EQ(plan.stages[0].depth, 0);
    EXPECT_TRUE(plan.stages[0].detail.startsWith(QStringLiteral("sort ")));
    EXPECT_TRUE(plan.stages[0].warning);
    EXPECT_EQ(plan.stages[1].stage, QStringLiteral("SORT_KEY_GENERATOR"));
    EXPECT_FALSE(plan.stages[1].warning);
    EXPECT_EQ(plan.stages[2].stage, QStringLiteral("COLLSCAN"));
    EXPECT_EQ(plan.stages[2].depth, 2);
    EXPECT_EQ(plan.stages[2].docsExamined, 25359);
    EXPECT_TRUE(plan.stages[2].detail.contains(QStringLiteral("Brooklyn")));

    // COLLSCAN, in-memory SORT and the examined/returned ratio.
    EXPECT_EQ(plan.warnings.size(), 3);
}

TEST(ExplainPlanTest, index_scan_query_planner_only)
{
    const auto doc = bsoncxx::from_json(R"({
        "queryPlanner": { "namespace": "shop.orders",
            "winningPlan": { "stage": "FETCH",
                "inputStage": { "stage": "IXSCAN", "indexName": "customer_1", "keyPattern": { "customer": 1 } } } }
    })");
    const ExplainPlan plan = ExplainPlanParser::parse(doc.view());
    ASSERT_TRUE(plan.valid);
    EXPECT_FALSE(plan.hasExecutionStats);
    EXPECT_EQ(plan.nReturned, -1);
    ASSERT_EQ(plan.stages.size(), 2u);
    EXPECT_EQ(plan.stages[1].stage, QStringLiteral("IXSCAN"));
    EXPECT_TRUE(plan.stages[1].detail.startsWith(QStringLiteral("customer_1 ")));
    EXPECT_EQ(plan.indexesUsed, QStringList{QStringLiteral("customer_1")});
    EXPECT_TRUE(plan.warnings.isEmpty());
}

TEST(ExplainPlanTest, aggregate_cursor_and_sbe_and_shards)
{
    // Aggregate explain: plan sits in stages[0].$cursor; SBE wraps it in queryPlan.
    const auto agg = bsoncxx::from_json(R"({ "stages": [
        { "$cursor": { "queryPlanner": { "namespace": "db.c",
            "winningPlan": { "queryPlan": { "stage": "COLLSCAN" } } } } },
        { "$group": { "_id": "$x" } } ] })");
    ASSERT_TRUE(ExplainPlanParser::isExplain(agg.view()));
    const ExplainPlan aggPlan = ExplainPlanParser::parse(agg.view());
    ASSERT_TRUE(aggPlan.valid);
    EXPECT_EQ(aggPlan.ns, QStringLiteral("db.c"));
    ASSERT_EQ(aggPlan.stages.size(), 1u);
    EXPECT_EQ(aggPlan.stages[0].stage, QStringLiteral("COLLSCAN"));

    const auto sharded = bsoncxx::from_json(R"({ "queryPlanner": { "namespace": "db.c",
        "winningPlan": { "stage": "SHARD_MERGE", "shards": [
            { "shardName": "rs0", "winningPlan": { "stage": "IXSCAN", "indexName": "a_1" } },
            { "shardName": "rs1", "winningPlan": { "stage": "COLLSCAN" } } ] } } })");
    const ExplainPlan shardPlan = ExplainPlanParser::parse(sharded.view());
    ASSERT_EQ(shardPlan.stages.size(), 5u);
    EXPECT_EQ(shardPlan.stages[1].stage, QStringLiteral("SHARD"));
    EXPECT_EQ(shardPlan.stages[1].detail, QStringLiteral("rs0"));
    EXPECT_EQ(shardPlan.stages[2].depth, 2);

    EXPECT_FALSE(ExplainPlanParser::isExplain(bsoncxx::from_json(R"({ "ok": 1 })").view()));
    EXPECT_FALSE(ExplainPlanParser::parse(bsoncxx::from_json(R"({ "ok": 1 })").view()).valid);
}
