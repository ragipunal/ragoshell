#pragma once

#include "ragoshell/core/cloud/CloudClient.h"
#include "ragoshell/core/cloud/SyncCrypto.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

class QNetworkReply;

namespace RagoShell
{
    class SettingsManager;
    class QueryHistoryStore;

    /**
     * @brief Zero-knowledge cloud sync engine (docs/SyncClient.md). Sequentially pulls/pushes
     *        per-collection AES-256-GCM envelopes through CloudClient's authenticated access
     *        token; optimistic concurrency via the server's If-Match / If-None-Match contract.
     *        Never syncs on its own — syncNow() is user-initiated only (Roadmap §3.4).
     */
    class SyncEngine : public QObject
    {
        Q_OBJECT

    public:
        enum class State { Disabled, Idle, Syncing, Blocked, Error };
        Q_ENUM(State)

        /// Persisted per-collection wire state (etag + entry fingerprints) in sync-state.json.
        struct CollectionState
        {
            QString etag;
            QVariantMap entries;   ///< key -> {h: content-hash, u: updated_at ms, d: device}
            QVariantMap cleared;   ///< query_history: {u: cleared_at ms}
            QVariantMap doc;       ///< doc collections: {h, u, d}
        };

        explicit SyncEngine(CloudClient *cloud,
                            SettingsManager *settings,
                            QueryHistoryStore *history,
                            std::shared_ptr<ICloudCredentialStore> credentialStore,
                            QObject *parent = nullptr);

        State state() const noexcept { return m_state; }
        bool syncEnabled() const;
        QStringList enabledCollections() const;
        bool passwordsIncluded() const;
        QString keyMode() const;       ///< "" | "passphrase" | "device"
        bool hasConfiguredKey() const;
        qint64 lastSyncAtMs() const;
        QString lastError() const { return m_lastError; }
        QVariantMap collectionStatus() const { return m_collectionStatus; }
        QStringList lastConflictLabels() const { return m_conflictLabels; }

        /// true when a usable master key exists (KeyStore slot) — QML wizard gating.
        bool keyAvailable();

        /// Passphrase mode: derives master key via Argon2id, stores in KeyStore, enables sync.
        bool configurePassphrase(const QString &passphrase, QString *error = nullptr);

        /// Device-key mode: random 32-byte key in KeyStore (other devices cannot read vaults).
        bool configureDeviceKey(QString *error = nullptr);

        /// Attach to an existing remote vault: pulls any blob, derives the key from its
        /// kdf_params + passphrase, verifies by decrypting, then stores the key.
        bool attachToRemoteVault(const QString &passphrase, QString *error = nullptr);

        void setCollections(const QStringList &collections, bool includePasswords);
        void disable();     ///< clears stored key + config; remote vault untouched
        void syncNow();     ///< explicit user action
        void wipeRemote();  ///< DELETE /v1/sync (all collections), keeps local config

    public Q_SLOTS:
        /// Clears local sync key, sync-state.json ETags/fingerprints, and timers when switching cloud servers.
        void resetLocalSyncStateForServerSwitch();

    Q_SIGNALS:
        void stateChanged();
        void syncStarted();
        void syncFinished(bool ok);
        void collectionSynced(const QString &collection);
        void conflictsResolved(const QStringList &losingDevices);
        void wipeFinished(bool ok);
        /// A merge rewrote the QueryHistoryStore (snippets/query_history). Emitted
        /// synchronously: receivers rebuild their views and save the store before the run
        /// records the pulled keys in sync-state.json (docs/SyncClient.md §10 R1).
        void historyChanged();

    private:
        void setState(State s);
        void setCollectionStatus(const QString &collection, const QString &status,
                                 const QString &message = QString());
        QByteArray masterKey(QString *error = nullptr);
        QString deviceLabel() const;
        qint64 nowMs() const;

        void loadSyncState();
        bool saveSyncState();

        QNetworkRequest buildRequest(const QString &path, const QString &accessToken) const;
        void startManifestFetch();
        void advanceCollection();
        void handleManifest(int status, const QByteArray &body);
        void handleCollectionGet(const QString &collection, int status,
                                 const QString &etagHeader, const QByteArray &body);
        void putCollection(const QString &collection, const QByteArray &payloadDoc,
                           const QString &ifMatchEtag, bool createOnly, bool retried);
        void handlePutResult(const QString &collection, int status, const QString &etagHeader,
                             const QByteArray &body, bool createOnly, bool retried,
                             const QByteArray &payloadDoc);
        /// Records the remote etag + fresh local fingerprints once a collection is in sync.
        void commitCollectionState(const QString &collection, const QString &etag);
        void finishRun(bool ok, const QString &error = QString());
        void markBlocked(const QString &reason);
        /// Clears the modal passphrase and settles the attach chain.
        void finishAttach(const QString &error, bool ok);

        /// Transport-level failure (no HTTP status): mark offline and schedule
        /// an exponential-backoff retry while sync stays enabled.
        void finishOffline(const QString &what);
        void scheduleOfflineRetry();

        CloudClient *m_cloud;
        SettingsManager *m_settings;
        QueryHistoryStore *m_history;
        std::shared_ptr<ICloudCredentialStore> m_creds;
        QNetworkAccessManager m_nam;

        State m_state = State::Disabled;
        QString m_lastError;
        QVariantMap m_collectionStatus;
        QStringList m_conflictLabels;

        // persisted sync-state.json fields
        QHash<QString, CollectionState> m_states;
        qint64 m_lastSyncAtMs = 0;

        // in-flight run state
        QStringList m_runQueue;
        int m_runIndex = -1;
        QString m_accessToken;
        bool m_writable = false;
        int m_maxCollections = 0;
        int m_maxBlobBytes = 0;
        bool m_wipeInFlight = false;
        int m_retried412 = 0;
        /// Bumped whenever a run ends; reply handlers of an older run return early.
        quint64 m_runGeneration = 0;

        // offline tolerance: transport-failure backoff
        QTimer m_retryTimer;
        int m_retryBackoffMs = 0;
        bool m_offline = false;

        QVariantMap m_kdfParams;        ///< vault-wide argon2id params (m/t/p/salt-b64) persisted in sync-state
        QByteArray m_lastSentEnvelope;  ///< envelope JSON sent in the last PUT

        // attachToRemoteVault modal chain state
        bool m_attachDone = false;
        bool m_attachOk = false;
        QString m_attachError;
        QString m_attachPassphrase;
        QPointer<QNetworkReply> m_wipeReply;
        QPointer<QNetworkReply> m_attachReply;
    };
}
