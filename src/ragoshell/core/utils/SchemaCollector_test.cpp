#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <mongocxx/instance.hpp>

#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/utils/SchemaCatalog.h"
#include "ragoshell/core/utils/SchemaCollector.h"
#include "ragoshell/core/utils/SchemaService.h"

using namespace RagoShell;
using Entry = SchemaAnalysis::CollectionEntry;
using Mode = SchemaCollector::Mode;

namespace
{
    const QString kConn = QStringLiteral("7c3e9a1b-2d4f-4e6a-8b0c-1d2e3f4a5b6c");

    QCoreApplication *ensureApp()
    {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char appName[] = "rago_unit_tests";
            static char *argv[] = { appName, nullptr };
            static QCoreApplication app(argc, argv);
            return &app;
        }
        return QCoreApplication::instance();
    }

    bool waitFor(const std::function<bool()> &done, int timeoutMs = 5000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!done()) {
            if (timer.elapsed() > timeoutMs)
                return false;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    Entry entry(const char *name, qint64 documents, const char *type = "collection")
    {
        Entry e;
        e.name = QString::fromLatin1(name);
        e.type = QString::fromLatin1(type);
        e.documents = documents;
        return e;
    }

    /// The server as the collector and the service see it: a collection
    /// list per database and instant analyses, both recorded.
    struct FakeServer
    {
        std::mutex mutex;
        std::vector<Entry> collections;
        QString listError;
        std::atomic<int> lists{0};
        std::vector<QString> analyzed;   ///< "<collection>/<sampleSize>"

        std::vector<QString> analyzedLog()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return analyzed;
        }
    };

    struct Fixture
    {
        QTemporaryDir dir;
        std::shared_ptr<FakeServer> server = std::make_shared<FakeServer>();
        std::unique_ptr<SchemaCatalog> catalog;
        std::unique_ptr<SchemaService> service;
        std::unique_ptr<SchemaCollector> collector;
        std::shared_ptr<const ConnectionSettings> connection = std::make_shared<ConnectionSettings>(false);
        SchemaCollector::Options options;

        Fixture()
        {
            ensureApp();
            catalog = std::make_unique<SchemaCatalog>(dir.path());
            std::shared_ptr<FakeServer> s = server;
            auto analyze = [s](const ConnectionSettings &, const QString &, const QString &collection, int sampleSize,
                               int, UUIDEncoding, SupportedTimes) {
                {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    s->analyzed.push_back(QStringLiteral("%1/%2").arg(collection).arg(sampleSize));
                }
                SchemaResult r;
                r.ok = true;
                r.sampledDocuments = sampleSize;
                r.totalDocuments = 100;
                r.analyzedAt = QDateTime::currentDateTimeUtc();
                return r;
            };
            auto count = [](const ConnectionSettings &, const QString &, const QString &) { return qint64(100); };
            service = std::make_unique<SchemaService>(catalog.get(), analyze, count);
            auto list = [s](const ConnectionSettings &, const QString &) {
                ++s->lists;
                std::lock_guard<std::mutex> lock(s->mutex);
                SchemaAnalysis::CollectionList l;
                l.ok = s->listError.isEmpty();
                l.error = s->listError;
                if (l.ok)
                    l.collections = s->collections;
                return l;
            };
            collector = std::make_unique<SchemaCollector>(service.get(), catalog.get(), list);
            options.sampleSize = 200;
            options.maxCollections = 50;
        }

        ~Fixture()
        {
            waitFor([this] { return !service->isBusy(kConn); });
        }

        bool idle()
        {
            return !service->isBusy(kConn) && collector->progress(kConn, "shop").total >= 0;
        }
    };
}

TEST(SchemaCollectorTest, candidates_skip_system_collections_views_and_fresh_snapshots)
{
    const std::vector<Entry> all{entry("orders", 50000), entry("system.profile", 10), entry("recentOrders", -1, "view"),
                                 entry("customers", 800), entry("events", -1, "timeseries"), entry("tags", 12)};
    auto fresh = [](const QString &name) { return name == QLatin1String("customers"); };
    // Smallest first, unknown sizes last; time-series collections are kept.
    EXPECT_EQ(SchemaCollector::backgroundCandidates(all, fresh, 10), (QStringList{"tags", "orders", "events"}));
    EXPECT_EQ(SchemaCollector::backgroundCandidates(all, fresh, 1), QStringList{"tags"});
    EXPECT_TRUE(SchemaCollector::backgroundCandidates(all, fresh, 0).isEmpty());
    EXPECT_EQ(SchemaCollector::backgroundCandidates(all, {}, 10).size(), 4);
}

TEST(SchemaCollectorTest, server_internal_databases_are_not_collected_in_the_background)
{
    EXPECT_FALSE(SchemaCollector::isBackgroundDatabase("local"));
    EXPECT_FALSE(SchemaCollector::isBackgroundDatabase("config"));
    EXPECT_TRUE(SchemaCollector::isBackgroundDatabase("admin"));
    EXPECT_TRUE(SchemaCollector::isBackgroundDatabase("shop"));
}

TEST(SchemaCollectorTest, a_database_in_use_is_listed_once_and_queued_up_to_the_limit)
{
    Fixture f;
    f.server->collections = {entry("orders", 5000), entry("customers", 300), entry("tags", 5), entry("audit", 90000)};
    f.options.maxCollections = 3;

    f.collector->databaseInUse(kConn, f.connection, "shop", f.options);
    ASSERT_TRUE(waitFor([&f] { return f.idle(); }));
    EXPECT_EQ(f.server->analyzedLog(), (std::vector<QString>{"tags/200", "customers/200", "orders/200"}));

    // In use again (shell, tree): not listed again, nothing more queued.
    f.collector->databaseInUse(kConn, f.connection, "shop", f.options);
    ASSERT_TRUE(waitFor([&f] { return f.idle(); }));
    EXPECT_EQ(f.server->lists.load(), 1);
    EXPECT_EQ(f.server->analyzedLog().size(), 3u);

    const SchemaCollector::Progress p = f.collector->progress(kConn, "shop");
    EXPECT_EQ(p.total, 4);
    EXPECT_EQ(p.analyzed, 3);
    EXPECT_EQ(p.pending, 0);
}

TEST(SchemaCollectorTest, off_mode_does_nothing_but_an_explicit_request_still_works)
{
    Fixture f;
    f.server->collections = {entry("orders", 5000), entry("customers", 300)};
    f.options.mode = Mode::Off;
    f.options.maxCollections = 1;

    f.collector->databaseInUse(kConn, f.connection, "shop", f.options);
    f.collector->collectionInUse(kConn, f.connection, "shop", "orders", f.options);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    QCoreApplication::processEvents();
    EXPECT_EQ(f.server->lists.load(), 0);
    EXPECT_TRUE(f.server->analyzedLog().empty());

    // "Analyze schema of all collections": no mode, no limit.
    f.collector->analyzeDatabase(kConn, f.connection, "shop", f.options);
    ASSERT_TRUE(waitFor([&f] { return f.idle() && f.server->analyzedLog().size() == 2; }));
    EXPECT_EQ(f.server->lists.load(), 1);
}

TEST(SchemaCollectorTest, an_explicit_request_lists_again_and_skips_fresh_snapshots)
{
    Fixture f;
    f.server->collections = {entry("orders", 5000)};
    f.collector->databaseInUse(kConn, f.connection, "shop", f.options);
    ASSERT_TRUE(waitFor([&f] { return f.idle() && f.server->analyzedLog().size() == 1; }));

    f.server->collections.push_back(entry("invoices", 40));   // created since
    f.collector->analyzeDatabase(kConn, f.connection, "shop", f.options);
    ASSERT_TRUE(waitFor([&f] { return f.server->lists.load() == 2 && f.idle(); }));
    ASSERT_TRUE(waitFor([&f] { return f.server->analyzedLog().size() == 2; }));
    EXPECT_EQ(f.server->analyzedLog().back(), QStringLiteral("invoices/200"));   // orders is fresh
}

TEST(SchemaCollectorTest, a_collection_in_use_is_hinted_once_per_session)
{
    Fixture f;
    f.collector->collectionInUse(kConn, f.connection, "shop", "orders", f.options);
    ASSERT_TRUE(waitFor([&f] { return f.server->analyzedLog().size() == 1 && !f.service->isBusy(kConn); }));
    f.collector->collectionInUse(kConn, f.connection, "shop", "orders", f.options);
    f.collector->collectionInUse(kConn, f.connection, "shop", "system.views", f.options);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    QCoreApplication::processEvents();
    EXPECT_FALSE(f.service->isBusy(kConn));
    EXPECT_EQ(f.server->analyzedLog().size(), 1u);

    f.collector->disconnected(kConn);   // a new session hints again (the snapshot is fresh: no sample)
    f.collector->collectionInUse(kConn, f.connection, "shop", "orders", f.options);
    EXPECT_TRUE(f.service->isBusy(kConn));
}

TEST(SchemaCollectorTest, a_listing_error_is_not_retried_and_a_disconnect_drops_a_late_listing)
{
    Fixture f;
    f.server->listError = QStringLiteral("not authorized on shop to execute command { listCollections: 1 }");
    f.collector->databaseInUse(kConn, f.connection, "shop", f.options);
    ASSERT_TRUE(waitFor([&f] { return f.collector->progress(kConn, "shop").total >= 0; }));
    f.collector->databaseInUse(kConn, f.connection, "shop", f.options);
    QCoreApplication::processEvents();
    EXPECT_EQ(f.server->lists.load(), 1);
    EXPECT_TRUE(f.server->analyzedLog().empty());

    // A listing that comes back after the disconnect queues nothing.
    f.server->listError.clear();
    f.server->collections = {entry("orders", 10)};
    f.collector->disconnected(kConn);
    f.collector->databaseInUse(kConn, f.connection, "shop", f.options);
    f.collector->disconnected(kConn);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    QCoreApplication::processEvents();
    EXPECT_TRUE(f.server->analyzedLog().empty());
    EXPECT_EQ(f.collector->progress(kConn, "shop").total, -1);
}

TEST(SchemaCollectorTest, whole_database_mode_puts_every_listed_database_in_use)
{
    Fixture f;
    f.server->collections = {entry("orders", 10)};
    f.collector->databasesListed(kConn, f.connection, {"shop", "local"}, f.options);   // OnUse: nothing
    QCoreApplication::processEvents();
    EXPECT_EQ(f.server->lists.load(), 0);

    f.options.mode = Mode::WholeDatabase;
    f.collector->databasesListed(kConn, f.connection, {"shop", "local", "config"}, f.options);
    ASSERT_TRUE(waitFor([&f] { return f.idle() && f.server->analyzedLog().size() == 1; }));
    EXPECT_EQ(f.server->lists.load(), 1);   // local and config skipped
}

// Live: dev/mongo-test MongoDB with the seeded "samples" db; skipped when absent.
TEST(SchemaCollectorTest, live_samples_database_fills_the_catalog)
{
    ensureApp();
    try {
        static mongocxx::instance instance{};
    } catch (const std::exception &) {
    }
    auto connection = std::make_shared<ConnectionSettings>(false);
    connection->setServerHost("localhost");
    connection->setServerPort(27017);
    const SchemaAnalysis::CollectionList listed = SchemaAnalysis::listCollections(*connection, "samples", 3000);
    if (!listed.ok || listed.collections.empty()) {
        std::cerr << "[ SKIP ] MongoDB with the samples db not reachable on localhost:27017\n";
        return;
    }

    QTemporaryDir dir;
    SchemaCatalog catalog(dir.path());
    SchemaService service(&catalog);
    SchemaCollector collector(&service, &catalog);
    SchemaCollector::Options options;   // OnUse, 200 documents, 50 collections
    collector.databaseInUse(kConn, connection, "samples", options);
    ASSERT_TRUE(waitFor([&] {
        const SchemaCollector::Progress p = collector.progress(kConn, "samples");
        return p.total >= 0 && p.pending == 0;
    }, 120000));

    const SchemaCollector::Progress p = collector.progress(kConn, "samples");
    EXPECT_GT(p.total, 0);
    EXPECT_EQ(p.analyzed, p.total);
    const auto restaurants = catalog.find(kConn, "samples", "restaurants");
    ASSERT_TRUE(restaurants.has_value());
    EXPECT_LE(restaurants->result.sampledDocuments, 200);
    EXPECT_FALSE(restaurants->valuesIncluded);
}
