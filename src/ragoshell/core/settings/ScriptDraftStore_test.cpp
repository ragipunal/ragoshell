#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "ragoshell/core/settings/ScriptDraftStore.h"

using namespace RagoShell;

namespace
{
    ScriptDraft draft(const QString &id, const QString &script, qint64 savedAtMs = 1000)
    {
        ScriptDraft d;
        d.id = id;
        d.serverUuid = QStringLiteral("u1");
        d.serverName = QStringLiteral("local");
        d.database = QStringLiteral("samples");
        d.title = QStringLiteral("zips");
        d.filePath = QStringLiteral("C:/scripts/zips.js");
        d.script = script;
        d.savedAtMs = savedAtMs;
        return d;
    }
}

TEST(ScriptDraftStoreTest, update_replaces_the_draft_of_a_tab_and_puts_it_first)
{
    ScriptDraftStore store;
    EXPECT_TRUE(store.update(draft("a", "db.a.find({})")));
    EXPECT_TRUE(store.update(draft("b", "db.b.find({})")));
    ASSERT_EQ(store.entries().size(), 2u);
    EXPECT_EQ(store.entries()[0].id, QStringLiteral("b"));

    EXPECT_TRUE(store.update(draft("a", "db.a.find({ x: 1 })")));   // more typing in tab a
    ASSERT_EQ(store.entries().size(), 2u);                          // replaced, not added
    EXPECT_EQ(store.entries()[0].id, QStringLiteral("a"));
    EXPECT_EQ(store.entries()[0].script, QStringLiteral("db.a.find({ x: 1 })"));
}

TEST(ScriptDraftStoreTest, blank_oversized_or_unkeyed_scripts_are_not_kept)
{
    ScriptDraftStore store;
    store.update(draft("a", "db.a.find({})"));

    EXPECT_FALSE(store.update(draft("", "db.a.find({})")));        // no id
    EXPECT_FALSE(store.update(draft("a", "  \n\t ")));             // text deleted: the draft goes
    EXPECT_EQ(store.find("a"), nullptr);

    store.update(draft("b", "db.b.find({})"));
    const QString huge(ScriptDraftStore::kMaxScriptChars + 1, QLatin1Char('x'));
    EXPECT_FALSE(store.update(draft("b", huge)));
    EXPECT_EQ(store.find("b"), nullptr);                           // no stale older text either
    EXPECT_TRUE(store.entries().empty());
}

TEST(ScriptDraftStoreTest, keeps_only_the_newest_drafts)
{
    ScriptDraftStore store;
    for (int i = 0; i < ScriptDraftStore::kMaxDrafts + 5; ++i)
        store.update(draft(QString::number(i), QStringLiteral("db.c.find({ n: %1 })").arg(i)));

    EXPECT_EQ(static_cast<int>(store.entries().size()), ScriptDraftStore::kMaxDrafts);
    EXPECT_NE(store.find(QString::number(ScriptDraftStore::kMaxDrafts + 4)), nullptr);   // newest kept
    EXPECT_EQ(store.find("0"), nullptr);                                                 // oldest dropped
}

TEST(ScriptDraftStoreTest, remove_and_prune_by_age)
{
    ScriptDraftStore store;
    const qint64 now = 100LL * 24 * 60 * 60 * 1000;
    store.update(draft("old", "db.old.find({})", now - ScriptDraftStore::kMaxAgeMs - 1));
    store.update(draft("recent", "db.recent.find({})", now - 1000));
    store.update(draft("gone", "db.gone.find({})", now));

    EXPECT_TRUE(store.remove("gone"));
    EXPECT_FALSE(store.remove("gone"));
    EXPECT_EQ(store.pruneOlderThan(now), 1);
    ASSERT_EQ(store.entries().size(), 1u);
    EXPECT_EQ(store.entries()[0].id, QStringLiteral("recent"));
}

TEST(ScriptDraftStoreTest, json_round_trip_keeps_every_field_and_the_order)
{
    ScriptDraftStore store;
    ScriptDraft a = draft("a", "db.a.find({})\n// line two \"quoted\" \\ and unicode: çğıöşü 日本語", 2000);
    store.update(a);
    store.update(draft("b", "db.b.find({})", 3000));

    ScriptDraftStore loaded;
    ASSERT_TRUE(loaded.fromJson(store.toJson()));
    ASSERT_EQ(loaded.entries().size(), 2u);
    EXPECT_EQ(loaded.entries()[0].id, QStringLiteral("b"));         // newest first
    const ScriptDraft *back = loaded.find("a");
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->script, a.script);
    EXPECT_EQ(back->serverUuid, a.serverUuid);
    EXPECT_EQ(back->serverName, a.serverName);
    EXPECT_EQ(back->database, a.database);
    EXPECT_EQ(back->title, a.title);
    EXPECT_EQ(back->filePath, a.filePath);
    EXPECT_EQ(back->savedAtMs, a.savedAtMs);
}

TEST(ScriptDraftStoreTest, bad_json_is_refused_and_keeps_the_contents)
{
    ScriptDraftStore store;
    store.update(draft("a", "db.a.find({})"));

    QString error;
    EXPECT_FALSE(store.fromJson("{ not json", &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(store.entries().size(), 1u);
}

TEST(ScriptDraftStoreTest, a_hand_edited_file_cannot_break_the_rules)
{
    ScriptDraftStore store;
    const QByteArray json = R"json({"version":1,"drafts":[
        {"id":"","script":"no id"},
        {"id":"blank","script":"   "},
        {"id":"ok","script":"db.ok.find({})","savedAt":5}]})json";
    ASSERT_TRUE(store.fromJson(json));
    ASSERT_EQ(store.entries().size(), 1u);
    EXPECT_EQ(store.entries()[0].id, QStringLiteral("ok"));
}

TEST(ScriptDraftStoreTest, save_and_load_a_file_and_an_empty_store_removes_it)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("drafts.json"));

    ScriptDraftStore missing;
    EXPECT_TRUE(missing.load(path));                     // no file: empty, not an error
    EXPECT_TRUE(missing.entries().empty());

    ScriptDraftStore store;
    store.update(draft("a", "db.a.find({})"));
    QString error;
    ASSERT_TRUE(store.save(path, &error)) << qPrintable(error);
    EXPECT_TRUE(QFileInfo::exists(path));

    ScriptDraftStore loaded;
    ASSERT_TRUE(loaded.load(path, &error)) << qPrintable(error);
    ASSERT_EQ(loaded.entries().size(), 1u);
    EXPECT_EQ(loaded.entries()[0].script, QStringLiteral("db.a.find({})"));

    store.remove("a");
    ASSERT_TRUE(store.save(path, &error)) << qPrintable(error);
    EXPECT_FALSE(QFileInfo::exists(path));               // nothing to recover: no file left behind
}
