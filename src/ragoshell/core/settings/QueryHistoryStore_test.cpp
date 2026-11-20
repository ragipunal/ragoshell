#include <gtest/gtest.h>

#include <QTemporaryDir>

#include "ragoshell/core/settings/QueryHistoryStore.h"

using namespace RagoShell;

namespace
{
    QueryHistoryEntry run(const QString &script, const QString &db = QStringLiteral("samples"),
                          const QString &server = QStringLiteral("u1"))
    {
        QueryHistoryEntry e;
        e.script = script;
        e.database = db;
        e.serverUuid = server;
        e.serverName = QStringLiteral("local");
        e.executedAtMs = 1000;
        e.resultCount = 3;
        return e;
    }
}

TEST(QueryHistoryStoreTest, newest_first_and_dedupes_same_script)
{
    QueryHistoryStore store;
    const qint64 a = store.record(run("db.a.find({})"));
    store.record(run("db.b.find({})"));
    ASSERT_EQ(store.entries().size(), 2u);
    EXPECT_EQ(store.entries()[0].script, QStringLiteral("db.b.find({})"));

    store.setFavorite(a, true);
    store.rename(a, "all of a");
    QueryHistoryEntry again = run("db.a.find({})");
    again.resultCount = 7;
    EXPECT_EQ(store.record(again), a);           // same id, moved to the top
    ASSERT_EQ(store.entries().size(), 2u);
    EXPECT_EQ(store.entries()[0].id, a);
    EXPECT_EQ(store.entries()[0].resultCount, 7);
    EXPECT_TRUE(store.entries()[0].favorite);    // snippet data survives
    EXPECT_EQ(store.entries()[0].name, QStringLiteral("all of a"));

    // Different database or server is a different entry.
    store.record(run("db.a.find({})", "other"));
    store.record(run("db.a.find({})", "samples", "u2"));
    EXPECT_EQ(store.entries().size(), 4u);

    EXPECT_EQ(store.record(run("   ")), 0);
    EXPECT_EQ(store.record(run(QString(QueryHistoryStore::kMaxScriptChars + 1, 'x'))), 0);
}

TEST(QueryHistoryStoreTest, trims_plain_entries_but_keeps_favorites)
{
    QueryHistoryStore store;
    const qint64 fav = store.record(run("db.keep.find({})"));
    store.setFavorite(fav, true);
    for (int i = 0; i < QueryHistoryStore::kMaxEntries + 20; ++i)
        store.record(run(QStringLiteral("db.c.find({n: %1})").arg(i)));

    EXPECT_EQ(store.entries().size(), static_cast<size_t>(QueryHistoryStore::kMaxEntries + 1));
    EXPECT_NE(store.find(fav), nullptr);

    store.clearHistory();
    ASSERT_EQ(store.entries().size(), 1u);
    EXPECT_EQ(store.entries()[0].id, fav);
    EXPECT_TRUE(store.remove(fav));
    EXPECT_TRUE(store.entries().empty());
}

TEST(QueryHistoryStoreTest, save_and_load_round_trip)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("query_history.json");

    QueryHistoryStore store;
    QueryHistoryEntry failed = run("db.x.bad(");
    failed.ok = false;
    failed.error = "SyntaxError";
    store.record(failed);
    const qint64 snippet = store.record(run("db.zips.find({state: 'NY'})"));
    store.setFavorite(snippet, true);
    store.rename(snippet, "NY zips");
    ASSERT_TRUE(store.save(path));

    QueryHistoryStore loaded;
    ASSERT_TRUE(loaded.load(path));
    ASSERT_EQ(loaded.entries().size(), 2u);
    EXPECT_EQ(loaded.entries()[0].name, QStringLiteral("NY zips"));
    EXPECT_TRUE(loaded.entries()[0].favorite);
    EXPECT_FALSE(loaded.entries()[1].ok);
    EXPECT_EQ(loaded.entries()[1].error, QStringLiteral("SyntaxError"));

    // Missing file = empty history; corrupt file keeps current contents.
    QueryHistoryStore empty;
    EXPECT_TRUE(empty.load(dir.filePath("missing.json")));
    EXPECT_TRUE(empty.entries().empty());
    QString error;
    EXPECT_FALSE(loaded.fromJson("{not json", &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(loaded.entries().size(), 2u);
}
