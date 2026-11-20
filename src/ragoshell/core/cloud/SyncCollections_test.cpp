#include "gtest/gtest.h"

#include "ragoshell/core/cloud/SyncCollections.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/settings/QueryHistoryStore.h"
#include "ragoshell/core/settings/SettingsManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using namespace RagoShell;

namespace
{
    struct Fixture
    {
        QTemporaryDir dir;
        SettingsManager *settings = nullptr;
        QueryHistoryStore *history = nullptr;

        Fixture()
        {
            qputenv("RAGOSHELL_CONFIG_DIR", dir.path().toUtf8());
            settings = new SettingsManager();
            history = new QueryHistoryStore();
        }
        ~Fixture()
        {
            delete settings;
            delete history;
            qunsetenv("RAGOSHELL_CONFIG_DIR");
        }
    };

    ConnectionSettings *addConn(Fixture &f, const QString &name, const QString &uuid,
                                const QString &password = QString())
    {
        auto *c = new ConnectionSettings(false);
        QVariantMap m;
        m.insert(QStringLiteral("connectionName"), name);
        m.insert(QStringLiteral("serverHost"), QStringLiteral("localhost"));
        m.insert(QStringLiteral("serverPort"), 27017);
        m.insert(QStringLiteral("uuid"), uuid);
        if (!password.isEmpty())
        {
            QVariantMap cred;
            cred.insert(QStringLiteral("databaseName"), QStringLiteral("admin"));
            cred.insert(QStringLiteral("userName"), QStringLiteral("u"));
            cred.insert(QStringLiteral("userPassword"), password);
            m.insert(QStringLiteral("credentials"), QVariantList{cred});
        }
        c->fromVariant(m);
        f.settings->addConnection(c);
        return c;
    }

    QJsonObject payloadOf(const QByteArray &doc)
    {
        return QJsonDocument::fromJson(doc).object()
            .value(QStringLiteral("payload")).toObject();
    }
}

TEST(SyncCollectionsTest, ConnectionsRoundTripAndPasswordGate)
{
    Fixture f;
    addConn(f, QStringLiteral("Prod"), QStringLiteral("aaaaaaaa-0000-0000-0000-000000000001"),
            QStringLiteral("secret-pw"));

    QString err;
    QVariantMap state;
    const QByteArray noPw = SyncCollections::exportPayload(
        QStringLiteral("connections"), f.settings, f.history, false,
        QStringLiteral("dev-1"), QStringLiteral("Mac"), {}, &state, 1000, &err);
    ASSERT_FALSE(noPw.isEmpty()) << err.toStdString();
    const QJsonArray entries = payloadOf(noPw).value(QStringLiteral("entries")).toArray();
    ASSERT_EQ(entries.size(), 1);
    const QJsonObject data = entries.first().toObject().value(QStringLiteral("data")).toObject();
    EXPECT_EQ(data.value(QStringLiteral("uuid")).toString(),
              QLatin1String("aaaaaaaa-0000-0000-0000-000000000001"));
    const QJsonArray creds = data.value(QStringLiteral("credentials")).toArray();
    ASSERT_EQ(creds.size(), 1);
    EXPECT_FALSE(creds.first().toObject().contains(QStringLiteral("userPassword")));
    EXPECT_FALSE(creds.first().toObject().contains(QStringLiteral("userPasswordEncrypted")));

    const QByteArray withPw = SyncCollections::exportPayload(
        QStringLiteral("connections"), f.settings, f.history, true,
        QStringLiteral("dev-1"), QStringLiteral("Mac"), {}, nullptr, 1000, &err);
    const QJsonObject credPw = payloadOf(withPw)
                                   .value(QStringLiteral("entries")).toArray()
                                   .first().toObject()
                                   .value(QStringLiteral("data")).toObject()
                                   .value(QStringLiteral("credentials")).toArray()
                                   .first().toObject();
    EXPECT_EQ(credPw.value(QStringLiteral("userPassword")).toString(),
              QLatin1String("secret-pw"));

    // Unchanged export must keep the writer timestamp (stable etags).
    const QJsonObject e1 = payloadOf(noPw).value(QStringLiteral("entries")).toArray()
                               .first().toObject();
    const QByteArray again = SyncCollections::exportPayload(
        QStringLiteral("connections"), f.settings, f.history, false,
        QStringLiteral("dev-1"), QStringLiteral("Mac"), state, &state, 2000, &err);
    EXPECT_EQ(payloadOf(again).value(QStringLiteral("entries")).toArray().first().toObject()
                  .value(QStringLiteral("updated_at")).toInteger(),
              e1.value(QStringLiteral("updated_at")).toInteger());
}

TEST(SyncCollectionsTest, MergeIsUnionWithLastWriterWinsAndTombstones)
{
    Fixture f;
    addConn(f, QStringLiteral("Prod"), QStringLiteral("aaaaaaaa-0000-0000-0000-000000000001"));
    QueryHistoryEntry he;
    he.script = QStringLiteral("db.x.find()");
    he.database = QStringLiteral("d");
    he.executedAtMs = 10;
    he.favorite = true;
    he.name = QStringLiteral("s1");
    f.history->record(he);

    QVariantMap st;
    QString err;
    const QByteArray localDoc = SyncCollections::exportPayload(
        QStringLiteral("connections"), f.settings, f.history, false,
        QStringLiteral("dev-local"), QStringLiteral("Mac"), {}, &st, 1000, &err);

    // Remote adds a second connection and tombstones ours.
    QJsonObject remoteEntries;
    // simulate remote payload: same doc with extra entry + tombstone
    // Build a remote doc manually: entries = [our conn (same), remote conn newer, tombstone]
    const QJsonObject lp = QJsonDocument::fromJson(localDoc).object();
    QJsonObject rp = lp;
    QJsonArray entries = payloadOf(localDoc).value(QStringLiteral("entries")).toArray();
    QJsonObject extra;
    extra.insert(QStringLiteral("key"), QStringLiteral("bbbbbbbb-0000-0000-0000-000000000002"));
    extra.insert(QStringLiteral("updated_at"), 2000);
    extra.insert(QStringLiteral("device"), QStringLiteral("PC"));
    QJsonObject edata;
    edata.insert(QStringLiteral("uuid"), QStringLiteral("bbbbbbbb-0000-0000-0000-000000000002"));
    edata.insert(QStringLiteral("connectionName"), QStringLiteral("Staging"));
    extra.insert(QStringLiteral("data"), edata);
    entries.append(extra);
    QJsonObject tomb;
    tomb.insert(QStringLiteral("key"), QStringLiteral("cccccccc-0000-0000-0000-000000000003"));
    tomb.insert(QStringLiteral("updated_at"), 3000);
    tomb.insert(QStringLiteral("device"), QStringLiteral("PC"));
    tomb.insert(QStringLiteral("deleted"), true);
    entries.append(tomb);
    QJsonObject rpPayload = payloadOf(localDoc);
    rpPayload.insert(QStringLiteral("entries"), entries);
    rp.insert(QStringLiteral("payload"), rpPayload);
    const QByteArray remoteDoc = QJsonDocument(rp).toJson(QJsonDocument::Compact);

    auto outcome = SyncCollections::merge(QStringLiteral("connections"), remoteDoc, localDoc,
                                          f.settings, f.history,
                                          QStringLiteral("dev-local"), QStringLiteral("Mac"), 5000);
    ASSERT_TRUE(outcome.ok) << outcome.error.toStdString();
    EXPECT_TRUE(outcome.remoteNeedsUpdate);
    EXPECT_TRUE(outcome.localChanged);

    // Local now holds both live connections.
    ASSERT_EQ(f.settings->connections().size(), 2u);
    const QJsonArray merged = payloadOf(outcome.mergedPayload)
                                  .value(QStringLiteral("entries")).toArray();
    EXPECT_EQ(merged.size(), 3); // 2 live + 1 tombstone
}

TEST(SyncCollectionsTest, PreferencesAreDocLevelLastWriterWins)
{
    Fixture f;
    f.settings->setBatchSize(200);

    QString err;
    const QByteArray localDoc = SyncCollections::exportPayload(
        QStringLiteral("preferences"), f.settings, f.history, false,
        QStringLiteral("dev-local"), QStringLiteral("Mac"), {}, nullptr, 1000, &err);
    ASSERT_FALSE(localDoc.isEmpty()) << err.toStdString();
    EXPECT_EQ(payloadOf(localDoc).value(QStringLiteral("doc")).toObject()
                  .value(QStringLiteral("batchSize")).toInt(), 200);
    // Secrets/machine-local keys must never appear.
    const QString docStr = QString::fromUtf8(localDoc);
    EXPECT_FALSE(docStr.contains(QLatin1String("apiKey")));
    EXPECT_FALSE(docStr.contains(QLatin1String("anonymousID")));

    // Remote doc with newer timestamp wins and applies locally.
    QJsonObject r = QJsonDocument::fromJson(localDoc).object();
    QJsonObject rp = payloadOf(localDoc);
    QJsonObject rdoc = rp.value(QStringLiteral("doc")).toObject();
    rdoc.insert(QStringLiteral("batchSize"), 50);
    rp.insert(QStringLiteral("doc"), rdoc);
    rp.insert(QStringLiteral("doc_updated_at"), 9000);
    rp.insert(QStringLiteral("doc_device"), QStringLiteral("PC"));
    r.insert(QStringLiteral("payload"), rp);

    auto outcome = SyncCollections::merge(QStringLiteral("preferences"),
                                          QJsonDocument(r).toJson(QJsonDocument::Compact),
                                          localDoc, f.settings, f.history,
                                          QStringLiteral("dev-local"), QStringLiteral("Mac"), 9500);
    ASSERT_TRUE(outcome.ok);
    EXPECT_TRUE(outcome.localChanged);
    EXPECT_EQ(f.settings->batchSize(), 50);
    EXPECT_TRUE(outcome.conflictLabels.contains(QStringLiteral("PC")));
}

TEST(SyncCollectionsTest, HistoryClearPropagatesViaClearedAt)
{
    Fixture f;
    QueryHistoryEntry he;
    he.script = QStringLiteral("db.y.count()");
    he.database = QStringLiteral("d2");
    he.executedAtMs = 100;
    he.favorite = false;
    f.history->record(he);

    QString err;
    const QByteArray localDoc = SyncCollections::exportPayload(
        QStringLiteral("query_history"), f.settings, f.history, false,
        QStringLiteral("dev-local"), QStringLiteral("Mac"), {}, nullptr, 1000, &err);

    // Remote cleared at t=2000 → merged drops our older history entry on apply.
    QJsonObject r = QJsonDocument::fromJson(localDoc).object();
    QJsonObject rp = payloadOf(localDoc);
    rp.insert(QStringLiteral("cleared_at"), 2000);
    r.insert(QStringLiteral("payload"), rp);

    auto outcome = SyncCollections::merge(QStringLiteral("query_history"),
                                          QJsonDocument(r).toJson(QJsonDocument::Compact),
                                          localDoc, f.settings, f.history,
                                          QStringLiteral("dev-local"), QStringLiteral("Mac"), 3000);
    ASSERT_TRUE(outcome.ok);
    EXPECT_TRUE(outcome.localChanged);
    EXPECT_TRUE(f.history->entries().empty());
}

// docs/SyncClient.md §10 R4: a known tombstone keeps its original time and writer.
TEST(SyncCollectionsTest, TombstoneKeepsItsOriginalTimestamp)
{
    Fixture f;
    const qint64 day = 24LL * 3600 * 1000;
    const qint64 now = 200 * day;
    QVariantMap prev;
    prev.insert(QStringLiteral("old-tomb"), QVariantMap{{QStringLiteral("h"), QStringLiteral("t")},
                                                        {QStringLiteral("u"), now - 5 * day},
                                                        {QStringLiteral("d"), QStringLiteral("PC")}});
    prev.insert(QStringLiteral("expired-tomb"), QVariantMap{{QStringLiteral("h"), QStringLiteral("t")},
                                                            {QStringLiteral("u"), now - 100 * day},
                                                            {QStringLiteral("d"), QStringLiteral("PC")}});
    prev.insert(QStringLiteral("just-deleted"), QVariantMap{{QStringLiteral("h"), QStringLiteral("abc")},
                                                            {QStringLiteral("u"), 7},
                                                            {QStringLiteral("d"), QStringLiteral("Mac")}});
    QVariantMap state;
    QString err;
    const QByteArray doc = SyncCollections::exportPayload(
        QStringLiteral("connections"), f.settings, f.history, false,
        QStringLiteral("dev-local"), QStringLiteral("Mac"), prev, &state, now, &err);
    ASSERT_FALSE(doc.isEmpty()) << err.toStdString();

    QHash<QString, QJsonObject> byKey;
    for (const QJsonValue &v : payloadOf(doc).value(QStringLiteral("entries")).toArray())
        byKey.insert(v.toObject().value(QStringLiteral("key")).toString(), v.toObject());

    ASSERT_TRUE(byKey.contains(QStringLiteral("old-tomb")));
    EXPECT_EQ(byKey[QStringLiteral("old-tomb")].value(QStringLiteral("updated_at")).toInteger(),
              now - 5 * day);
    EXPECT_EQ(byKey[QStringLiteral("old-tomb")].value(QStringLiteral("device")).toString(),
              QStringLiteral("PC"));
    EXPECT_EQ(state.value(QStringLiteral("old-tomb")).toMap().value(QStringLiteral("u")).toLongLong(),
              now - 5 * day);
    // Past the 90-day window the tombstone is pruned instead of living forever.
    EXPECT_FALSE(byKey.contains(QStringLiteral("expired-tomb")));
    // First detection of a deletion is stamped now, by this device.
    ASSERT_TRUE(byKey.contains(QStringLiteral("just-deleted")));
    EXPECT_TRUE(byKey[QStringLiteral("just-deleted")].value(QStringLiteral("deleted")).toBool());
    EXPECT_EQ(byKey[QStringLiteral("just-deleted")].value(QStringLiteral("updated_at")).toInteger(), now);
    EXPECT_EQ(byKey[QStringLiteral("just-deleted")].value(QStringLiteral("device")).toString(),
              QStringLiteral("Mac"));
}

// docs/SyncClient.md §10 R2: an empty store on a fresh device is not a "clear".
TEST(SyncCollectionsTest, FreshDeviceDoesNotClearRemoteHistory)
{
    Fixture f;
    QString err;
    const QByteArray localDoc = SyncCollections::exportPayload(
        QStringLiteral("query_history"), f.settings, f.history, false,
        QStringLiteral("dev-new"), QStringLiteral("Laptop"), {}, nullptr, 9000, &err);
    ASSERT_FALSE(localDoc.isEmpty()) << err.toStdString();
    EXPECT_EQ(payloadOf(localDoc).value(QStringLiteral("cleared_at")).toInteger(), 0);

    const QJsonObject data{{QStringLiteral("script"), QStringLiteral("db.a.find()")},
                           {QStringLiteral("database"), QStringLiteral("d")},
                           {QStringLiteral("executedAt"), 400},
                           {QStringLiteral("ok"), true}};
    const QJsonObject entry{{QStringLiteral("key"), QStringLiteral("k1")},
                            {QStringLiteral("updated_at"), 500},
                            {QStringLiteral("device"), QStringLiteral("PC")},
                            {QStringLiteral("data"), data}};
    const QJsonObject remote{{QStringLiteral("schema_version"), 1},
                             {QStringLiteral("collection"), QStringLiteral("query_history")},
                             {QStringLiteral("device_id"), QStringLiteral("dev-pc")},
                             {QStringLiteral("saved_at"), 500},
                             {QStringLiteral("payload"),
                              QJsonObject{{QStringLiteral("entries"), QJsonArray{entry}},
                                          {QStringLiteral("cleared_at"), 0}}}};
    auto outcome = SyncCollections::merge(QStringLiteral("query_history"),
                                          QJsonDocument(remote).toJson(QJsonDocument::Compact),
                                          localDoc, f.settings, f.history,
                                          QStringLiteral("dev-new"), QStringLiteral("Laptop"), 9000);
    ASSERT_TRUE(outcome.ok) << outcome.error.toStdString();
    EXPECT_EQ(payloadOf(outcome.mergedPayload).value(QStringLiteral("cleared_at")).toInteger(), 0);
    ASSERT_EQ(f.history->entries().size(), 1u);
    EXPECT_EQ(f.history->entries().front().script, QStringLiteral("db.a.find()"));

    // A real local clear (last sync knew rows, none left) is still detected.
    QVariantMap prev;
    prev.insert(QStringLiteral("k1"), QVariantMap{{QStringLiteral("h"), QStringLiteral("x")},
                                                  {QStringLiteral("u"), 500},
                                                  {QStringLiteral("d"), QStringLiteral("PC")}});
    f.history->clearHistory();
    const QByteArray clearedDoc = SyncCollections::exportPayload(
        QStringLiteral("query_history"), f.settings, f.history, false,
        QStringLiteral("dev-new"), QStringLiteral("Laptop"), prev, nullptr, 9500, &err);
    EXPECT_EQ(payloadOf(clearedDoc).value(QStringLiteral("cleared_at")).toInteger(), 9500);
}

// docs/SyncClient.md §10 R6: a device without a saved preferences slot adopts the vault's.
TEST(SyncCollectionsTest, PreferencesFirstContactAdoptsRemote)
{
    Fixture f;
    f.settings->setBatchSize(200);
    QString err;
    const QByteArray localDoc = SyncCollections::exportPayload(
        QStringLiteral("preferences"), f.settings, f.history, false,
        QStringLiteral("dev-new"), QStringLiteral("Laptop"), {}, nullptr, 9000, &err);
    ASSERT_FALSE(localDoc.isEmpty()) << err.toStdString();
    EXPECT_EQ(payloadOf(localDoc).value(QStringLiteral("doc_updated_at")).toInteger(), 0);

    // The vault's document is older than "now" but was established by another device.
    QJsonObject r = QJsonDocument::fromJson(localDoc).object();
    QJsonObject rp = payloadOf(localDoc);
    QJsonObject rdoc = rp.value(QStringLiteral("doc")).toObject();
    rdoc.insert(QStringLiteral("batchSize"), 50);
    rp.insert(QStringLiteral("doc"), rdoc);
    rp.insert(QStringLiteral("doc_updated_at"), 500);
    rp.insert(QStringLiteral("doc_device"), QStringLiteral("PC"));
    r.insert(QStringLiteral("payload"), rp);
    const QByteArray remoteDoc = QJsonDocument(r).toJson(QJsonDocument::Compact);

    auto outcome = SyncCollections::merge(QStringLiteral("preferences"), remoteDoc, localDoc,
                                          f.settings, f.history,
                                          QStringLiteral("dev-new"), QStringLiteral("Laptop"), 9000);
    ASSERT_TRUE(outcome.ok) << outcome.error.toStdString();
    EXPECT_TRUE(outcome.localChanged);
    EXPECT_FALSE(outcome.remoteNeedsUpdate);
    EXPECT_EQ(f.settings->batchSize(), 50);

    // Once established, a local change after the remote one still wins.
    f.settings->setBatchSize(300);
    QVariantMap prev;
    prev.insert(QStringLiteral("_doc"), QVariantMap{{QStringLiteral("h"), QStringLiteral("stale")},
                                                    {QStringLiteral("u"), 100},
                                                    {QStringLiteral("d"), QStringLiteral("Laptop")}});
    const QByteArray editedDoc = SyncCollections::exportPayload(
        QStringLiteral("preferences"), f.settings, f.history, false,
        QStringLiteral("dev-new"), QStringLiteral("Laptop"), prev, nullptr, 9500, &err);
    auto second = SyncCollections::merge(QStringLiteral("preferences"), remoteDoc, editedDoc,
                                         f.settings, f.history,
                                         QStringLiteral("dev-new"), QStringLiteral("Laptop"), 9500);
    ASSERT_TRUE(second.ok);
    EXPECT_FALSE(second.localChanged);
    EXPECT_TRUE(second.remoteNeedsUpdate);
    EXPECT_EQ(f.settings->batchSize(), 300);
}

TEST(SyncCollectionsTest, ParseRejectsMalformedAndMismatchedDocs)
{
    QJsonObject p;
    EXPECT_FALSE(SyncCollections::parsePayload(QStringLiteral("connections"),
                                               QByteArrayLiteral("not json"), &p));
    EXPECT_FALSE(SyncCollections::parsePayload(QStringLiteral("connections"),
                                               QByteArrayLiteral("{}"), &p));
    QJsonObject wrong{{QStringLiteral("schema_version"), 1},
                      {QStringLiteral("collection"), QStringLiteral("snippets")},
                      {QStringLiteral("payload"), QJsonObject{{QStringLiteral("entries"),
                                                               QJsonArray{}}}}};
    EXPECT_FALSE(SyncCollections::parsePayload(QStringLiteral("connections"),
                                               QJsonDocument(wrong).toJson(QJsonDocument::Compact),
                                               &p));
}
