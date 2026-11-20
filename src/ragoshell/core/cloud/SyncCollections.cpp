#include "ragoshell/core/cloud/SyncCollections.h"

#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/settings/QueryHistoryStore.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/settings/SslSettings.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace RagoShell
{
namespace SyncCollections
{
namespace
{
    const QString kConnections = QStringLiteral("connections");
    const QString kSnippets = QStringLiteral("snippets");
    const QString kQueryHistory = QStringLiteral("query_history");
    const QString kPreferences = QStringLiteral("preferences");
    // startup_script is deliberately NOT synced: a remote payload would execute
    // on every device at next connect (docs/SyncClient.md threat list).

    // Tombstones kept long enough for a rarely-online device to learn the deletion.
    constexpr qint64 kTombstoneMaxAgeMs = 90LL * 24 * 3600 * 1000;
    constexpr int kMaxTombstones = 200;
    // Serialized payload safety margin under the server 1 MiB ciphertext cap.
    constexpr int kMaxPayloadDocBytes = 900 * 1024;

    QString canonical(const QJsonObject &obj)
    {
        return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }

    /// History/snippet identity independent of per-device numeric ids.
    QString historyEntryKey(const QJsonObject &data)
    {
        const QByteArray raw = (data.value(QStringLiteral("script")).toString()
                                + u'\x1f' + data.value(QStringLiteral("database")).toString()
                                + u'\x1f' + data.value(QStringLiteral("serverUuid")).toString()
                                + u'\x1f' + data.value(QStringLiteral("name")).toString())
                                   .toUtf8();
        return QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
    }

    QJsonObject entryToJson(const QString &key, qint64 updatedAt, const QString &device,
                            bool deleted, const QJsonObject &data)
    {
        QJsonObject e;
        e.insert(QStringLiteral("key"), key);
        e.insert(QStringLiteral("updated_at"), updatedAt);
        e.insert(QStringLiteral("device"), device);
        if (deleted)
            e.insert(QStringLiteral("deleted"), true);
        if (!deleted)
            e.insert(QStringLiteral("data"), data);
        return e;
    }

    QList<QJsonObject> entriesOf(const QJsonObject &payload)
    {
        QList<QJsonObject> out;
        for (const QJsonValue &v : payload.value(QStringLiteral("entries")).toArray())
            out.append(v.toObject());
        return out;
    }

    /// Union by key; newer updated_at wins; equal → lexicographically later device wins
    /// (deterministic across devices). Tombstones survive so deletions propagate.
    /// Remote timestamps are clamped to now+24h: a far-future writer would otherwise
    /// win every merge forever and dodge tombstone pruning.
    QList<QJsonObject> mergeEntryLists(const QList<QJsonObject> &local,
                                       const QList<QJsonObject> &remote,
                                       qint64 nowMs,
                                       const QString &deviceName,
                                       QStringList *conflictLabels,
                                       bool *remoteChanged)
    {
        const qint64 maxFuture = nowMs + 24LL * 3600 * 1000;
        QHash<QString, QJsonObject> byKey;
        for (const QJsonObject &e : local)
            byKey.insert(e.value(QStringLiteral("key")).toString(), e);
        for (const QJsonObject &reIn : remote)
        {
            QJsonObject re = reIn;
            const qint64 ru0 = re.value(QStringLiteral("updated_at")).toInteger();
            if (ru0 > maxFuture)
                re.insert(QStringLiteral("updated_at"), maxFuture);
            const QString key = re.value(QStringLiteral("key")).toString();
            auto it = byKey.find(key);
            if (it == byKey.end())
            {
                byKey.insert(key, re);
                if (remoteChanged) *remoteChanged = true;
                continue;
            }
            const qint64 lu = it->value(QStringLiteral("updated_at")).toInteger();
            const qint64 ru = re.value(QStringLiteral("updated_at")).toInteger();
            const QString ld = it->value(QStringLiteral("device")).toString();
            const QString rd = re.value(QStringLiteral("device")).toString();
            const bool remoteWins = ru > lu || (ru == lu && rd > ld);
            if (remoteWins)
            {
                if (ld != rd && conflictLabels && !conflictLabels->contains(rd))
                    conflictLabels->append(rd);
                *it = re;
                if (remoteChanged) *remoteChanged = true;
            }
        }
        QList<QJsonObject> out = byKey.values();
        std::sort(out.begin(), out.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a.value(QStringLiteral("key")).toString() < b.value(QStringLiteral("key")).toString();
        });
        return out;
    }

    /// Drops expired/excess tombstones deterministically (oldest first beyond the cap).
    QList<QJsonObject> pruneTombstones(QList<QJsonObject> entries, qint64 nowMs)
    {
        QList<QJsonObject> live, dead;
        for (const QJsonObject &e : entries)
            (e.value(QStringLiteral("deleted")).toBool() ? dead : live).append(e);
        dead.erase(std::remove_if(dead.begin(), dead.end(), [nowMs](const QJsonObject &e) {
                       return nowMs - e.value(QStringLiteral("updated_at")).toInteger()
                              > kTombstoneMaxAgeMs;
                   }),
                   dead.end());
        std::sort(dead.begin(), dead.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a.value(QStringLiteral("updated_at")).toInteger()
                   < b.value(QStringLiteral("updated_at")).toInteger();
        });
        while (dead.size() > kMaxTombstones)
            dead.removeFirst();
        return live + dead;
    }

    QJsonObject historyEntryToData(const QueryHistoryEntry &e)
    {
        QJsonObject o{
            {QStringLiteral("script"), e.script},
            {QStringLiteral("database"), e.database},
            {QStringLiteral("serverName"), e.serverName},
            {QStringLiteral("serverUuid"), e.serverUuid},
            {QStringLiteral("executedAt"), e.executedAtMs},
            {QStringLiteral("durationMs"), e.durationMs},
            {QStringLiteral("resultCount"), e.resultCount},
            {QStringLiteral("ok"), e.ok},
        };
        if (!e.error.isEmpty())
            o.insert(QStringLiteral("error"), e.error);
        if (e.favorite)
            o.insert(QStringLiteral("favorite"), true);
        if (!e.name.isEmpty())
            o.insert(QStringLiteral("name"), e.name);
        return o;
    }

    QJsonObject payloadRoot(const QString &collection, const QString &deviceId,
                            qint64 savedAtMs, const QJsonObject &payload)
    {
        QJsonObject doc;
        doc.insert(QStringLiteral("schema_version"), 1);
        doc.insert(QStringLiteral("collection"), collection);
        doc.insert(QStringLiteral("device_id"), deviceId);
        doc.insert(QStringLiteral("saved_at"), savedAtMs);
        doc.insert(QStringLiteral("payload"), payload);
        return doc;
    }

    /// Applies merged entry list into local stores; true when anything changed.
    bool applyKeyedEntries(const QString &collection,
                           const QList<QJsonObject> &merged,
                           qint64 clearedAtMs,
                           SettingsManager *settings,
                           QueryHistoryStore *history,
                           QString *error)
    {
        Q_UNUSED(error);
        bool changed = false;

        if (collection == kConnections)
        {
            if (!settings)
                return false;
            QHash<QString, QJsonObject> liveByKey;
            for (const QJsonObject &e : merged)
                if (!e.value(QStringLiteral("deleted")).toBool())
                    liveByKey.insert(e.value(QStringLiteral("key")).toString(),
                                     e.value(QStringLiteral("data")).toObject());

            // Snapshot: merge must not delete ConnectionSettings objects the UI
            // (ConnectionListModel, open shells) still holds. Removals leave the
            // vector via reorderConnections() and die with deleteLater().
            const auto local = settings->connections();
            std::vector<ConnectionSettings *> snapshot(local.begin(), local.end());
            QList<ConnectionSettings *> toRemove;
            QSet<QString> localKeys;
            for (ConnectionSettings *conn : snapshot)
            {
                if (!conn)
                    continue;
                const QString key = conn->uuid();
                localKeys.insert(key);
                auto it = liveByKey.constFind(key);
                if (it == liveByKey.constEnd())
                {
                    toRemove.append(conn); // tombstoned remotely
                    changed = true;
                    continue;
                }
                const QVariantMap mergedMap = it->toVariantMap();
                if (conn->toVariant().toMap() != mergedMap)
                {
                    // In-place apply keeps the object alive for holders of the pointer.
                    auto *fresh = new ConnectionSettings(false);
                    fresh->fromVariant(mergedMap);
                    conn->apply(fresh);
                    delete fresh;
                    changed = true;
                }
            }
            if (!toRemove.isEmpty())
            {
                SettingsManager::ConnectionSettingsContainerType remaining;
                for (ConnectionSettings *conn : snapshot)
                    if (conn && !toRemove.contains(conn))
                        remaining.push_back(conn);
                settings->reorderConnections(remaining);
                for (ConnectionSettings *conn : toRemove)
                    conn->deleteLater();
            }
            for (const QJsonObject &e : merged)
            {
                if (e.value(QStringLiteral("deleted")).toBool())
                    continue;
                const QString key = e.value(QStringLiteral("key")).toString();
                if (localKeys.contains(key))
                    continue;
                auto *fresh = new ConnectionSettings(false);
                fresh->fromVariant(e.value(QStringLiteral("data")).toObject().toVariantMap());
                settings->addConnection(fresh);
                changed = true;
            }
            return changed;
        }

        if (!history)
            return false;

        // Rebuild the store: keep the opposite class untouched, replace this one.
        const bool wantFavorites = (collection == kSnippets);
        QList<QueryHistoryEntry> rebuilt;
        for (const QueryHistoryEntry &e : history->entries())
        {
            if (e.favorite != wantFavorites)
                rebuilt.append(e);
            else
                changed = true; // this class is replaced wholesale below
        }
        for (const QJsonObject &e : merged)
        {
            if (e.value(QStringLiteral("deleted")).toBool())
                continue;
            const QJsonObject d = e.value(QStringLiteral("data")).toObject();
            QueryHistoryEntry he;
            he.script = d.value(QStringLiteral("script")).toString();
            he.database = d.value(QStringLiteral("database")).toString();
            he.serverName = d.value(QStringLiteral("serverName")).toString();
            he.serverUuid = d.value(QStringLiteral("serverUuid")).toString();
            he.executedAtMs = d.value(QStringLiteral("executedAt")).toInteger();
            he.durationMs = d.value(QStringLiteral("durationMs")).toInteger();
            he.resultCount = d.value(QStringLiteral("resultCount")).toInt();
            he.ok = d.value(QStringLiteral("ok")).toBool(true);
            he.error = d.value(QStringLiteral("error")).toString();
            he.favorite = d.value(QStringLiteral("favorite")).toBool();
            he.name = d.value(QStringLiteral("name")).toString();
            if (collection == kQueryHistory && clearedAtMs > 0
                && he.executedAtMs <= clearedAtMs)
                continue; // a clear propagated from another device
            rebuilt.append(he);
            changed = true;
        }
        if (!changed)
            return false;

        QJsonArray arr;
        for (const QueryHistoryEntry &e : rebuilt)
            arr.append(historyEntryToData(e));
        const QJsonObject root{{QStringLiteral("version"), 1},
                               {QStringLiteral("entries"), arr}};
        QString err;
        if (!history->fromJson(QJsonDocument(root).toJson(QJsonDocument::Compact), &err))
            return false;
        return true;
    }
} // namespace

QStringList knownCollections()
{
    return {kConnections, kSnippets, kQueryHistory, kPreferences};
}

bool isKnownCollection(const QString &name)
{
    return knownCollections().contains(name);
}

bool isKeyedCollection(const QString &name)
{
    return name == kConnections || name == kSnippets || name == kQueryHistory;
}

bool isDocCollection(const QString &name)
{
    return name == kPreferences;
}

QString entryHash(const QString &collection, const QJsonObject &data)
{
    Q_UNUSED(collection);
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical(data).toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString contentKey(const QString &collection, const QJsonObject &data)
{
    if (collection == kConnections)
        return data.value(QStringLiteral("uuid")).toString();
    return historyEntryKey(data);
}

QVariantMap exportablePreferences(SettingsManager *settings)
{
    if (!settings)
        return {};

    // Explicit allowlist — additions to convertToMap() must never leak secrets or
    // machine-local state by accident (docs/SyncClient.md §3).
    QVariantMap prefs;
    prefs.insert(QStringLiteral("uuidEncoding"), static_cast<int>(settings->uuidEncoding()));
    prefs.insert(QStringLiteral("timeZone"), static_cast<int>(settings->timeZone()));
    prefs.insert(QStringLiteral("viewMode"), static_cast<int>(settings->viewMode()));
    prefs.insert(QStringLiteral("autocompletionMode"), static_cast<int>(settings->autocompletionMode()));
    prefs.insert(QStringLiteral("autoExpand"), settings->autoExpand());
    // autoExec stays local: a remote payload must never enable command auto-execution.
    prefs.insert(QStringLiteral("minimizeToTray"), settings->minimizeToTray());
    prefs.insert(QStringLiteral("minimizeToTray"), settings->minimizeToTray());
    prefs.insert(QStringLiteral("lineNumbers"), settings->lineNumbers());
    prefs.insert(QStringLiteral("disableConnectionShortcuts"), settings->disableConnectionShortcuts());
    prefs.insert(QStringLiteral("batchSize"), settings->batchSize());
    prefs.insert(QStringLiteral("checkForUpdates"), settings->checkForUpdates());
    prefs.insert(QStringLiteral("uiTheme"), settings->uiTheme());
    prefs.insert(QStringLiteral("uiDensity"), settings->uiDensity());
    prefs.insert(QStringLiteral("uiLanguage"), settings->uiLanguage());
    prefs.insert(QStringLiteral("textFontFamily"), settings->textFontFamily());
    prefs.insert(QStringLiteral("textFontPointSize"), settings->textFontPointSize());
    prefs.insert(QStringLiteral("schemaCollectionMode"),
                 SettingsManager::schemaCollectionModeId(settings->schemaCollectionMode()));
    prefs.insert(QStringLiteral("schemaBackgroundSampleSize"), settings->schemaBackgroundSampleSize());
    prefs.insert(QStringLiteral("schemaBackgroundMaxCollections"),
                 settings->schemaBackgroundMaxCollections());
    prefs.insert(QStringLiteral("schemaUnmeteredOnly"), settings->schemaUnmeteredOnly());
    return prefs;
}

QByteArray exportPayload(const QString &collection,
                         SettingsManager *settings,
                         QueryHistoryStore *history,
                         bool includePasswords,
                         const QString &deviceId,
                         const QString &deviceName,
                         const QVariantMap &prevEntryState,
                         QVariantMap *newEntryState,
                         qint64 nowMs,
                         QString *error)
{
    if (!isKnownCollection(collection))
    {
        if (error) *error = QStringLiteral("unknown_collection");
        return {};
    }

    QJsonObject payload;

    if (isDocCollection(collection))
    {
        QJsonObject doc;
        QVariantMap prefs;
        if (collection == kPreferences)
        {
            prefs = exportablePreferences(settings);
            doc = QJsonObject::fromVariantMap(prefs);
            payload.insert(QStringLiteral("doc"), doc);
        }
        // Doc-level LWW needs a writer timestamp; persist it via entry-state map slot "_doc".
        QVariantMap prevDoc = prevEntryState.value(QStringLiteral("_doc")).toMap();
        const QString h = QString::fromLatin1(
            QCryptographicHash::hash(canonical(payload).toUtf8(), QCryptographicHash::Sha256).toHex());
        qint64 updated = prevDoc.value(QStringLiteral("u")).toLongLong();
        // No saved slot (new device, reset state) → 0 = "never established", so the
        // first contact adopts the vault's document instead of pushing local defaults
        // over every device (docs/SyncClient.md §10 R6).
        if (!prevDoc.isEmpty() && prevDoc.value(QStringLiteral("h")).toString() != h)
            updated = nowMs;
        payload.insert(QStringLiteral("doc_updated_at"), updated);
        payload.insert(QStringLiteral("doc_device"),
                       updated == nowMs ? deviceName
                                        : prevDoc.value(QStringLiteral("d")).toString());
        if (newEntryState)
        {
            QVariantMap slot;
            slot.insert(QStringLiteral("h"), h);
            slot.insert(QStringLiteral("u"), updated);
            slot.insert(QStringLiteral("d"), updated == nowMs ? deviceName
                                                              : prevDoc.value(QStringLiteral("d")).toString());
            newEntryState->insert(QStringLiteral("_doc"), slot);
        }
    }
    else
    {
        QList<QJsonObject> entries;
        QVariantMap fresh;

        if (collection == kConnections)
        {
            const auto conns = settings ? settings->connections()
                                        : SettingsManager::ConnectionSettingsContainerType{};
            for (const ConnectionSettings *conn : conns)
            {
                if (!conn)
                    continue;
                // Re-attach plaintext secrets under the legacy fields only when the
                // user opted in; device-key ciphertexts (RagoCrypt *Encrypted blobs)
                // are always stripped — they would decrypt to garbage remotely.
                QVariantMap raw = conn->toVariant().toMap();
                QVariantList creds = raw.value(QStringLiteral("credentials")).toList();
                QVariantList credsOut;
                for (const QVariant &cv : creds)
                {
                    QVariantMap cm = cv.toMap();
                    CredentialSettings probe(cm);
                    cm.remove(QStringLiteral("userPasswordEncrypted"));
                    cm.remove(QStringLiteral("userPassword"));
                    if (includePasswords)
                    {
                        const QString plain =
                            QString::fromStdString(probe.userPassword());
                        if (!plain.isEmpty())
                            cm.insert(QStringLiteral("userPassword"), plain);
                    }
                    credsOut.append(cm);
                }
                raw.insert(QStringLiteral("credentials"), credsOut);

                QVariantMap ssh = raw.value(QStringLiteral("ssh")).toMap();
                {
                    SshSettings sshProbe;
                    sshProbe.fromVariant(ssh);
                    ssh.remove(QStringLiteral("userPasswordEncrypted"));
                    ssh.remove(QStringLiteral("userPassword"));
                    ssh.remove(QStringLiteral("passphraseEncrypted"));
                    ssh.remove(QStringLiteral("passphrase"));
                    if (includePasswords)
                    {
                        const QString pw =
                            QString::fromStdString(sshProbe.userPassword());
                        const QString pp =
                            QString::fromStdString(sshProbe.passphrase());
                        if (!pw.isEmpty())
                            ssh.insert(QStringLiteral("userPassword"), pw);
                        if (!pp.isEmpty())
                            ssh.insert(QStringLiteral("passphrase"), pp);
                    }
                }
                raw.insert(QStringLiteral("ssh"), ssh);

                QVariantMap ssl = raw.value(QStringLiteral("ssl")).toMap();
                {
                    SslSettings sslProbe;
                    sslProbe.fromVariant(ssl);
                    ssl.remove(QStringLiteral("pemPassPhraseEncrypted"));
                    ssl.remove(QStringLiteral("pemPassPhrase"));
                    if (includePasswords)
                    {
                        const QString pp =
                            QString::fromStdString(sslProbe.pemPassPhrase());
                        if (!pp.isEmpty())
                            ssl.insert(QStringLiteral("pemPassPhrase"), pp);
                    }
                }
                raw.insert(QStringLiteral("ssl"), ssl);

                // Gateway pairing token — same device-bound ciphertext
                // problem as the other *Encrypted fields (8.5 review).
                raw.remove(QStringLiteral("transportTokenEncrypted"));
                raw.remove(QStringLiteral("transportToken"));
                if (includePasswords)
                {
                    const QString tt =
                        QString::fromStdString(conn->transportToken());
                    if (!tt.isEmpty())
                        raw.insert(QStringLiteral("transportToken"), tt);
                }

                const QJsonObject data = QJsonObject::fromVariantMap(raw);
                const QString key = data.value(QStringLiteral("uuid")).toString();
                if (key.isEmpty())
                    continue;
                const QString h = entryHash(collection, data);
                QVariantMap prev = prevEntryState.value(key).toMap();
                const qint64 u =
                    (prev.value(QStringLiteral("h")).toString() == h)
                        ? prev.value(QStringLiteral("u")).toLongLong()
                        : nowMs;
                const QString dev =
                    (prev.value(QStringLiteral("h")).toString() == h)
                        ? prev.value(QStringLiteral("d")).toString()
                        : deviceName;
                entries.append(entryToJson(key, u, dev, false, data));
                QVariantMap slot{{QStringLiteral("h"), h},
                                 {QStringLiteral("u"), u},
                                 {QStringLiteral("d"), dev}};
                fresh.insert(key, slot);
            }
        }
        else
        {
            // snippets (favorites only) / query_history (non-favorites)
            const bool wantFavorites = (collection == kSnippets);
            if (history)
            {
                for (const QueryHistoryEntry &e : history->entries())
                {
                    if (e.favorite != wantFavorites)
                        continue;
                    const QJsonObject data = historyEntryToData(e);
                    const QString key = contentKey(collection, data);
                    const QString h = entryHash(collection, data);
                    QVariantMap prev = prevEntryState.value(key).toMap();
                    const qint64 u =
                        (prev.value(QStringLiteral("h")).toString() == h)
                            ? prev.value(QStringLiteral("u")).toLongLong()
                            : nowMs;
                    const QString dev =
                        (prev.value(QStringLiteral("h")).toString() == h)
                            ? prev.value(QStringLiteral("d")).toString()
                            : deviceName;
                    entries.append(entryToJson(key, u, dev, false, data));
                    QVariantMap slot{{QStringLiteral("h"), h},
                                     {QStringLiteral("u"), u},
                                     {QStringLiteral("d"), dev}};
                    fresh.insert(key, slot);
                }
            }
        }

        // Deletion detection: keys known last sync but absent now become tombstones.
        for (auto it = prevEntryState.begin(); it != prevEntryState.end(); ++it)
        {
            if (it.key() == QLatin1String("_doc") || fresh.contains(it.key()))
                continue;
            if (collection == kQueryHistory)
            {
                // History entries are append-only; a local clearHistory() propagates
                // through cleared_at instead of per-entry tombstones.
                continue;
            }
            // A key that already was a tombstone keeps its original time and writer: a
            // fresh stamp on every export would beat any later edit elsewhere and never
            // age past the pruning window (docs/SyncClient.md §10 R4).
            const QVariantMap prev = it.value().toMap();
            const bool wasTombstone = prev.value(QStringLiteral("h")).toString() == QLatin1String("t");
            const qint64 deletedAt = wasTombstone ? prev.value(QStringLiteral("u")).toLongLong() : nowMs;
            const QString deletedBy = wasTombstone ? prev.value(QStringLiteral("d")).toString() : deviceName;
            entries.append(entryToJson(it.key(), deletedAt, deletedBy, true, {}));
        }
        entries = pruneTombstones(entries, nowMs);
        QJsonArray entriesArr;
        for (const QJsonObject &e : entries)
            entriesArr.append(e);
        payload.insert(QStringLiteral("entries"), entriesArr);

        if (newEntryState)
            *newEntryState = fresh;

        if (collection == kQueryHistory)
        {
            QVariantMap prevClear = prevEntryState.value(QStringLiteral("_cleared")).toMap();
            // cleared_at = last clear timestamp; remote merge takes max().
            qint64 cleared = prevClear.value(QStringLiteral("u")).toLongLong();
            // A local clear is only inferred when the last sync knew history rows and none
            // are left; an empty store alone (fresh install joining a vault) would hide every
            // device's history (docs/SyncClient.md §10 R2).
            bool hadHistory = false;
            for (auto it = prevEntryState.begin(); it != prevEntryState.end() && !hadHistory; ++it)
                hadHistory = !it.key().startsWith(QLatin1Char('_'));
            if (history && hadHistory && fresh.isEmpty())
                cleared = nowMs;
            payload.insert(QStringLiteral("cleared_at"), cleared);
            if (newEntryState)
            {
                QVariantMap slot{{QStringLiteral("u"), cleared}};
                newEntryState->insert(QStringLiteral("_cleared"), slot);
            }
        }
        // Tombstones still need carrying into persisted state so they re-export.
        if (newEntryState)
            for (const QJsonObject &e : entries)
                if (e.value(QStringLiteral("deleted")).toBool())
                {
                    QVariantMap slot;
                    slot.insert(QStringLiteral("h"), QStringLiteral("t"));
                    slot.insert(QStringLiteral("u"),
                                e.value(QStringLiteral("updated_at")).toInteger());
                    slot.insert(QStringLiteral("d"),
                                e.value(QStringLiteral("device")).toString());
                    newEntryState->insert(e.value(QStringLiteral("key")).toString(), slot);
                }
    }

    const QByteArray doc = QJsonDocument(payloadRoot(collection, deviceId, nowMs, payload))
                               .toJson(QJsonDocument::Compact);
    if (doc.size() > kMaxPayloadDocBytes)
    {
        if (error) *error = QStringLiteral("sync_payload_too_large");
        return {};
    }
    return doc;
}

bool parsePayload(const QString &collection, const QByteArray &docBytes,
                  QJsonObject *payloadOut, QString *error)
{
    auto fail = [error](const QString &code) {
        if (error) *error = code;
        return false;
    };
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(docBytes, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("payload_malformed"));
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("schema_version")).toInt() != 1)
        return fail(QStringLiteral("schema_version_unsupported"));
    if (root.value(QStringLiteral("collection")).toString() != collection)
        return fail(QStringLiteral("collection_mismatch"));
    const QJsonObject payload = root.value(QStringLiteral("payload")).toObject();
    if (payload.isEmpty())
        return fail(QStringLiteral("payload_empty"));
    if (isKeyedCollection(collection))
        for (const QJsonValue &v : payload.value(QStringLiteral("entries")).toArray())
        {
            const QJsonObject e = v.toObject();
            if (e.value(QStringLiteral("key")).toString().isEmpty()
                || !e.contains(QStringLiteral("updated_at")))
                return fail(QStringLiteral("entry_malformed"));
        }
    if (payloadOut) *payloadOut = payload;
    return true;
}

MergeOutcome merge(const QString &collection,
                   const QByteArray &remotePayloadDoc,
                   const QByteArray &localPayloadDoc,
                   SettingsManager *settings,
                   QueryHistoryStore *history,
                   const QString &deviceId,
                   const QString &deviceName,
                   qint64 nowMs)
{
    MergeOutcome out;
    QJsonObject remote, local;
    if (!parsePayload(collection, remotePayloadDoc, &remote, &out.error)
        || !parsePayload(collection, localPayloadDoc, &local, &out.error))
        return out;

    if (isDocCollection(collection))
    {
        const qint64 maxFuture = nowMs + 24LL * 3600 * 1000;
        const qint64 ru = std::min(remote.value(QStringLiteral("doc_updated_at")).toInteger(),
                                   maxFuture);
        const qint64 lu = local.value(QStringLiteral("doc_updated_at")).toInteger();
        const QString rd = remote.value(QStringLiteral("doc_device")).toString();
        const QString ld = local.value(QStringLiteral("doc_device")).toString();
        // lu == 0: this device never established its document (see exportPayload) → the
        // vault's copy wins on first contact.
        const bool remoteWins = lu <= 0 || ru > lu || (ru == lu && rd > ld);
        const QJsonObject winner = remoteWins ? remote : local;
        if (remoteWins && !rd.isEmpty() && rd != deviceName && rd != ld)
            out.conflictLabels.append(rd);
        out.localChanged = remoteWins;
        out.remoteNeedsUpdate = !remoteWins;
        if (out.localChanged)
            applyPayload(collection,
                         QJsonDocument(payloadRoot(collection, deviceId, nowMs, winner))
                             .toJson(QJsonDocument::Compact),
                         settings, history, nullptr);
        out.mergedPayload = QJsonDocument(
                                payloadRoot(collection, deviceId, nowMs, winner))
                                .toJson(QJsonDocument::Compact);
        out.ok = true;
        return out;
    }

    QStringList conflicts;
    bool remoteChanged = false;
    QList<QJsonObject> merged = mergeEntryLists(entriesOf(local), entriesOf(remote),
                                              nowMs, deviceName, &conflicts, &remoteChanged);
    merged = pruneTombstones(merged, nowMs);
    out.conflictLabels = conflicts;

    QJsonObject mergedPayload = local; // keep local cleared_at handling below
    QJsonArray mergedArr;
    for (const QJsonObject &e : merged)
        mergedArr.append(e);
    mergedPayload.insert(QStringLiteral("entries"), mergedArr);
    qint64 cleared = 0;
    if (collection == kQueryHistory)
    {
        const qint64 maxFuture = nowMs + 24LL * 3600 * 1000;
        const qint64 rc = std::min(remote.value(QStringLiteral("cleared_at")).toInteger(0),
                                   maxFuture);
        const qint64 lc = local.value(QStringLiteral("cleared_at")).toInteger(0);
        cleared = std::max(rc, lc);
        mergedPayload.insert(QStringLiteral("cleared_at"), cleared);
        if (cleared > lc)
            remoteChanged = true; // remote clear must reach the wire payload
    }

    const QByteArray mergedDoc =
        QJsonDocument(payloadRoot(collection, deviceId, nowMs, mergedPayload))
            .toJson(QJsonDocument::Compact);
    out.mergedPayload = mergedDoc;
    // Push when merged differs from remote (remote entries/tombstones folded in or vice versa).
    out.remoteNeedsUpdate = remoteChanged
                            || canonical(remote) != canonical(mergedPayload);
    out.localChanged = applyKeyedEntries(collection, merged, cleared,
                                         settings, history, nullptr);
    out.ok = true;
    return out;
}

bool applyPayload(const QString &collection,
                  const QByteArray &mergedPayloadDoc,
                  SettingsManager *settings,
                  QueryHistoryStore *history,
                  QString *error)
{
    QJsonObject payload;
    if (!parsePayload(collection, mergedPayloadDoc, &payload, error))
        return false;

    if (collection == kPreferences)
    {
        if (!settings)
            return false;
        const QVariantMap prefs = payload.value(QStringLiteral("doc")).toObject().toVariantMap();
        if (prefs.contains(QStringLiteral("uuidEncoding")))
            settings->setUuidEncoding(static_cast<UUIDEncoding>(
                prefs.value(QStringLiteral("uuidEncoding")).toInt()));
        if (prefs.contains(QStringLiteral("timeZone")))
            settings->setTimeZone(static_cast<SupportedTimes>(
                prefs.value(QStringLiteral("timeZone")).toInt()));
        if (prefs.contains(QStringLiteral("viewMode")))
            settings->setViewMode(static_cast<ViewMode>(
                prefs.value(QStringLiteral("viewMode")).toInt()));
        if (prefs.contains(QStringLiteral("autocompletionMode")))
            settings->setAutocompletionMode(static_cast<AutocompletionMode>(
                prefs.value(QStringLiteral("autocompletionMode")).toInt()));
        if (prefs.contains(QStringLiteral("autoExpand")))
            settings->setAutoExpand(prefs.value(QStringLiteral("autoExpand")).toBool());
        // autoExec is never applied from a remote payload (command auto-exec risk).
        if (prefs.contains(QStringLiteral("minimizeToTray")))
            settings->setMinimizeToTray(prefs.value(QStringLiteral("minimizeToTray")).toBool());
        if (prefs.contains(QStringLiteral("lineNumbers")))
            settings->setLineNumbers(prefs.value(QStringLiteral("lineNumbers")).toBool());
        if (prefs.contains(QStringLiteral("disableConnectionShortcuts")))
            settings->setDisableConnectionShortcuts(
                prefs.value(QStringLiteral("disableConnectionShortcuts")).toBool());
        if (prefs.contains(QStringLiteral("batchSize")))
            settings->setBatchSize(prefs.value(QStringLiteral("batchSize")).toInt());
        if (prefs.contains(QStringLiteral("checkForUpdates")))
            settings->setCheckForUpdates(prefs.value(QStringLiteral("checkForUpdates")).toBool());
        if (prefs.contains(QStringLiteral("uiTheme")))
            settings->setUiTheme(prefs.value(QStringLiteral("uiTheme")).toString());
        if (prefs.contains(QStringLiteral("uiDensity")))
            settings->setUiDensity(prefs.value(QStringLiteral("uiDensity")).toString());
        if (prefs.contains(QStringLiteral("uiLanguage")))
            settings->setUiLanguage(prefs.value(QStringLiteral("uiLanguage")).toString());
        if (prefs.contains(QStringLiteral("textFontFamily")))
            settings->setTextFontFamily(prefs.value(QStringLiteral("textFontFamily")).toString());
        if (prefs.contains(QStringLiteral("textFontPointSize")))
            settings->setTextFontPointSize(
                prefs.value(QStringLiteral("textFontPointSize")).toInt());
        if (prefs.contains(QStringLiteral("schemaCollectionMode")))
            settings->setSchemaCollectionMode(SettingsManager::schemaCollectionModeFromId(
                prefs.value(QStringLiteral("schemaCollectionMode")).toString()));
        if (prefs.contains(QStringLiteral("schemaBackgroundSampleSize")))
            settings->setSchemaBackgroundSampleSize(
                prefs.value(QStringLiteral("schemaBackgroundSampleSize")).toInt());
        if (prefs.contains(QStringLiteral("schemaBackgroundMaxCollections")))
            settings->setSchemaBackgroundMaxCollections(
                prefs.value(QStringLiteral("schemaBackgroundMaxCollections")).toInt());
        if (prefs.contains(QStringLiteral("schemaUnmeteredOnly")))
            settings->setSchemaUnmeteredOnly(
                prefs.value(QStringLiteral("schemaUnmeteredOnly")).toBool());
        return true;
    }

    if (collection == kConnections || collection == kSnippets || collection == kQueryHistory)
        return applyKeyedEntries(collection, entriesOf(payload),
                                 payload.value(QStringLiteral("cleared_at")).toInteger(0),
                                 settings, history, error);

    if (error) *error = QStringLiteral("unknown_collection");
    return false;
}

} // namespace SyncCollections
} // namespace RagoShell
