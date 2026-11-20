#pragma once

#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <QObject>
#include <QSet>
#include <QString>

#include "ragoshell/core/utils/SchemaAnalysis.h"

namespace RagoShell
{
    class ConnectionSettings;
    class SchemaCatalog;

    /// What a schema job ended with, delivered to every ticket the job served.
    struct SchemaJobOutcome
    {
        /// false: the stored snapshot was still fresh (onlyIfStale), or the
        /// job was cancelled; result is then empty.
        bool sampled = false;
        bool cancelled = false;
        SchemaResult result;
    };

    /**
     * @brief The one owner of schema sampling (docs/SchemaIntelligence.md).
     * Jobs are queued per connection and run one at a time per
     * connection on a detached worker thread; a job for a collection that is
     * already queued or running is merged into it rather than run twice.
     * Results go to the schema catalog and to the tickets that asked.
     *
     * Lives on the GUI thread; instance() is the application's service (set
     * in main). Tests inject the analysis and count functions.
     */
    class SchemaService : public QObject
    {
        Q_OBJECT

    public:
        enum class Priority
        {
            Background = 0,   ///< collector, nobody waiting
            Hint = 1,         ///< the user is working with the collection
            Interactive = 2,  ///< a dialog waits for the result
        };

        struct Request
        {
            QString connectionId;
            std::shared_ptr<const ConnectionSettings> connection;
            QString database;
            QString collection;
            Priority priority = Priority::Background;
            int sampleSize = 200;
            int maxTimeMs = 30000;
            /// Sample only when the stored snapshot is missing or stale (age,
            /// or the document count moved; counted on the server).
            bool onlyIfStale = false;
            /// Keep values in the catalog (aiIncludeSampleValues).
            bool valuesIncluded = false;
            QString serverVersion;
            UUIDEncoding uuidEncoding = DefaultEncoding;
            SupportedTimes timeZone = Utc;
        };

        /// Blocking; runs on the worker thread.
        using AnalyzeFunction = std::function<SchemaResult(const ConnectionSettings &, const QString &database,
                                                           const QString &collection, int sampleSize,
                                                           int maxTimeMs, UUIDEncoding, SupportedTimes)>;
        /// Estimated document count, -1 on error. Blocking; worker thread.
        using CountFunction = std::function<qint64(const ConnectionSettings &, const QString &database,
                                                   const QString &collection)>;

        /// Empty functions use SchemaAnalysis.
        explicit SchemaService(SchemaCatalog *catalog, AnalyzeFunction analyze = {}, CountFunction count = {},
                               QObject *parent = nullptr);

        static SchemaService *instance();
        static void setInstance(SchemaService *service);

        /// Queues (or merges) the job; the returned ticket comes back in
        /// finished(). 0 when the request is unusable (no connection) or
        /// background work for the database was stopped by a permission error.
        quint64 enqueue(const Request &request);

        /// Drops a connection's queue (disconnect). Queued tickets finish as
        /// cancelled; the running job finishes as cancelled and is not stored.
        void cancel(const QString &connectionId);

        /// Jobs queued or running for a connection's database.
        int pending(const QString &connectionId, const QString &database) const;
        bool isBusy(const QString &connectionId) const;

        /// A permission error stopped background work for the database.
        bool isBlocked(const QString &connectionId, const QString &database) const;

        /// While paused, Background jobs stay queued (a running job finishes); Hint and
        /// Interactive jobs, which the user is waiting for, still run. Resuming starts the queue.
        void setBackgroundPaused(bool paused);
        bool backgroundPaused() const { return _backgroundPaused; }

        static bool isAuthorizationError(const QString &error);

    Q_SIGNALS:
        void finished(quint64 ticket, const RagoShell::SchemaJobOutcome &outcome);
        /// A new snapshot is in the catalog.
        void snapshotReady(const QString &connectionId, const QString &database, const QString &collection);
        void queueChanged(const QString &connectionId);

    private:
        struct Job
        {
            Request request;
            std::vector<quint64> tickets;
            quint64 order = 0;   ///< FIFO within a priority
            int retries = 0;
        };

        struct Lane
        {
            std::vector<Job> queue;
            std::unique_ptr<Job> running;
            quint64 generation = 0;   ///< bumped by cancel()
        };

        static QString key(const QString &database, const QString &collection);
        static void merge(Request &into, const Request &from);
        /// The running job already gives what the request wants.
        static bool covers(const Request &running, const Request &wanted);

        void startNext(const QString &connectionId);
        void onDone(const QString &connectionId, quint64 generation, const SchemaJobOutcome &outcome);
        void store(const Request &request, const SchemaResult &result);

        SchemaCatalog *_catalog;
        AnalyzeFunction _analyze;
        CountFunction _count;
        std::map<QString, Lane> _lanes;          // by connection id
        QSet<QString> _blocked;                  // "<connection>\n<database>"
        quint64 _nextTicket = 0;
        quint64 _nextOrder = 0;
        bool _backgroundPaused = false;
    };
}
