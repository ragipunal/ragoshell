#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace RagoShell
{
    class SettingsManager;
    class QueryHistoryStore;

    /**
     * @brief Pure export/merge logic for sync collections (docs/SyncClient.md §3-§5).
     *        No network, no crypto — testable in isolation.
     *
     * Payload document (canonical JSON inside the sealed envelope):
     *   {"schema_version":1, "collection":"<name>", "device_id":"<uuid>",
     *    "saved_at":<unix ms>, "payload":{...}}
     *
     * Keyed collections (connections/snippets/query_history):
     *   payload.entries[] = {key, updated_at, device, deleted, data}
     * Doc collections (preferences):
     *   payload.doc = <object|string>, payload.doc_updated_at, payload.doc_device
     * query_history additionally carries payload.cleared_at (ms) for clear propagation.
     */
    namespace SyncCollections
    {
        QStringList knownCollections();
        bool isKnownCollection(const QString &name);
        bool isKeyedCollection(const QString &name);
        bool isDocCollection(const QString &name);

        /// Per-key fingerprint of an entry's data (stable across runs/devices).
        QString entryHash(const QString &collection, const QJsonObject &data);
        /// Stable dedup key for history/snippet rows (local ids collide across devices).
        QString contentKey(const QString &collection, const QJsonObject &data);

        /// Serializable preferences subset — strips keys that must never leave the device.
        QVariantMap exportablePreferences(SettingsManager *settings);

        /**
         * @brief Builds the canonical payload document for one collection from local state.
         * @param prevEntryState persisted per-key {hash, updated_at} map so unchanged
         *        entries keep their writer timestamps (collection -> key -> {h, u}).
         * @param newEntryState out: refreshed map to persist after a successful sync.
         */
        QByteArray exportPayload(const QString &collection,
                                 SettingsManager *settings,
                                 QueryHistoryStore *history,
                                 bool includePasswords,
                                 const QString &deviceId,
                                 const QString &deviceName,
                                 const QVariantMap &prevEntryState,
                                 QVariantMap *newEntryState,
                                 qint64 nowMs,
                                 QString *error = nullptr);

        /// Parses + validates a payload document; false on malformed/mismatched input.
        bool parsePayload(const QString &collection, const QByteArray &doc,
                          QJsonObject *payloadOut, QString *error = nullptr);

        struct MergeOutcome
        {
            bool ok = false;
            QString error;
            bool localChanged = false;      ///< merged state differs from local → caller must save
            bool remoteNeedsUpdate = false; ///< merged doc differs from remote → PUT required
            QByteArray mergedPayload;       ///< canonical doc to PUT when remoteNeedsUpdate
            QStringList conflictLabels;   ///< devices that lost per-entry/doc conflicts
        };

        /// Last-writer-wins merge (union by key + device label) per docs/SyncClient.md §5.
        MergeOutcome merge(const QString &collection,
                           const QByteArray &remotePayload,
                           const QByteArray &localPayload,
                           SettingsManager *settings,
                           QueryHistoryStore *history,
                           const QString &deviceId,
                           const QString &deviceName,
                           qint64 nowMs);

        /// Applies an already-merged payload document to local stores.
        /// Returns false on malformed data; local state left untouched on failure.
        bool applyPayload(const QString &collection,
                          const QByteArray &mergedPayload,
                          SettingsManager *settings,
                          QueryHistoryStore *history,
                          QString *error = nullptr);
    }
}
