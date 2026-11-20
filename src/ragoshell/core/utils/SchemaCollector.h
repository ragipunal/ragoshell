#pragma once

#include <functional>
#include <map>
#include <memory>

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/SchemaAnalysis.h"

namespace RagoShell
{
    class ConnectionSettings;
    class SchemaCatalog;
    class SchemaService;

    /**
     * @brief Decides what the background schema collector analyzes
     * (docs/SchemaIntelligence.md) and queues it on the
     * SchemaService. A database is listed once per session when it comes
     * into use; its collections without a fresh snapshot are then queued as
     * Background jobs, smallest first, up to the per-database limit.
     *
     * Lives on the GUI thread; the listing runs on a detached worker thread.
     * Tests inject the listing function.
     */
    class SchemaCollector : public QObject
    {
        Q_OBJECT

    public:
        using Mode = SettingsManager::SchemaCollectionMode;

        struct Options
        {
            Mode mode = Mode::OnUse;   ///< resolved: the connection's override or the global setting
            int sampleSize = SettingsManager::kDefaultSchemaBackgroundSampleSize;
            int maxCollections = SettingsManager::kDefaultSchemaBackgroundMaxCollections;
            bool valuesIncluded = false;
            QString serverVersion;
            UUIDEncoding uuidEncoding = DefaultEncoding;
            SupportedTimes timeZone = Utc;
        };

        struct Progress
        {
            int total = -1;     ///< collections that can be analyzed; -1 = not listed yet
            int analyzed = 0;   ///< of those, with a snapshot in the catalog
            int pending = 0;    ///< jobs queued or running for the database
        };

        /// Blocking; runs on the worker thread.
        using ListFunction = std::function<SchemaAnalysis::CollectionList(const ConnectionSettings &,
                                                                          const QString &database)>;

        /// An empty function uses SchemaAnalysis::listCollections.
        SchemaCollector(SchemaService *service, SchemaCatalog *catalog, ListFunction list = {},
                        QObject *parent = nullptr);

        /// Collections worth a background job, in queue order: no system
        /// collections and no views; none with a fresh snapshot
        /// (hasFreshSnapshot); smallest first, unknown sizes last; at most limit.
        static QStringList backgroundCandidates(const std::vector<SchemaAnalysis::CollectionEntry> &collections,
                                                const std::function<bool(const QString &)> &hasFreshSnapshot,
                                                int limit);
        /// Not a system collection and not a view.
        static bool isAnalyzable(const SchemaAnalysis::CollectionEntry &collection);
        /// local and config hold server internals (oplog, sessions): never in the background.
        static bool isBackgroundDatabase(const QString &database);

        /// The database came into use (tree expanded, shell switched to it).
        /// Listed and queued once per session; nothing when the mode is Off.
        void databaseInUse(const QString &connectionId, std::shared_ptr<const ConnectionSettings> connection,
                           const QString &database, const Options &options);
        /// The user is working with the collection (shell): a Hint job, once
        /// per collection and session (the shell asks on every keystroke).
        void collectionInUse(const QString &connectionId, std::shared_ptr<const ConnectionSettings> connection,
                             const QString &database, const QString &collection, const Options &options);
        /// "Analyze schema of all collections": ignores the mode and the limit.
        void analyzeDatabase(const QString &connectionId, std::shared_ptr<const ConnectionSettings> connection,
                             const QString &database, const Options &options);
        /// The database list of a connection arrived: WholeDatabase mode
        /// puts every database in use.
        void databasesListed(const QString &connectionId, std::shared_ptr<const ConnectionSettings> connection,
                             const QStringList &databases, const Options &options);
        /// Forgets the session state of a connection.
        void disconnected(const QString &connectionId);

        Progress progress(const QString &connectionId, const QString &database) const;

    Q_SIGNALS:
        void progressChanged(const QString &connectionId, const QString &database);

    private:
        struct DatabaseState
        {
            bool listing = false;
            bool listed = false;
            bool explicitRequest = false;   ///< analyzeDatabase while listing
            QStringList collections;        ///< analyzable ones
            int queued = 0;                 ///< background jobs queued this session
            bool active = false;            ///< a batch is running (for its log line)
        };

        static QString key(const QString &connectionId, const QString &database);
        void list(const QString &connectionId, std::shared_ptr<const ConnectionSettings> connection,
                  const QString &database, const Options &options);
        void onListed(const QString &connectionId, std::shared_ptr<const ConnectionSettings> connection,
                      const QString &database, const Options &options, const SchemaAnalysis::CollectionList &list);
        quint64 enqueue(const QString &connectionId, std::shared_ptr<const ConnectionSettings> connection,
                        const QString &database, const QString &collection, const Options &options,
                        int priority);
        void onQueueChanged(const QString &connectionId);
        int analyzedCount(const QString &connectionId, const QString &database, const QStringList &collections) const;

        SchemaService *_service;
        SchemaCatalog *_catalog;
        ListFunction _list;
        std::map<QString, DatabaseState> _databases;   // key(connection, database)
        std::map<QString, quint64> _generations;       // by connection; bumped by disconnected()
        QSet<QString> _hinted;                         // "<connection>\n<database>\n<collection>"
    };
}
