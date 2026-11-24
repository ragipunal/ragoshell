#include "gtest/gtest.h"

#include "ragoshell/core/transport/BridgeQuota.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimeZone>

using namespace RagoShell;

// Hosted bridge free tier (docs/HostedGateway.md §9.2–§9.4): monthly transfer
// counter persisted in a local file, and the operator's account deny list.

namespace
{
    const QString kAccountA = QStringLiteral("550e8400-e29b-41d4-a716-446655440000");
    const QString kAccountB = QStringLiteral("7c9e6679-7425-40de-944b-e07fc1f90ae7");

    qint64 utc(int y, int m, int d, int h = 12)
    {
        return QDateTime(QDate(y, m, d), QTime(h, 0), QTimeZone::UTC).toSecsSinceEpoch();
    }

    void writeFile(const QString &path, const QByteArray &data)
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(data);
    }
}

TEST(BridgeQuotaTest, counts_per_account_up_to_the_limit)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    BridgeQuota q(dir.filePath(QStringLiteral("quota.json")), 1000);
    QString err;
    ASSERT_TRUE(q.load(&err)) << err.toStdString(); // missing file → empty

    const qint64 now = utc(2026, 11, 24);
    EXPECT_TRUE(q.consume(kAccountA, 600, now));
    EXPECT_TRUE(q.consume(kAccountA, 400, now)); // exactly the limit
    EXPECT_TRUE(q.exhausted(kAccountA, now));
    EXPECT_FALSE(q.consume(kAccountA, 1, now));  // over → refused, not added
    EXPECT_EQ(q.used(kAccountA, now), 1000);
    EXPECT_FALSE(q.consume(kAccountB, 1001, now));
    EXPECT_EQ(q.used(kAccountB, now), 0);        // other account unaffected
    EXPECT_TRUE(q.consume(kAccountB, 10, now));
    EXPECT_FALSE(q.consume(kAccountB, -1, now));
    // Account ids compare case-insensitively (the verifier lowercases sub).
    EXPECT_EQ(q.used(kAccountA.toUpper(), now), 1000);
}

TEST(BridgeQuotaTest, a_new_utc_month_resets_every_counter)
{
    QTemporaryDir dir;
    BridgeQuota q(dir.filePath(QStringLiteral("quota.json")), 1000);
    QString err;
    ASSERT_TRUE(q.load(&err));
    EXPECT_TRUE(q.consume(kAccountA, 1000, utc(2026, 11, 30, 23)));
    EXPECT_TRUE(q.exhausted(kAccountA, utc(2026, 11, 30, 23)));
    EXPECT_EQ(BridgeQuota::monthKey(utc(2026, 12, 1, 0)), QStringLiteral("2026-12"));
    EXPECT_FALSE(q.exhausted(kAccountA, utc(2026, 12, 1, 0)));
    EXPECT_TRUE(q.consume(kAccountA, 1000, utc(2026, 12, 1, 0)));

    // A clock stepping back into the old month does not reset anything.
    EXPECT_TRUE(q.exhausted(kAccountA, utc(2026, 11, 15)));
    EXPECT_FALSE(q.consume(kAccountA, 1, utc(2026, 11, 15)));
    EXPECT_TRUE(q.exhausted(kAccountA, utc(2026, 12, 2)));
}

TEST(BridgeQuotaTest, repeated_save_failures_mark_it_unhealthy)
{
    QTemporaryDir dir;
    // A directory where the file should be: every save fails.
    const QString path = dir.filePath(QStringLiteral("quota.json"));
    ASSERT_TRUE(QDir().mkpath(path));
    BridgeQuota q(path, 1000);
    QString err;
    for (int i = 0; i < BridgeQuota::kMaxSaveFailures; ++i) {
        EXPECT_TRUE(q.healthy());
        EXPECT_TRUE(q.consume(kAccountA, 1, utc(2026, 11, 24)));
        EXPECT_FALSE(q.save(&err));
    }
    EXPECT_FALSE(q.healthy());
}

TEST(BridgeQuotaTest, survives_a_restart_and_stores_hashed_ids_only)
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quota.json"));
    const qint64 now = utc(2026, 11, 24);
    {
        BridgeQuota q(path, 1000);
        QString err;
        ASSERT_TRUE(q.load(&err));
        EXPECT_TRUE(q.save(&err)); // nothing changed → no file needed
        EXPECT_TRUE(q.consume(kAccountA, 700, now));
        EXPECT_TRUE(q.isDirty());
        ASSERT_TRUE(q.save(&err)) << err.toStdString();
        EXPECT_FALSE(q.isDirty());
    }
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray stored = f.readAll();
    f.close();
    EXPECT_FALSE(stored.contains(kAccountA.toLatin1()));
    EXPECT_TRUE(stored.contains(BridgeQuota::accountKey(kAccountA).toLatin1()));
    EXPECT_TRUE(stored.contains("\"month\":\"2026-11\""));

    BridgeQuota again(path, 1000);
    QString err;
    ASSERT_TRUE(again.load(&err)) << err.toStdString();
    EXPECT_EQ(again.used(kAccountA, now), 700);
    EXPECT_FALSE(again.consume(kAccountA, 301, now));
    // The stored month is over: the next month starts from zero.
    EXPECT_EQ(again.used(kAccountA, utc(2027, 1, 2)), 0);
}

TEST(BridgeQuotaTest, an_unreadable_counter_file_fails_closed)
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("quota.json"));
    const QByteArray hash = BridgeQuota::accountKey(kAccountA).toLatin1();
    const QList<QByteArray> bad{
        "not json",
        "[]",
        "{\"version\":2,\"month\":\"2026-11\",\"accounts\":{}}",
        "{\"version\":1,\"month\":\"November\",\"accounts\":{}}",
        "{\"version\":1,\"month\":\"2026-11\"}",
        "{\"version\":1,\"month\":\"2026-11\",\"accounts\":{\"" + hash + "\":-5}}",
        "{\"version\":1,\"month\":\"2026-11\",\"accounts\":{\"" + hash + "\":1.5}}",
        "{\"version\":1,\"month\":\"2026-11\",\"accounts\":{\"" + hash + "\":\"10\"}}",
        "{\"version\":1,\"month\":\"2026-11\",\"accounts\":{\"" + kAccountA.toLatin1() + "\":10}}",
    };
    for (const QByteArray &data : bad) {
        writeFile(path, data);
        BridgeQuota q(path, 1000);
        QString err;
        EXPECT_FALSE(q.load(&err)) << data.constData();
        EXPECT_FALSE(err.isEmpty());
    }
    writeFile(path, "{\"version\":1,\"month\":\"2026-11\",\"accounts\":{\"" + hash + "\":10}}");
    BridgeQuota ok(path, 1000);
    QString err;
    EXPECT_TRUE(ok.load(&err)) << err.toStdString();
    EXPECT_EQ(ok.used(kAccountA, utc(2026, 11, 24)), 10);
}

TEST(BridgeQuotaTest, a_non_positive_limit_refuses_everything)
{
    BridgeQuota q(QString(), 0);
    EXPECT_EQ(q.monthlyLimit(), 1);
    EXPECT_FALSE(q.consume(kAccountA, 2, utc(2026, 11, 24)));
}

TEST(AccountDenyListTest, reads_uuids_and_reloads_on_change)
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("deny.txt"));
    AccountDenyList deny(path);
    EXPECT_TRUE(deny.enabled());
    QString err;
    EXPECT_TRUE(deny.reload(&err)); // missing file → empty list
    EXPECT_FALSE(deny.contains(kAccountA));

    writeFile(path, "# switched off 2026-11-24\n" + kAccountA.toUpper().toLatin1()
                        + "  # abuse report\n\nnot-a-uuid\n");
    err.clear();
    EXPECT_FALSE(deny.reload(&err)); // one bad line is reported…
    EXPECT_FALSE(err.isEmpty());
    EXPECT_TRUE(deny.contains(kAccountA));    // …and the good ones apply
    EXPECT_FALSE(deny.contains(kAccountB));
    EXPECT_EQ(deny.size(), 1);

    // Every reload reads the file again: a same-length rewrite is seen too.
    writeFile(path, kAccountB.toLatin1() + "\n");
    EXPECT_TRUE(deny.reload(&err));
    EXPECT_TRUE(deny.contains(kAccountB));
    EXPECT_FALSE(deny.contains(kAccountA));
    writeFile(path, kAccountA.toLatin1() + "\n");
    EXPECT_TRUE(deny.reload(&err));
    EXPECT_TRUE(deny.contains(kAccountA));
    EXPECT_FALSE(deny.contains(kAccountB));

    // A vanished file (lost mount) keeps the last list; an empty file clears it.
    QFile::remove(path);
    err.clear();
    EXPECT_FALSE(deny.reload(&err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_TRUE(deny.contains(kAccountA));
    writeFile(path, "");
    EXPECT_TRUE(deny.reload(&err));
    EXPECT_FALSE(deny.contains(kAccountA));

    AccountDenyList off;
    EXPECT_FALSE(off.enabled());
    EXPECT_TRUE(off.reload());
    EXPECT_FALSE(off.contains(kAccountA));
}
