#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "ragoshell/core/transport/InMemoryDemoTransport.h"

using namespace RagoShell;

namespace
{
    IMongoTransport::Result cmd(InMemoryDemoTransport &t, const QString &db,
                                const QString &ejson)
    {
        return t.runCommand(db, ejson);
    }

    QJsonArray firstBatch(const IMongoTransport::Result &r)
    {
        return QJsonDocument::fromJson(r.ejson.toUtf8()).object()
            .value("cursor").toObject().value("firstBatch").toArray();
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
    auto r = cmd(t, "catalog", R"({"find":"nonexistent"})");
    EXPECT_FALSE(r.ok);
    EXPECT_GT(r.code, 0);
    auto r2 = cmd(t, "catalog", R"({"unknownCmd":1})");
    EXPECT_FALSE(r2.ok);
    auto r3 = cmd(t, "otherdb", R"({"ping":1})");
    EXPECT_FALSE(r3.ok);
    InMemoryDemoTransport t2;
    auto r4 = t2.runCommand(QStringLiteral("catalog"), QStringLiteral(R"({"ping":1})"));
    EXPECT_FALSE(r4.ok);   // not connected
}
