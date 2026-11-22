#pragma once

#include <QCoreApplication>
#include <QObject>
#include <QPointer>
#include <QVariantMap>
#include <QVariantList>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QUrl>
#include <atomic>
#include <memory>
#include <thread>

#include "ragoshell/core/utils/SchemaCollector.h"
#include "ragoshell-app/ServerTreeModel.h"
#include "ragoshell-app/LogModel.h"
#include "ragoshell-app/QmlObjectListModel.h"
#include "ragoshell-app/TabModel.h"

namespace RagoShell
{
    class MongoServer;
    struct ScriptDraft;
    class BackgroundMonitor;
    class ConnectionSettings;
    class MongoDatabase;
    class App;
    class EventBus;
    class QmlBackend;

    class ConnectingEvent;
    struct ConnectionEstablishedEvent;
    class ConnectionFailedEvent;
    class DatabaseListLoadedEvent;
    class MongoServerLoadingDatabasesEvent;
    struct ReplicaSetFolderLoading;
    struct ReplicaSetFolderRefreshed;
    class MongoDatabaseCollectionListLoadedEvent;
    class MongoDatabaseCollectionsLoadingEvent;
    class MongoDatabaseFunctionsLoadedEvent;
    class MongoDatabaseFunctionsLoadingEvent;
    class MongoDatabaseUsersLoadedEvent;
    class MongoDatabaseUsersLoadingEvent;
    class LoadCollectionIndexesResponse;
    struct AddEditIndexResponse;
    class DropCollectionIndexResponse;
    class InsertDocumentResponse;
    class OpeningShellEvent;
    class OperationFailedEvent;
    class LogEvent;

    /**
     * @brief Lifetime-safe EventBus reply target for per-collection index requests
     *        dispatched to MongoWorker. Owned by QmlBackend (never tied to transient
     *        tree Node lifetimes) and self-deletes via deleteLater() once completed.
     */
    class IndexRequestProxy : public QObject
    {
        Q_OBJECT

    public:
        enum class Mode { LoadIndexes, SaveIndex, DropIndex };

        IndexRequestProxy(QmlBackend *backend, MongoServer *server,
                          const QString &dbName, const QString &collName,
                          Mode mode);

    public Q_SLOTS:
        // EventBus dispatch requires slots named "handle".
        void handle(LoadCollectionIndexesResponse *event);
        void handle(AddEditIndexResponse *event);
        void handle(DropCollectionIndexResponse *event);

    private:
        QmlBackend *_backend;
        QPointer<MongoServer> _server;
        QString _dbName;
        QString _collName;
        Mode _mode;
    };

    /**
     * @brief QML-facing backend bridge. Exposes connection operations and the
     *        connected-servers tree model to QML, and translates EventBus
     *        events into model updates + signals.
     */
    class QmlBackend : public QObject
    {
        Q_OBJECT
        // Registered as the QML singleton "Backend" via QmlBackendForeign.

        Q_PROPERTY(RagoShell::ServerTreeModel *serverTree READ serverTree CONSTANT FINAL)
        Q_PROPERTY(bool connecting READ connecting NOTIFY connectingChanged FINAL)
        Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged FINAL)
        Q_PROPERTY(QString appVersion READ appVersion CONSTANT FINAL)
        Q_PROPERTY(QString qtVersion READ qtVersion CONSTANT FINAL)
        Q_PROPERTY(QVariantList shells READ shells NOTIFY shellsChanged FINAL)
        Q_PROPERTY(QVariantList monitors READ monitors NOTIFY monitorsChanged FINAL)
        Q_PROPERTY(RagoShell::QmlObjectListModel *shellModel READ shellModel CONSTANT FINAL)
        Q_PROPERTY(RagoShell::QmlObjectListModel *monitorModel READ monitorModel CONSTANT FINAL)
        /// The work-area tab strip (Welcome, shell and monitor tabs).
        Q_PROPERTY(RagoShell::TabModel *tabs READ tabs CONSTANT FINAL)
        Q_PROPERTY(QStringList connectedUuids READ connectedUuids NOTIFY connectedServersChanged FINAL)
        Q_PROPERTY(RagoShell::LogModel *logModel READ logModel CONSTANT FINAL)
        Q_PROPERTY(bool transferBusy READ transferBusy NOTIFY transferStateChanged FINAL)
        Q_PROPERTY(int transferProgress READ transferProgress NOTIFY transferStateChanged FINAL)
        Q_PROPERTY(QString transferStatus READ transferStatus NOTIFY transferStateChanged FINAL)

    public:
        explicit QmlBackend(QObject *parent = nullptr);
        ~QmlBackend() override;

        ServerTreeModel *serverTree() const { return _tree; }
        bool connecting() const { return _connecting; }
        QString statusText() const { return _statusText; }
        /// True while the status line is "<server>: N database(s)". The dev/CI
        /// helpers in main.cpp wait for it; the text itself is translated.
        bool databasesListed() const { return _statusIsDatabaseList; }
        QString appVersion() const { return QCoreApplication::applicationVersion(); }
        QString qtVersion() const { return QString::fromLatin1(qVersion()); }
        QVariantList shells() const { return _shells; }
        QVariantList monitors() const { return _monitors; }
        QmlObjectListModel *shellModel() const { return _shellModel; }
        QmlObjectListModel *monitorModel() const { return _monitorModel; }
        TabModel *tabs() const { return _tabs; }
        QStringList connectedUuids() const { return _serverUuids.values(); }
        LogModel *logModel() const { return _logModel; }
        bool transferBusy() const { return _transferBusy; }
        int transferProgress() const { return _transferProgress; }
        QString transferStatus() const { return _transferStatus; }

        /// Open a primary (explorer) connection for the saved connection 'uuid'.
        Q_INVOKABLE void connectToServer(const QString &uuid);

        /// What the command palette can jump to: every saved connection, and the databases and
        /// collections the explorer has loaded for connected servers. Each entry is a map with
        /// kind ("connection" | "database" | "collection"), title, subtitle, uuid, db, collection
        /// and (for connections) connected.
        Q_INVOKABLE QVariantList explorerTargets() const;

        /// Close the connected server that belongs to 'uuid' (if any).
        Q_INVOKABLE void disconnectServer(const QString &uuid, bool closeOpenShells = false);
        Q_INVOKABLE int openTabsCountForServer(const QString &uuid) const;
        Q_INVOKABLE int dirtyShellsCountForServer(const QString &uuid) const;

        /// Test a connection described by a toVariant()-compatible map.
        Q_INVOKABLE void testConnection(const QVariantMap &map);

        /// Parse a MongoDB connection URI (mongodb:// or mongodb+srv://) into dialog fields.
        Q_INVOKABLE QVariantMap parseMongoUri(const QString &uriText) const;

        /// Refresh a connected server's database list (and RS status if Replica Set).
        Q_INVOKABLE void refreshServer(const QString &uuid);

        /// Replica Set operations.
        Q_INVOKABLE void refreshReplicaSetFolder(const QString &uuid);
        Q_INVOKABLE void replicaSetStatus(const QString &uuid);
        Q_INVOKABLE void openMemberShell(const QString &uuid,
                                         const QString &memberAddress,
                                         const QString &script = QString(),
                                         bool execute = true);
        Q_INVOKABLE void openMemberDirectConnection(const QString &uuid,
                                                    const QString &memberAddress);

        /// Open a new shell tab on 'dbName' of the connected server 'uuid'.
        /// If 'collection' is set, the tab is prefilled with db.<col>.find({})
        /// and executed immediately.
        Q_INVOKABLE void openShell(const QString &uuid, const QString &dbName,
                                   const QString &collection = QString());

        /// Open a shell tab prefilled with an arbitrary script.
        /// When 'execute' is true, runs immediately; when false, lets the user edit first.
        Q_INVOKABLE void openShellWithScript(const QString &uuid,
                                             const QString &dbName,
                                             const QString &script,
                                             bool execute = false);

        /// Close the shell tab at 'index'.
        Q_INVOKABLE void closeShell(int index);
        /// Opens a tab with the script of a saved draft (the recovery list, Drafts).
        /// When its connection is not open yet, connects first (the user asked for it
        /// by clicking) and opens the tab once connected; nothing connects by itself.
        Q_INVOKABLE void recoverDraft(const QString &draftId);
        /// Removes the drafts of all open tabs (the user chose to discard them on exit).
        Q_INVOKABLE void discardOpenDrafts();
        /// Drops memory that can be rebuilt (schema snapshots held in memory); called on low-memory
        /// warnings of the system (mobile) and available as an action for tests.
        Q_INVOKABLE void trimMemory();
        /// True for a short time after the app checked this connection by itself (the network came
        /// back or the computer woke); errors in that time are shown in the status bar, not as a dialog.
        Q_INVOKABLE bool autoRefreshing(const QString &uuid) const;
        /// The user's answer to hostKeyRequested(): trust the SSH server's host key or refuse.
        Q_INVOKABLE void answerHostKey(int id, bool trust);
        /// Closes the shell or monitor tab of 'object'; null closes the Welcome tab.
        Q_INVOKABLE void closeTabObject(QObject *object);
        Q_INVOKABLE void duplicateShell(int index, const QString &currentScript = QString());
        Q_INVOKABLE void closeOtherShells(int keepIndex);
        Q_INVOKABLE void closeShellsToRight(int fromIndex);

        /// Live Server Monitor & Current Operations tabs.
        Q_INVOKABLE void openMonitor(const QString &uuid);
        Q_INVOKABLE void openMemberMonitor(const QString &uuid, const QString &memberAddress);
        Q_INVOKABLE void closeMonitor(int index);

        /// Connected server metadata helpers (used by version-aware IndexDialog, etc.).
        Q_INVOKABLE double serverVersion(const QString &uuid) const;
        Q_INVOKABLE QString serverVersionString(const QString &uuid) const;
        Q_INVOKABLE QString serverAddress(const QString &uuid) const;
        Q_INVOKABLE bool serverIsMongos(const QString &uuid) const;

        /// JSON & clipboard helpers for QML editors/dialogs.
        Q_INVOKABLE QString validateJson(const QString &jsonText, bool singleDocument = false) const;
        Q_INVOKABLE QString formatJson(const QString &jsonText) const;
        Q_INVOKABLE void copyToClipboard(const QString &text) const;
        /// Preferences › AI › Paste. Reads the clipboard only when called
        /// (the user pressed the button), never on its own. Returns
        /// {"key": text} or {"error": message}; rejected text is not returned.
        Q_INVOKABLE QVariantMap pasteApiKey() const;
        /// Empties the clipboard if it still holds exactly this text (the
        /// pasted key), so something copied since is left alone.
        Q_INVOKABLE bool clearClipboardIfEquals(const QString &text) const;
        /// Opens the folder of the log file in the file manager.
        Q_INVOKABLE bool openLogFolder() const;
        /// Log file of this run (empty when file logging is not active).
        Q_INVOKABLE QString logFilePath() const;
        /// Platform bindings of a QKeySequence::StandardKey plus the extra
        /// sequences that are not already among them. A Shortcut listing the
        /// same key twice only signals activatedAmbiguously (e.g. Ctrl+W next
        /// to StandardKey.Close on Windows), so it would never fire.
        Q_INVOKABLE QVariantList keySequences(int standardKey, const QStringList &extra = {}) const;

        /// Remembered UI layout (split sizes, panel visibility) in the
        /// "QmlLayout" settings group; an invalid QVariant when unset.
        Q_INVOKABLE QVariant uiState(const QString &key) const;
        Q_INVOKABLE void setUiState(const QString &key, const QVariant &value);
        /// Dev/CI snapshot runs neither read nor write the remembered layout.
        void setPersistUiState(bool persist) { _persistUiState = persist; }

        /// Insert one or more BSON/JSON documents into 'dbName.collName' from Explorer.
        /// Returns empty string on success, or an error message string on failure.
        Q_INVOKABLE QString insertDocumentsJson(const QString &uuid, const QString &dbName,
                                                const QString &collName, const QString &jsonText);

        /// Check whether a server with 'uuid' is currently connected in the tree.
        Q_INVOKABLE bool isServerConnected(const QString &uuid) const
        {
            return _serverUuids.values().contains(uuid);
        }

        /// Uuid of the first connected server ("" when none) — used by Ctrl+T.
        Q_INVOKABLE QString firstConnectedUuid() const;

        /// Copy of a connected server's settings (SSH-tunnel host already
        /// applied), for standalone clients; nullptr when not connected.
        std::unique_ptr<ConnectionSettings> cloneConnection(const QString &uuid) const;

        /// Convert a FileDialog QUrl into a native local file path cleanly.
        Q_INVOKABLE QString urlToLocalFile(const QUrl &url) const;
        /// Import an SSH key or TLS CA/PEM/CRL file picked via FileDialog; on mobile, copies
        /// into private <appDataDir>/credentials/ (0600) so libssh2 and mongoc get a real path.
        Q_INVOKABLE QString importCredentialFile(const QUrl &url);
        /// Authentication mechanisms to offer; "saved" (the connection being edited) is added when
        /// this build does not support it, so that opening and saving does not change it.
        Q_INVOKABLE QStringList authMechanisms(const QString &saved = QString()) const;

        /// Read a script file (Ctrl+O -> FileDialog passes a QUrl).
        Q_INVOKABLE QString readFile(const QUrl &url) const;
        /// Read one of the bundled open-source license documents ("notices", "gplv3", "lgplv3", "copyright").
        Q_INVOKABLE QString licenseDocument(const QString &name) const;
        /// Save 'text' to 'url'; returns false on I/O error.
        Q_INVOKABLE bool saveFile(const QUrl &url, const QString &text);
        /// Browser download of 'text' as 'fileNameHint' (WASM only —
        /// QFileDialog::saveFileContent; docs/WebStorage.md §3). False elsewhere.
        Q_INVOKABLE bool saveFileAs(const QString &fileNameHint, const QString &text);
        /// Browser download of the file at 'path' under its own name (WASM only;
        /// used after a background export wrote to MEMFS). False elsewhere.
        Q_INVOKABLE bool downloadFile(const QString &path);

        /// Database / collection / index / user / function management.
        Q_INVOKABLE void createDatabase(const QString &uuid, const QString &name);
        Q_INVOKABLE void dropDatabase(const QString &uuid, const QString &name);
        Q_INVOKABLE void refreshDatabase(const QString &uuid, const QString &dbName);
        /// Database context menu: analyze the schema of every collection
        /// (docs/SchemaIntelligence.md), regardless of the collection mode.
        Q_INVOKABLE void analyzeDatabaseSchema(const QString &uuid, const QString &dbName);
        /// Connection context menu: drops the connection's schema catalog
        /// and pending schema jobs; a connected server collects again as it
        /// is used.
        Q_INVOKABLE void forgetSchema(const QString &uuid);
        /// The background schema collector's triggers (no-ops when it is off).
        void schemaDatabaseInUse(const QString &uuid, const QString &dbName);
        /// The collector's progress for a database (total -1 = not listed yet).
        SchemaCollector::Progress schemaProgress(const QString &uuid, const QString &dbName) const
        {
            return _schemaCollector ? _schemaCollector->progress(uuid, dbName) : SchemaCollector::Progress();
        }
        void schemaCollectionInUse(const QString &uuid, const QString &dbName, const QString &collName);
        Q_INVOKABLE void refreshCollections(const QString &uuid, const QString &dbName);
        Q_INVOKABLE void refreshFunctions(const QString &uuid, const QString &dbName);
        Q_INVOKABLE void refreshUsers(const QString &uuid, const QString &dbName);
        Q_INVOKABLE QStringList serverDatabaseNames(const QString &uuid) const;
        Q_INVOKABLE void requestDatabaseUsers(const QString &uuid, const QString &dbName);
        Q_INVOKABLE QVariantList cachedDatabaseUsers(const QString &uuid, const QString &dbName) const;
        Q_INVOKABLE void refreshIndexes(const QString &uuid, const QString &dbName,
                                        const QString &collName);

        Q_INVOKABLE void createCollection(const QString &uuid, const QString &dbName,
                                          const QString &collName);
        Q_INVOKABLE bool createCollectionAdvanced(const QString &uuid, const QString &dbName,
                                                  const QString &collName,
                                                  bool capped, qint64 sizeBytes, int maxDocs,
                                                  const QString &validatorJson,
                                                  const QString &validationLevel,
                                                  const QString &validationAction,
                                                  const QString &storageEngineJson,
                                                  const QString &extraOptionsJson);
        Q_INVOKABLE void dropCollection(const QString &uuid, const QString &dbName,
                                        const QString &collName);
        Q_INVOKABLE void renameCollection(const QString &uuid, const QString &dbName,
                                          const QString &collName, const QString &newName);
        Q_INVOKABLE void duplicateCollection(const QString &uuid, const QString &dbName,
                                             const QString &collName, const QString &newName);
        Q_INVOKABLE void removeAllDocuments(const QString &uuid, const QString &dbName,
                                            const QString &collName);

        Q_INVOKABLE void saveIndex(const QString &uuid, const QString &dbName,
                                   const QString &collName, const QString &oldIndexName,
                                   const QVariantMap &spec);
        Q_INVOKABLE void setIndexHidden(const QString &uuid, const QString &dbName,
                                        const QString &collName, const QVariantMap &indexMeta,
                                        bool hidden);
        Q_INVOKABLE void dropIndex(const QString &uuid, const QString &dbName,
                                   const QString &collName, const QString &indexName);

        Q_INVOKABLE bool createOrUpdateFunction(const QString &uuid, const QString &dbName,
                                                const QString &oldName, const QString &funcName,
                                                const QString &code);
        Q_INVOKABLE void dropFunction(const QString &uuid, const QString &dbName,
                                      const QString &funcName);

        Q_INVOKABLE bool createOrUpdateUser(const QString &uuid, const QString &dbName,
                                            const QString &userName, const QString &password,
                                            const QString &roleDb, const QStringList &roles,
                                            bool isUpdate = false,
                                            const QStringList &mechanisms = QStringList());
        Q_INVOKABLE void dropUser(const QString &uuid, const QString &dbName,
                                  const QString &userName);

        /// Native C++ JSON Array / NDJSON / CSV export & import (0 = JSON Array, 1 = NDJSON, 2 = CSV).
        Q_INVOKABLE void startExportCollection(const QString &uuid, const QString &dbName,
                                               const QString &collName, const QString &filePath,
                                               int format, const QString &filterJson = QString(),
                                               int limit = 0);
        Q_INVOKABLE void startImportCollection(const QString &uuid, const QString &dbName,
                                               const QString &collName, const QString &filePath,
                                               int format, bool dropBeforeImport = false,
                                               bool stopOnError = false);
        Q_INVOKABLE void cancelTransfer();

        // Internal callbacks invoked by IndexRequestProxy
        void onIndexesLoaded(MongoServer *server, const QString &dbName,
                             const QString &collName, LoadCollectionIndexesResponse *event);
        void onIndexSaved(MongoServer *server, const QString &dbName,
                          const QString &collName, AddEditIndexResponse *event);
        void onIndexDropped(MongoServer *server, const QString &dbName,
                            const QString &collName, DropCollectionIndexResponse *event);

    public Q_SLOTS:
        /// Re-applies translated text the models cache (tree folder labels, the
        /// Welcome tab) after a language switch.
        void retranslate();

        // EventBus dispatch requires methods named "handle".
        void handle(ConnectingEvent *event);
        void handle(ConnectionEstablishedEvent *event);
        void handle(ConnectionFailedEvent *event);
        void handle(DatabaseListLoadedEvent *event);
        void handle(MongoServerLoadingDatabasesEvent *event);
        void handle(ReplicaSetFolderLoading *event);
        void handle(ReplicaSetFolderRefreshed *event);
        void handle(MongoDatabaseCollectionListLoadedEvent *event);
        void handle(MongoDatabaseCollectionsLoadingEvent *event);
        void handle(MongoDatabaseFunctionsLoadedEvent *event);
        void handle(MongoDatabaseFunctionsLoadingEvent *event);
        void handle(MongoDatabaseUsersLoadedEvent *event);
        void handle(MongoDatabaseUsersLoadingEvent *event);
        void handle(OpeningShellEvent *event);
        void handle(InsertDocumentResponse *event);
        void handle(OperationFailedEvent *event);
        void handle(LogEvent *event);

    private Q_SLOTS:
        void loadCollectionIndexes(MongoServer *server, const QString &dbName,
                                   const QString &collName);

    Q_SIGNALS:
        void connectingChanged();
        void statusTextChanged();
        /// Emitted when a connection or operation fails with a user-visible error.
        void connectionError(const QString &uuid, const QString &message);
        /// An SSH tunnel met a host key that is new or has changed and waits for answerHostKey().
        void hostKeyRequested(int id, const QString &host, int port, const QString &keyType,
                              const QString &fingerprint, bool changed);
        /// Emitted for ConnectionTest results: (success, message).
        void testResult(bool success, const QString &message);
        /// Step-by-step diagnostic rows for ConnectionDialog: [{ label, status, detail }]
        void testDiagnosticSteps(const QVariantList &steps);
        void shellsChanged();
        void monitorsChanged();
        void connectedServersChanged();
        /// Emitted after a new shell tab controller was appended to 'shells'.
        void shellOpened(QObject *controller);
        /// Emitted after a new ServerMonitorController was appended to 'monitors'.
        void monitorOpened(QObject *controller);
        /// Emitted when openMonitor() focuses an already-open monitor tab at 'index'.
        void monitorActivated(int index);
        void transferStateChanged();
        void transferFinished(bool success, int count, const QString &message);
        void databaseUsersLoading(const QString &uuid, const QString &dbName);
        void databaseUsersLoaded(const QString &uuid, const QString &dbName,
                                 const QVariantList &users, const QString &errorMessage);
        void documentInsertFinished(bool success, const QString &message);

    private:
        void testConnectionNow(const QVariantMap &map);
        void setStatus(const QString &text, bool databaseList = false);
        /// "<server>: N database(s)"; flags the status for databasesListed().
        void setDatabaseListStatus(const QString &serverName, int count);
        MongoServer *findServerByUuid(const QString &uuid) const;
        void openRecoveredDraft(MongoServer *server, const ScriptDraft &draft);
        void openPendingRecoveries(const QString &uuid);
        MongoDatabase *findDatabaseBySender(QObject *sender) const;
        /// The collector's options for a connected server (its own mode or
        /// the global one) and a copy of its connection; false when not connected.
        bool schemaOptions(const QString &uuid, SchemaCollector::Options *options,
                           std::shared_ptr<const ConnectionSettings> *connection) const;

        EventBus *_bus;
        SchemaCollector *_schemaCollector = nullptr;
        App *_app;
        ServerTreeModel *_tree;
        bool _connecting = false;
        QString _statusText;
        bool _statusIsDatabaseList = false;
        int _testServerHandle = -1;
        QVariantMap _lastTestMap;
        /// uuid of each server node in the tree, keyed by MongoServer*.
        QHash<MongoServer *, QString> _serverUuids;
        /// Exact MongoDB version strings (e.g. "7.0.8") reported on connect.
        QHash<MongoServer *, QString> _serverVersionStrings;
        /// Cached database user lists ("<uuid>/<dbName>" -> QVariantList of user maps).
        QHash<QString, QVariantList> _databaseUsersCache;
        /// Shell tab controllers, in tab order.
        QVariantList _shells;
        /// Live Server Monitor tab controllers, in tab order.
        QVariantList _monitors;
        QmlObjectListModel *_shellModel;
        QmlObjectListModel *_monitorModel;
        TabModel *_tabs;
        bool _persistUiState = true;
        /// Drafts waiting for their connection to come up: server uuid -> draft ids.
        QMultiHash<QString, QString> _pendingRecoveries;
        BackgroundMonitor *_monitor = nullptr;
        QHash<QString, qint64> _autoRefreshUntilMs;   // server uuid -> end of the quiet window
        void recheckConnections();
        LogModel *_logModel;

        bool _transferBusy = false;
        int _transferProgress = 0;
        QString _transferStatus;
        std::atomic<bool> _transferCancel{false};
        std::unique_ptr<std::thread> _transferThread;

        int _pendingExplorerInserts = 0;
        int _explorerInsertBatchTotal = 0;
        bool _explorerInsertBatchHadError = false;
        QString _explorerInsertLastError;
        QString _explorerInsertDb;
        QString _explorerInsertColl;
    };
}
