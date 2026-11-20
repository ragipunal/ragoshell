#include <gtest/gtest.h>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

#include "ragoshell/core/domain/MongoDocument.h"
#include "ragoshell/core/mongodb/MongoClient.h"
#include "ragoshell/core/transport/InMemoryDemoTransport.h"

using namespace RagoShell;

namespace
{
    MongoQueryInfo makeQuery(const std::string &ns,
                             bsoncxx::document::view_or_value filter,
                             int limit = 0)
    {
        CollectionInfo ci;
        ci._ns = MongoNamespace(ns);
        MongoQueryInfo info;
        info._info = ci;
        info._query = bsoncxx::document::value(filter.view());
        info._limit = limit;
        return info;
    }
}

// MongoClient over InMemoryDemoTransport — the facade path the WASM build
// and gateways use (docs/TransportSeam.md §2/§7).

TEST(MongoClientTransportTest, database_names_over_demo_transport)
{
    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);
    MongoClient client(&t);
    const auto names = client.getDatabaseNames();
    EXPECT_NE(std::find(names.begin(), names.end(), "catalog"), names.end());
}

TEST(MongoClientTransportTest, collection_infos_over_demo_transport)
{
    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);
    MongoClient client(&t);
    const auto infos = client.listCollectionInfos("catalog");
    std::vector<std::string> names;
    for (const auto &i : infos)
        names.push_back(i.ns().collectionName());
    EXPECT_NE(std::find(names.begin(), names.end(), "products"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "orders"), names.end());
}

TEST(MongoClientTransportTest, query_filter_and_limit_over_demo_transport)
{
    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);
    MongoClient client(&t);

    namespace basic = bsoncxx::builder::basic;
    auto docs = client.query(makeQuery(
        "catalog.products",
        basic::make_document(basic::kvp("sku", "MS-07")).view()));
    ASSERT_EQ(docs.size(), 1u);
    EXPECT_EQ(std::string(docs[0]->bsonObj()["name"].get_string().value),
              std::string("Wireless Mouse"));

    auto limited = client.query(makeQuery(
        "catalog.products", bsoncxx::from_json("{}").view(), 1));
    EXPECT_EQ(limited.size(), 1u);
}

TEST(MongoClientTransportTest, command_errors_throw_like_native)
{
    InMemoryDemoTransport t;
    ASSERT_TRUE(t.connect(QStringLiteral("{}")).ok);
    MongoClient client(&t);
    // usersInfo is outside the demo's command surface → must throw, matching
    // the native path's runCommandChecked contract.
    EXPECT_THROW(client.getUsers("catalog"), std::runtime_error);
}
