#pragma once

#include <map>
#include <optional>

#include <QDateTime>
#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QStringList>

#include "ragoshell/core/utils/SchemaAnalysis.h"

namespace RagoShell
{
    /**
     * @brief The schema of one collection as it was analyzed, kept so the AI
     * assistant (and others) need not sample again every time.
     */
    struct SchemaSnapshot
    {
        QString database;
        QString collection;
        QString serverVersion;
        int sampleSize = 0;          ///< requested; result.sampledDocuments is what was read
        /// false: values (frequent values, ranges, letter case) were not
        /// kept, because the user did not allow sending them.
        bool valuesIncluded = true;
        SchemaResult result;         ///< analyzedAt, totalDocuments, fields, indexes, ...
    };

    /**
     * @brief Schema snapshots per connection, database and collection,
     * persisted as one JSON file per database:
     * <root>/<connection uuid>/<database>.json.
     *
     * Thread-safe. instance() is the application's catalog (set in main);
     * tests create their own with a temporary root.
     */
    class SchemaCatalog
    {
    public:
        static constexpr int kMaxAgeDays = 7;
        /// Relative change of the document count that makes a snapshot stale.
        static constexpr double kMaxCountChange = 0.25;

        explicit SchemaCatalog(const QString &rootDir);

        static SchemaCatalog *instance();
        static void setInstance(SchemaCatalog *catalog);

        QString rootDir() const { return _root; }

        std::optional<SchemaSnapshot> find(const QString &connectionId, const QString &database,
                                           const QString &collection);
        /// Stores (replaces) a snapshot and writes its database file. Values
        /// are dropped when snapshot.valuesIncluded is false.
        bool store(const QString &connectionId, const SchemaSnapshot &snapshot, QString *error = nullptr);
        bool remove(const QString &connectionId, const QString &database, const QString &collection);
        /// Forgets every snapshot of a connection (files included).
        bool clear(const QString &connectionId);
        /// Drops the snapshots held in memory (low memory); the files stay and are read again on use.
        void dropMemoryCache();

        /// Collections of a database that have a snapshot, sorted.
        QStringList analyzedCollections(const QString &connectionId, const QString &database);

        struct Coverage
        {
            int total = 0;      ///< collections asked about
            int analyzed = 0;   ///< of those, with a snapshot
            int stale = 0;      ///< of the analyzed, older than kMaxAgeDays
        };
        Coverage coverage(const QString &connectionId, const QString &database,
                          const QStringList &collections, const QDateTime &now);

        /// Too old, or the collection grew or shrank by more than
        /// kMaxCountChange (currentCount -1 = unknown, only the age counts).
        static bool isStale(const SchemaSnapshot &snapshot, const QDateTime &now, qint64 currentCount = -1);

        static QJsonObject toJson(const SchemaSnapshot &snapshot);
        static std::optional<SchemaSnapshot> fromJson(const QJsonObject &json);
        /// The snapshot without values (what is stored when valuesIncluded is false).
        static SchemaSnapshot withoutValues(SchemaSnapshot snapshot);

    private:
        using Collections = std::map<QString, SchemaSnapshot>;

        QString filePath(const QString &connectionId, const QString &database) const;
        Collections &load(const QString &connectionId, const QString &database);   // _mutex held
        bool save(const QString &connectionId, const QString &database, QString *error);   // _mutex held

        QString _root;
        QMutex _mutex;
        std::map<QString, Collections> _cache;   // "<connection>\n<database>"
    };
}
