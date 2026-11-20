#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include <bsoncxx/json.hpp>

#include "ragoshell/core/utils/AggregatePaging.h"

namespace RagoShell
{
    namespace
    {
        // bsoncxx writes JSON with spaces around the braces; the checks below do not care.
        std::string withoutSpaces(std::string text)
        {
            text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
            return text;
        }
    }

    TEST(AggregatePagingTest, the_page_is_the_pipeline_plus_skip_and_limit_read_by_the_shells_own_parser)
    {
        const auto pipeline = bsoncxx::from_json(R"({"0": {"$match": {"cuisine": "Italian"}}, "1": {"$sort": {"_id": 1}}})");
        const auto options = bsoncxx::from_json("{}");

        const std::string script = withoutSpaces(aggregatePageScript("restaurants", pipeline.view(), options.view(), 40, 20));

        EXPECT_EQ(script,
                  "db.getCollection('restaurants').aggregate(EJSON.parse('"
                  "[{\"$match\":{\"cuisine\":\"Italian\"}},{\"$sort\":{\"_id\":{\"$numberInt\":\"1\"}}},"
                  "{\"$skip\":40},{\"$limit\":20}]'),EJSON.parse('{}'))");
    }

    TEST(AggregatePagingTest, types_that_plain_JSON_cannot_hold_are_written_as_canonical_Extended_JSON)
    {
        const auto pipeline = bsoncxx::from_json(
            R"({"0": {"$match": {"_id": {"$gt": {"$oid": "507f1f77bcf86cd799439011"}},
                                 "at": {"$lt": {"$date": "2020-01-02T03:04:05Z"}}}}})");
        const auto options = bsoncxx::from_json("{}");

        const std::string script = withoutSpaces(aggregatePageScript("c", pipeline.view(), options.view(), 0, 10));

        EXPECT_NE(script.find("{\"$oid\":\"507f1f77bcf86cd799439011\"}"), std::string::npos) << script;
        EXPECT_NE(script.find("\"$date\":{\"$numberLong\":\"1577934245000\"}"), std::string::npos) << script;
    }

    TEST(AggregatePagingTest, quotes_backslashes_and_line_breaks_cannot_break_out_of_the_script_string)
    {
        // JSON text: the value is  it's a \ back<line feed>slash
        const auto pipeline = bsoncxx::from_json(R"({"0": {"$match": {"note": "it's a \\ back\nslash"}}})");
        const auto options = bsoncxx::from_json("{}");

        const std::string script = aggregatePageScript("it's", pipeline.view(), options.view(), 0, 1);

        EXPECT_EQ(script.find('\n'), std::string::npos);                       // one line
        EXPECT_NE(script.find("db.getCollection('it\\'s')"), std::string::npos) << script;
        // to_json writes  \\  and  \n  for the backslash and the line feed; inside the JavaScript string every
        // backslash is doubled again, and the apostrophe is escaped.
        EXPECT_NE(script.find("it\\'s a \\\\\\\\ back\\\\nslash"), std::string::npos) << script;
    }

    TEST(AggregatePagingTest, a_pipeline_element_that_is_not_a_stage_ends_the_list)
    {
        const auto pipeline = bsoncxx::from_json(R"({"0": {"$match": {}}, "1": 5, "2": {"$sort": {"_id": 1}}})");
        const auto options = bsoncxx::from_json("{}");

        const std::string script = aggregatePageScript("c", pipeline.view(), options.view(), 0, 1);

        EXPECT_EQ(script.find("$sort"), std::string::npos);
    }
}
