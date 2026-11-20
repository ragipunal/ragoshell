#include "gtest/gtest.h"
#include "ragoshell/core/utils/CollectionTransfer.h"

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/types.hpp>

using namespace RagoShell;

namespace
{
    std::vector<bsoncxx::document::value> makeSampleDocs()
    {
        std::vector<bsoncxx::document::value> docs;
        docs.push_back(bsoncxx::from_json(
            R"({"_id":{"$oid":"507f1f77bcf86cd799439011"},"name":"Alice, PhD","zip":"00123","age":31,"active":true})"));
        docs.push_back(bsoncxx::from_json(
            R"({"_id":{"$oid":"507f1f77bcf86cd799439012"},"name":"Bob \"The Builder\"\nLine2","zip":"90210","age":45,"active":false,"score":98.5})"));
        return docs;
    }
}

TEST(collection_transfer_tests, json_array_and_ndjson_roundtrip)
{
    const auto docs = makeSampleDocs();

    // JSON Array round-trip
    const QString jsonArr = CollectionTransfer::documentsToJsonArray(docs);
    EXPECT_TRUE(jsonArr.trimmed().startsWith(QLatin1Char('[')));
    EXPECT_TRUE(jsonArr.trimmed().endsWith(QLatin1Char(']')));

    std::vector<bsoncxx::document::value> parsedArr;
    QString err;
    ASSERT_TRUE(CollectionTransfer::parseJsonOrNdjson(jsonArr, parsedArr, &err))
        << err.toStdString();
    ASSERT_EQ(parsedArr.size(), 2u);
    EXPECT_EQ(parsedArr[0].view()["_id"].get_oid().value.to_string(), "507f1f77bcf86cd799439011");
    EXPECT_EQ(std::string(parsedArr[0].view()["name"].get_string().value), "Alice, PhD");
    EXPECT_EQ(std::string(parsedArr[1].view()["name"].get_string().value), "Bob \"The Builder\"\nLine2");

    // NDJSON round-trip
    const QString ndjson = CollectionTransfer::documentsToNdjson(docs);
    const QStringList lines = ndjson.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    EXPECT_EQ(lines.size(), 2);

    std::vector<bsoncxx::document::value> parsedNdjson;
    ASSERT_TRUE(CollectionTransfer::parseJsonOrNdjson(ndjson, parsedNdjson, &err))
        << err.toStdString();
    ASSERT_EQ(parsedNdjson.size(), 2u);
    EXPECT_EQ(parsedNdjson[1].view()["_id"].get_oid().value.to_string(), "507f1f77bcf86cd799439012");
}

TEST(collection_transfer_tests, csv_rfc4180_and_type_inference_roundtrip)
{
    const auto docs = makeSampleDocs();
    const QString csv = CollectionTransfer::documentsToCsv(docs);

    // Header row must list _id first and include union column 'score'
    const QString firstLine = csv.section(QLatin1Char('\n'), 0, 0);
    EXPECT_EQ(firstLine, QStringLiteral("_id,name,zip,age,active,score"));

    std::vector<bsoncxx::document::value> parsedCsv;
    QString err;
    ASSERT_TRUE(CollectionTransfer::parseCsv(csv, parsedCsv, &err))
        << err.toStdString();
    ASSERT_EQ(parsedCsv.size(), 2u);

    const auto v0 = parsedCsv[0].view();
    // 24-hex _id restored as ObjectId
    EXPECT_EQ(v0["_id"].type(), bsoncxx::type::k_oid);
    EXPECT_EQ(v0["_id"].get_oid().value.to_string(), "507f1f77bcf86cd799439011");
    // Comma inside string preserved
    EXPECT_EQ(std::string(v0["name"].get_string().value), "Alice, PhD");
    // Leading-zero zip "00123" MUST stay a String, not become integer 123
    EXPECT_EQ(v0["zip"].type(), bsoncxx::type::k_string);
    EXPECT_EQ(std::string(v0["zip"].get_string().value), "00123");
    // Integer & Boolean inferred cleanly
    EXPECT_EQ(v0["age"].type(), bsoncxx::type::k_int32);
    EXPECT_EQ(v0["age"].get_int32().value, 31);
    EXPECT_EQ(v0["active"].type(), bsoncxx::type::k_bool);
    EXPECT_TRUE(v0["active"].get_bool().value);

    const auto v1 = parsedCsv[1].view();
    // Escaped quotes & embedded newline preserved per RFC 4180
    EXPECT_EQ(std::string(v1["name"].get_string().value), "Bob \"The Builder\"\nLine2");
    EXPECT_EQ(v1["score"].type(), bsoncxx::type::k_double);
    EXPECT_DOUBLE_EQ(v1["score"].get_double().value, 98.5);
}
