#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/utils/SchemaCatalog.h"
#include "ragoshell/core/utils/SchemaService.h"

using namespace RagoShell;
using Priority = SchemaService::Priority;

namespace
{
    const QString kConn = QStringLiteral("5e1d7c2a-8b3f-4a61-9c0e-2f4b6d8a1c33");
    const QString kOtherConn = QStringLiteral("9a7b5c3d-1e2f-4a6b-8c0d-3e5f7a9b1c2d");

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

    /// Stands in for the server: records the analyses, can hold them at a
    /// gate, and fails on demand. Shared with the worker threads.
    struct FakeServer
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool open = true;
        std::vector<QString> calls;   ///< "<collection>/<sampleSize>"
        QString error;                ///< returned instead of a result when set
        std::atomic<qint64> count{1000};
        std::atomic<int> counts{0};
        std::atomic<int> running{0};
        std::atomic<int> maxRunning{0};

        void close()
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = false;
        }
        void release()
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                open = true;
            }
            cv.notify_all();
        }
        int callCount()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return static_cast<int>(calls.size());
        }
        std::vector<QString> callLog()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return calls;
        }
    };

    SchemaResult fakeResult(int sampleSize)
    {
        SchemaResult r;
        r.ok = true;
        r.sampledDocuments = sampleSize;
        r.totalDocuments = 1000;
        r.analyzedAt = QDateTime::currentDateTimeUtc();
        SchemaField status;
        status.path = status.name = QStringLiteral("status");
        status.presentCount = sampleSize;
        status.presence = 1.0;
        status.types = {{"String", sampleSize}};
        status.topValues = {{"\"SHIPPED\"", sampleSize}};
        r.fields = {status};
        return r;
    }

    struct Fixture
    {
        QTemporaryDir dir;
        std::unique_ptr<SchemaCatalog> catalog;
        std::shared_ptr<FakeServer> server = std::make_shared<FakeServer>();
        std::unique_ptr<SchemaService> service;
        std::shared_ptr<const ConnectionSettings> connection = std::make_shared<ConnectionSettings>(false);
        std::map<quint64, SchemaJobOutcome> outcomes;
        int snapshots = 0;

        Fixture()
        {
            ensureApp();
            catalog = std::make_unique<SchemaCatalog>(dir.path());
            std::shared_ptr<FakeServer> s = server;
            auto analyze = [s](const ConnectionSettings &, const QString &, const QString &collection,
                               int sampleSize, int, UUIDEncoding, SupportedTimes) {
                const int now = ++s->running;
                int seen = s->maxRunning.load();
                while (now > seen && !s->maxRunning.compare_exchange_weak(seen, now)) {
                }
                QString error;
                {
                    std::unique_lock<std::mutex> lock(s->mutex);
                    s->calls.push_back(QStringLiteral("%1/%2").arg(collection).arg(sampleSize));
                    s->cv.wait(lock, [&s] { return s->open; });
                    error = s->error;
                }
                --s->running;
                if (!error.isEmpty()) {
                    SchemaResult failed;
                    failed.error = error;
                    return failed;
                }
                return fakeResult(sampleSize);
            };
            auto count = [s](const ConnectionSettings &, const QString &, const QString &) {
                ++s->counts;
                return s->count.load();
            };
            service = std::make_unique<SchemaService>(catalog.get(), analyze, count);
            QObject::connect(service.get(), &SchemaService::finished,
                             [this](quint64 ticket, const SchemaJobOutcome &outcome) { outcomes[ticket] = outcome; });
            QObject::connect(service.get(), &SchemaService::snapshotReady,
                             [this](const QString &, const QString &, const QString &) { ++snapshots; });
        }

        ~Fixture()
        {
            // Let held workers finish before the service goes away.
            server->release();
            waitFor([this] { return !service->isBusy(kConn) && !service->isBusy(kOtherConn); });
        }

        SchemaService::Request request(const QString &collection, Priority priority = Priority::Background,
                                       int sampleSize = 200, const QString &connectionId = kConn)
        {
            SchemaService::Request r;
            r.connectionId = connectionId;
            r.connection = connection;
            r.database = QStringLiteral("shop");
            r.collection = collection;
            r.priority = priority;
            r.sampleSize = sampleSize;
            return r;
        }

        bool finished(quint64 ticket) const { return outcomes.count(ticket) > 0; }
    };
}

TEST(SchemaServiceTest, the_same_collection_queued_twice_is_sampled_once_with_the_bigger_sample)
{
    Fixture f;
    f.server->close();
    const quint64 blocker = f.service->enqueue(f.request("customers"));
    ASSERT_TRUE(waitFor([&f] { return f.server->callCount() == 1; }));

    const quint64 a = f.service->enqueue(f.request("orders", Priority::Background, 200));
    const quint64 b = f.service->enqueue(f.request("orders", Priority::Interactive, 1000));
    EXPECT_NE(a, b);
    EXPECT_EQ(f.service->pending(kConn, "shop"), 2);   // customers running, orders once

    f.server->release();
    ASSERT_TRUE(waitFor([&] { return f.finished(blocker) && f.finished(a) && f.finished(b); }));
    EXPECT_EQ(f.server->callLog(), (std::vector<QString>{"customers/200", "orders/1000"}));
    EXPECT_EQ(f.outcomes[a].result.sampledDocuments, 1000);
    EXPECT_EQ(f.outcomes[b].result.sampledDocuments, 1000);
}

TEST(SchemaServiceTest, a_running_job_serves_a_request_it_covers)
{
    Fixture f;
    f.server->close();
    const quint64 big = f.service->enqueue(f.request("orders", Priority::Interactive, 1000));
    ASSERT_TRUE(waitFor([&f] { return f.server->callCount() == 1; }));
    const quint64 small = f.service->enqueue(f.request("orders", Priority::Background, 200));

    f.server->release();
    ASSERT_TRUE(waitFor([&] { return f.finished(big) && f.finished(small); }));
    EXPECT_EQ(f.server->callCount(), 1);
    EXPECT_TRUE(f.outcomes[small].sampled);
    EXPECT_EQ(f.outcomes[small].result.sampledDocuments, 1000);
}

TEST(SchemaServiceTest, one_job_at_a_time_per_connection_but_connections_run_side_by_side)
{
    Fixture f;
    f.server->close();
    std::vector<quint64> tickets;
    for (const char *collection : {"a", "b", "c"})
        tickets.push_back(f.service->enqueue(f.request(collection)));
    tickets.push_back(f.service->enqueue(f.request("x", Priority::Background, 200, kOtherConn)));
    ASSERT_TRUE(waitFor([&f] { return f.server->callCount() == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(f.server->callCount(), 2);   // a and x; b and c wait for a
    EXPECT_EQ(f.server->maxRunning.load(), 2);

    f.server->release();
    ASSERT_TRUE(waitFor([&] {
        for (const quint64 t : tickets)
            if (!f.finished(t))
                return false;
        return true;
    }));
    EXPECT_EQ(f.server->callCount(), 4);
}

TEST(SchemaServiceTest, interactive_jobs_jump_the_queue)
{
    Fixture f;
    f.server->close();
    const quint64 blocker = f.service->enqueue(f.request("blocker"));
    ASSERT_TRUE(waitFor([&f] { return f.server->callCount() == 1; }));
    const quint64 a = f.service->enqueue(f.request("a"));
    const quint64 b = f.service->enqueue(f.request("b", Priority::Hint));
    const quint64 c = f.service->enqueue(f.request("c", Priority::Interactive, 200));

    f.server->release();
    ASSERT_TRUE(waitFor([&] { return f.finished(blocker) && f.finished(a) && f.finished(b) && f.finished(c); }));
    EXPECT_EQ(f.server->callLog(), (std::vector<QString>{"blocker/200", "c/200", "b/200", "a/200"}));
}

TEST(SchemaServiceTest, results_go_to_the_catalog_without_values_unless_allowed)
{
    Fixture f;
    const quint64 t = f.service->enqueue(f.request("orders"));
    ASSERT_TRUE(waitFor([&] { return f.finished(t); }));
    EXPECT_EQ(f.snapshots, 1);
    const auto stored = f.catalog->find(kConn, "shop", "orders");
    ASSERT_TRUE(stored.has_value());
    EXPECT_FALSE(stored->valuesIncluded);
    ASSERT_EQ(stored->result.fields.size(), 1u);
    EXPECT_TRUE(stored->result.fields[0].topValues.empty());
    // The finished outcome itself keeps the values for the dialog that asked.
    EXPECT_FALSE(f.outcomes[t].result.fields[0].topValues.empty());
}

TEST(SchemaServiceTest, a_smaller_sample_does_not_replace_a_fresh_bigger_one)
{
    Fixture f;
    const quint64 big = f.service->enqueue(f.request("orders", Priority::Interactive, 1000));
    ASSERT_TRUE(waitFor([&] { return f.finished(big); }));
    const quint64 small = f.service->enqueue(f.request("orders", Priority::Background, 200));
    ASSERT_TRUE(waitFor([&] { return f.finished(small); }));
    EXPECT_EQ(f.catalog->find(kConn, "shop", "orders")->result.sampledDocuments, 1000);
    EXPECT_EQ(f.snapshots, 1);
}

TEST(SchemaServiceTest, a_fresh_snapshot_is_not_sampled_again)
{
    Fixture f;
    const quint64 first = f.service->enqueue(f.request("orders"));
    ASSERT_TRUE(waitFor([&] { return f.finished(first); }));
    ASSERT_EQ(f.server->callCount(), 1);

    SchemaService::Request refresh = f.request("orders");
    refresh.onlyIfStale = true;
    const quint64 unchanged = f.service->enqueue(refresh);
    ASSERT_TRUE(waitFor([&] { return f.finished(unchanged); }));
    EXPECT_FALSE(f.outcomes[unchanged].sampled);
    EXPECT_EQ(f.server->callCount(), 1);
    EXPECT_EQ(f.server->counts.load(), 1);

    f.server->count = 2000;   // grew by more than kMaxCountChange
    const quint64 grown = f.service->enqueue(refresh);
    ASSERT_TRUE(waitFor([&] { return f.finished(grown); }));
    EXPECT_TRUE(f.outcomes[grown].sampled);
    EXPECT_EQ(f.server->callCount(), 2);
}

TEST(SchemaServiceTest, cancel_finishes_every_ticket_and_stores_nothing)
{
    Fixture f;
    f.server->close();
    const quint64 running = f.service->enqueue(f.request("orders"));
    ASSERT_TRUE(waitFor([&f] { return f.server->callCount() == 1; }));
    const quint64 queued = f.service->enqueue(f.request("customers"));

    f.service->cancel(kConn);
    ASSERT_TRUE(f.finished(queued));   // at once
    EXPECT_TRUE(f.outcomes[queued].cancelled);
    EXPECT_FALSE(f.finished(running));

    f.server->release();
    ASSERT_TRUE(waitFor([&] { return f.finished(running); }));
    EXPECT_TRUE(f.outcomes[running].cancelled);
    EXPECT_FALSE(f.catalog->find(kConn, "shop", "orders").has_value());
    EXPECT_EQ(f.server->callCount(), 1);   // customers never ran
}

TEST(SchemaServiceTest, a_permission_error_stops_background_work_for_the_database)
{
    Fixture f;
    f.server->error = QStringLiteral("not authorized on shop to execute command { aggregate: \"orders\" }");
    f.server->close();
    const quint64 first = f.service->enqueue(f.request("orders"));
    ASSERT_TRUE(waitFor([&f] { return f.server->callCount() == 1; }));
    const quint64 second = f.service->enqueue(f.request("customers"));

    f.server->release();
    ASSERT_TRUE(waitFor([&] { return f.finished(first) && f.finished(second); }));
    EXPECT_EQ(f.server->callCount(), 1);   // customers dropped, no retry
    EXPECT_TRUE(f.service->isBlocked(kConn, "shop"));
    EXPECT_EQ(f.service->enqueue(f.request("invoices")), 0u);

    // A dialog the user opens still tries.
    const quint64 interactive = f.service->enqueue(f.request("invoices", Priority::Interactive));
    EXPECT_NE(interactive, 0u);
    ASSERT_TRUE(waitFor([&] { return f.finished(interactive); }));

    f.service->cancel(kConn);   // a reconnect starts over
    EXPECT_FALSE(f.service->isBlocked(kConn, "shop"));
}

TEST(SchemaServiceTest, a_failed_background_job_is_retried_once)
{
    Fixture f;
    f.server->error = QStringLiteral("operation exceeded time limit");
    const quint64 background = f.service->enqueue(f.request("orders"));
    ASSERT_TRUE(waitFor([&] { return f.finished(background); }));
    EXPECT_EQ(f.server->callCount(), 2);
    EXPECT_FALSE(f.outcomes[background].result.ok);

    const quint64 interactive = f.service->enqueue(f.request("customers", Priority::Interactive));
    ASSERT_TRUE(waitFor([&] { return f.finished(interactive); }));
    EXPECT_EQ(f.server->callCount(), 3);   // not retried: the dialog shows the error
    EXPECT_FALSE(f.service->isBlocked(kConn, "shop"));
}

TEST(SchemaServiceTest, unusable_requests_get_no_ticket)
{
    Fixture f;
    SchemaService::Request noConnection = f.request("orders");
    noConnection.connection.reset();
    EXPECT_EQ(f.service->enqueue(noConnection), 0u);
    EXPECT_EQ(f.service->enqueue(f.request("")), 0u);
    EXPECT_FALSE(f.service->isBusy(kConn));
}

TEST(SchemaServiceTest, a_paused_service_holds_background_jobs_and_still_runs_what_the_user_waits_for)
{
    Fixture f;
    f.service->setBackgroundPaused(true);
    EXPECT_TRUE(f.service->backgroundPaused());

    const quint64 background1 = f.service->enqueue(f.request("orders"));
    const quint64 background2 = f.service->enqueue(f.request("customers"));
    QCoreApplication::processEvents();
    EXPECT_EQ(f.server->callCount(), 0);                         // held

    const quint64 hint = f.service->enqueue(f.request("products", Priority::Hint));
    ASSERT_TRUE(waitFor([&] { return f.finished(hint); }));
    EXPECT_EQ(f.server->callLog(), (std::vector<QString>{"products/200"}));   // the user's job ran first and alone
    EXPECT_FALSE(f.finished(background1));
    EXPECT_FALSE(f.finished(background2));

    f.service->setBackgroundPaused(false);                       // resume: the queue starts
    ASSERT_TRUE(waitFor([&] { return f.finished(background1) && f.finished(background2); }));
    EXPECT_EQ(f.server->callCount(), 3);
}
