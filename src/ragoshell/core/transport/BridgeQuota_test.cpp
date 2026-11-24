#include "gtest/gtest.h"

#include "ragoshell/core/transport/BridgeQuota.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimeZone>

#include <limits>

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

TEST(BridgeQuotaTest, totals_aggregate_the_current_month_without_ids)
{
    BridgeQuota q(QString(), 1000);
    const qint64 now = utc(2026, 11, 24);
    EXPECT_TRUE(q.consume(kAccountA, 1000, now));
    EXPECT_TRUE(q.consume(kAccountB, 250, now));
    BridgeQuota::Totals t = q.totals(now);
    EXPECT_EQ(t.month, QStringLiteral("2026-11"));
    EXPECT_EQ(t.bytes, 1250);
    EXPECT_EQ(t.accounts, 2);
    EXPECT_EQ(t.exhausted, 1);
    t = q.totals(utc(2026, 12, 1));
    EXPECT_EQ(t.month, QStringLiteral("2026-12"));
    EXPECT_EQ(t.bytes, 0);
    EXPECT_EQ(t.accounts, 0);
}

TEST(BridgeQuotaTest, refusal_window_counts_the_last_hour_only)
{
    RefusalWindow w;
    const qint64 t0 = utc(2026, 11, 24, 10);
    EXPECT_EQ(w.lastHour(t0), 0);
    w.add(t0);
    w.add(t0 + 5);
    w.add(t0 + 30 * 60);
    EXPECT_EQ(w.lastHour(t0 + 30 * 60), 3);
    EXPECT_EQ(w.lastHour(t0 + 60 * 60), 1);      // the first minute has left the window
    w.add(t0 + 60 * 60);                          // same bucket index as t0, a new minute
    EXPECT_EQ(w.lastHour(t0 + 60 * 60), 2);
    EXPECT_EQ(w.lastHour(t0 + 3 * 60 * 60), 0);
}

TEST(BridgeQuotaTest, status_file_is_aggregate_key_value_and_world_readable)
{
    BridgeStatus st;
    st.writtenAt = 1795000000;
    st.month = QStringLiteral("2026-11");
    st.sessions = 3;
    st.maxSessions = 8;
    st.fullRefusals1h = 2;
    st.monthlyBytes = 123456789;
    st.monthlyLimit = 1000000000;
    st.accounts = 4;
    st.accountsExhausted = 1;
    st.quotaHealthy = false;
    const QByteArray text = formatBridgeStatus(st);
    EXPECT_EQ(text, QByteArray("version=1\nwritten_at=1795000000\nmonth=2026-11\nsessions=3\nmax_sessions=8\n"
                               "full_refusals_1h=2\nmonthly_bytes=123456789\nmonthly_limit=1000000000\n"
                               "accounts=4\naccounts_exhausted=1\nquota_healthy=0\n"));
    st.month = QStringLiteral("x\nevil=1");
    st.sessions = -5;
    const QByteArray sanitized = formatBridgeStatus(st);
    EXPECT_TRUE(sanitized.contains("month=0000-00\n"));
    EXPECT_TRUE(sanitized.contains("sessions=0\n"));
    EXPECT_FALSE(sanitized.contains("evil"));

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("status"));
    QString err;
    ASSERT_TRUE(writeBridgeStatus(path, st, &err)) << err.toStdString();
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), sanitized);
#ifndef Q_OS_WIN
    EXPECT_TRUE(f.permissions().testFlag(QFileDevice::ReadOther));
    EXPECT_FALSE(f.permissions().testFlag(QFileDevice::WriteOther));
#endif
    EXPECT_FALSE(writeBridgeStatus(dir.filePath(QStringLiteral("missing/dir/status")), st, &err));
}

TEST(BridgeQuotaTest, consume_or_exhaust_fills_remainder_and_clamps_limits)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("quota.json"));
    const qint64 now = utc(2026, 11, 24);

    BridgeQuota q(path, 100);
    QString err;
    ASSERT_TRUE(q.load(&err));
    EXPECT_TRUE(q.consumeOrExhaust(kAccountA, 85, now));
    EXPECT_FALSE(q.exhausted(kAccountA, now));
    EXPECT_FALSE(q.consumeOrExhaust(kAccountA, 20, now));
    EXPECT_EQ(q.used(kAccountA, now), 100);
    EXPECT_TRUE(q.exhausted(kAccountA, now));
    EXPECT_TRUE(q.isDirty());
    // Re-exhausting an already exhausted account still rejects without mutating.
    ASSERT_TRUE(q.save(&err)) << err.toStdString();
    EXPECT_FALSE(q.isDirty());
    EXPECT_FALSE(q.consumeOrExhaust(kAccountA, 1, now));
    EXPECT_FALSE(q.isDirty());

    // Values above kMaxSafeJsonBytes are clamped at construction and rejected in JSON.
    BridgeQuota clamped(QString(), BridgeQuota::kMaxSafeJsonBytes + 1000);
    EXPECT_EQ(clamped.monthlyLimit(), BridgeQuota::kMaxSafeJsonBytes);
    const QByteArray hash = BridgeQuota::accountKey(kAccountA).toLatin1();
    writeFile(path, "{\"version\":1,\"month\":\"2026-11\",\"accounts\":{\"" + hash + "\":"
                        + QByteArray::number(BridgeQuota::kMaxSafeJsonBytes + 1) + "}}");
    BridgeQuota overJson(path, 1000);
    EXPECT_FALSE(overJson.load(&err));

    // Non-regular files (directories) and saturating totals are handled safely.
    BridgeQuota dirPath(dir.path(), 1000);
    EXPECT_FALSE(dirPath.load(&err));
    AccountDenyList dirDeny(dir.path());
    EXPECT_FALSE(dirDeny.reload(&err));

    BridgeQuota sat(path, BridgeQuota::kMaxSafeJsonBytes);
    QJsonObject satAccounts;
    for (int i = 0; i < 1025; ++i) {
        const QString acct = QStringLiteral("acct-%1").arg(i);
        satAccounts.insert(BridgeQuota::accountKey(acct), double(BridgeQuota::kMaxSafeJsonBytes));
    }
    const QJsonObject satRoot{
        {QStringLiteral("version"), 1},
        {QStringLiteral("month"), BridgeQuota::monthKey(now)},
        {QStringLiteral("accounts"), satAccounts},
    };
    writeFile(path, QJsonDocument(satRoot).toJson(QJsonDocument::Compact));
    ASSERT_TRUE(sat.load(&err)) << err.toStdString();
    const BridgeQuota::Totals t = sat.totals(now);
    EXPECT_EQ(t.accounts, 1025);
    EXPECT_EQ(t.exhausted, 1025);
    EXPECT_EQ(t.bytes, std::numeric_limits<qint64>::max());
}
