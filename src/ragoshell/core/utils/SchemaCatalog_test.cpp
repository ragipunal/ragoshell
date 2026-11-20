#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "ragoshell/core/utils/SchemaCatalog.h"

using namespace RagoShell;

namespace
{
    const QString kConn = QStringLiteral("0b8f6c1e-3c1a-4d55-9a3e-5f0c2a7d9e11");

    SchemaSnapshot sampleSnapshot(const QString &collection = QStringLiteral("orders"),
                                  const QDateTime &at = QDateTime::currentDateTimeUtc())
    {
        SchemaSnapshot s;
        s.database = QStringLiteral("shop");
        s.collection = collection;
        s.serverVersion = QStringLiteral("7.0.16");
        s.sampleSize = 1000;
        s.result.ok = true;
        s.result.analyzedAt = at;
        s.result.sampledDocuments = 1000;
        s.result.recentDocuments = 300;
        s.result.totalDocuments = 25359;
        s.result.indexes = {QStringLiteral(R"({ "status" : 1 })")};

        SchemaField status;
        status.path = status.name = QStringLiteral("status");
        status.presentCount = 990;
        status.presence = 0.99;
        status.types = {{"String", 990}};
        status.topValues = {{"\"SHIPPED\"", 700}, {"\"PENDING\"", 290}};
        status.distinctValues = 2;
        status.categorical = true;
        status.letterCase = QStringLiteral("upper");
        SchemaField city;
        city.path = QStringLiteral("customer.city");
        city.name = QStringLiteral("city");
        city.depth = 1;
        city.presentCount = 1000;
        city.presence = 1.0;
        city.types = {{"String", 1000}};
        city.topValues = {{"\"secret-town\"", 3}};
        city.valuesTruncated = true;
        city.onlyRecent = true;
        SchemaField total;
        total.path = total.name = QStringLiteral("total");
        total.presentCount = 1000;
        total.presence = 1.0;
        total.types = {{"Double", 1000}};
        total.minValue = QStringLiteral("1.5");
        total.maxValue = QStringLiteral("999");
        s.result.fields = {status, city, total};
        return s;
    }

    const SchemaField *field(const SchemaSnapshot &s, const char *path)
    {
        for (const auto &f : s.result.fields)
            if (f.path == QLatin1String(path))
                return &f;
        return nullptr;
    }
}

TEST(SchemaCatalogTest, json_round_trip_keeps_everything)
{
    const SchemaSnapshot in = sampleSnapshot();
    const auto out = SchemaCatalog::fromJson(SchemaCatalog::toJson(in));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->serverVersion, in.serverVersion);
    EXPECT_EQ(out->sampleSize, 1000);
    EXPECT_TRUE(out->valuesIncluded);
    EXPECT_EQ(out->result.analyzedAt.toSecsSinceEpoch(), in.result.analyzedAt.toSecsSinceEpoch());
    EXPECT_EQ(out->result.sampledDocuments, 1000);
    EXPECT_EQ(out->result.recentDocuments, 300);
    EXPECT_EQ(out->result.totalDocuments, 25359);
    EXPECT_FALSE(out->result.fullScan);
    EXPECT_EQ(out->result.indexes, in.result.indexes);
    ASSERT_EQ(out->result.fields.size(), 3u);

    const SchemaField *status = field(*out, "status");
    ASSERT_NE(status, nullptr);
    EXPECT_EQ(status->types, (std::vector<std::pair<QString, qint64>>{{"String", 990}}));
    EXPECT_EQ(status->topValues.size(), 2u);
    EXPECT_TRUE(status->categorical);
    EXPECT_EQ(status->letterCase, QStringLiteral("upper"));
    EXPECT_EQ(status->distinctValues, 2);
    const SchemaField *city = field(*out, "customer.city");
    ASSERT_NE(city, nullptr);
    EXPECT_EQ(city->name, QStringLiteral("city"));
    EXPECT_EQ(city->depth, 1);
    EXPECT_TRUE(city->valuesTruncated);
    EXPECT_TRUE(city->onlyRecent);
    const SchemaField *total = field(*out, "total");
    ASSERT_NE(total, nullptr);
    EXPECT_EQ(total->minValue, QStringLiteral("1.5"));
    EXPECT_EQ(total->maxValue, QStringLiteral("999"));
}

TEST(SchemaCatalogTest, stored_snapshot_survives_a_restart)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    {
        SchemaCatalog catalog(dir.path());
        QString error;
        ASSERT_TRUE(catalog.store(kConn, sampleSnapshot(), &error)) << error.toStdString();
    }
    EXPECT_TRUE(QFile::exists(dir.filePath(kConn + "/shop.json")));

    SchemaCatalog reopened(dir.path());
    const auto found = reopened.find(kConn, "shop", "orders");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->database, QStringLiteral("shop"));
    EXPECT_EQ(found->collection, QStringLiteral("orders"));
    EXPECT_EQ(found->result.fields.size(), 3u);
    EXPECT_FALSE(reopened.find(kConn, "shop", "customers").has_value());
    EXPECT_FALSE(reopened.find("another-connection", "shop", "orders").has_value());
}

TEST(SchemaCatalogTest, values_are_not_written_unless_allowed)
{
    QTemporaryDir dir;
    SchemaCatalog catalog(dir.path());
    SchemaSnapshot s = sampleSnapshot();
    s.valuesIncluded = false;
    ASSERT_TRUE(catalog.store(kConn, s));

    QFile file(dir.filePath(kConn + "/shop.json"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray text = file.readAll();
    EXPECT_FALSE(text.contains("secret-town"));
    EXPECT_FALSE(text.contains("SHIPPED"));
    EXPECT_FALSE(text.contains("999"));

    const auto found = SchemaCatalog(dir.path()).find(kConn, "shop", "orders");
    ASSERT_TRUE(found.has_value());
    EXPECT_FALSE(found->valuesIncluded);
    const SchemaField *status = field(*found, "status");
    ASSERT_NE(status, nullptr);
    EXPECT_TRUE(status->topValues.empty());
    EXPECT_TRUE(status->letterCase.isEmpty());
    EXPECT_TRUE(status->categorical);   // structure stays
    EXPECT_TRUE(field(*found, "total")->minValue.isEmpty());
}

TEST(SchemaCatalogTest, staleness_by_age_and_size)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    EXPECT_FALSE(SchemaCatalog::isStale(sampleSnapshot("orders", now.addDays(-1)), now));
    EXPECT_TRUE(SchemaCatalog::isStale(sampleSnapshot("orders", now.addDays(-(SchemaCatalog::kMaxAgeDays + 1))), now));
    EXPECT_TRUE(SchemaCatalog::isStale(sampleSnapshot("orders", QDateTime()), now));

    const SchemaSnapshot fresh = sampleSnapshot("orders", now.addDays(-1));   // 25,359 documents
    EXPECT_FALSE(SchemaCatalog::isStale(fresh, now, 26000));
    EXPECT_TRUE(SchemaCatalog::isStale(fresh, now, 40000));
    EXPECT_TRUE(SchemaCatalog::isStale(fresh, now, 10000));
    EXPECT_FALSE(SchemaCatalog::isStale(fresh, now, -1));   // count unknown: age only

    SchemaSnapshot small = fresh;
    small.result.totalDocuments = 10;
    EXPECT_FALSE(SchemaCatalog::isStale(small, now, 30));    // +20 documents is not a change
    EXPECT_TRUE(SchemaCatalog::isStale(small, now, 200));
}

TEST(SchemaCatalogTest, coverage_and_listing)
{
    QTemporaryDir dir;
    SchemaCatalog catalog(dir.path());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    ASSERT_TRUE(catalog.store(kConn, sampleSnapshot("orders", now)));
    ASSERT_TRUE(catalog.store(kConn, sampleSnapshot("customers", now.addDays(-30))));

    const SchemaCatalog::Coverage c = catalog.coverage(kConn, "shop", {"orders", "customers", "invoices"}, now);
    EXPECT_EQ(c.total, 3);
    EXPECT_EQ(c.analyzed, 2);
    EXPECT_EQ(c.stale, 1);
    EXPECT_EQ(catalog.analyzedCollections(kConn, "shop"), (QStringList{"customers", "orders"}));

    EXPECT_TRUE(catalog.remove(kConn, "shop", "customers"));
    EXPECT_FALSE(catalog.remove(kConn, "shop", "customers"));
    EXPECT_EQ(SchemaCatalog(dir.path()).analyzedCollections(kConn, "shop"), (QStringList{"orders"}));

    EXPECT_TRUE(catalog.clear(kConn));
    EXPECT_TRUE(catalog.analyzedCollections(kConn, "shop").isEmpty());
    EXPECT_FALSE(QDir(dir.filePath(kConn)).exists());
}

TEST(SchemaCatalogTest, odd_database_names_and_damaged_files)
{
    QTemporaryDir dir;
    SchemaCatalog catalog(dir.path());
    SchemaSnapshot s = sampleSnapshot();
    s.database = QStringLiteral("sales 2024:eu?");
    ASSERT_TRUE(catalog.store(kConn, s));
    EXPECT_TRUE(SchemaCatalog(dir.path()).find(kConn, s.database, "orders").has_value());

    // A damaged file is ignored and replaced on the next store.
    const QString path = dir.filePath(kConn + "/shop.json");
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile broken(path);
    ASSERT_TRUE(broken.open(QIODevice::WriteOnly));
    broken.write("{ not json");
    broken.close();
    SchemaCatalog reopened(dir.path());
    EXPECT_FALSE(reopened.find(kConn, "shop", "orders").has_value());
    ASSERT_TRUE(reopened.store(kConn, sampleSnapshot()));
    EXPECT_TRUE(SchemaCatalog(dir.path()).find(kConn, "shop", "orders").has_value());
}

TEST(SchemaCatalogTest, dropping_the_memory_cache_keeps_the_files_and_reads_them_again)
{
    QTemporaryDir dir;
    SchemaCatalog catalog(dir.path());
    ASSERT_TRUE(catalog.store(kConn, sampleSnapshot()));
    ASSERT_TRUE(catalog.find(kConn, "shop", "orders").has_value());

    catalog.dropMemoryCache();                                    // low memory

    const auto again = catalog.find(kConn, "shop", "orders");     // read from the file
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->result.totalDocuments, 25359);
}
