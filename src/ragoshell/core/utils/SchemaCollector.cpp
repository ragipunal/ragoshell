#include "ragoshell/core/utils/SchemaCollector.h"

#include <algorithm>
#include <climits>
#include <thread>

#include <QDateTime>
#include <QPointer>

#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/utils/SchemaCatalog.h"
#include "ragoshell/core/utils/SchemaService.h"

namespace RagoShell
{
    namespace
    {
        constexpr int kListMaxTimeMs = 10000;
    }

    SchemaCollector::SchemaCollector(SchemaService *service, SchemaCatalog *catalog, ListFunction list,
                                     QObject *parent)
        : QObject(parent),
          _service(service),
          _catalog(catalog),
          _list(std::move(list))
    {
        if (!_list) {
            _list = [](const ConnectionSettings &connection, const QString &database) {
                return SchemaAnalysis::listCollections(connection, database, kListMaxTimeMs);
            };
        }
        if (_service) {
            connect(_service, &SchemaService::queueChanged, this, &SchemaCollector::onQueueChanged);
            connect(_service, &SchemaService::snapshotReady, this,
                    [this](const QString &connectionId, const QString &database, const QString &) {
                        emit progressChanged(connectionId, database);
                    });
        }
    }

    QString SchemaCollector::key(const QString &connectionId, const QString &database)
    {
        return connectionId + QLatin1Char('\n') + database;
    }

    bool SchemaCollector::isAnalyzable(const SchemaAnalysis::CollectionEntry &collection)
    {
        return !collection.name.startsWith(QLatin1String("system."))
               && collection.type != QLatin1String("view");
    }

    bool SchemaCollector::isBackgroundDatabase(const QString &database)
    {
        return database != QLatin1String("local") && database != QLatin1String("config");
    }

    QStringList SchemaCollector::backgroundCandidates(const std::vector<SchemaAnalysis::CollectionEntry> &collections,
                                                      const std::function<bool(const QString &)> &hasFreshSnapshot,
                                                      int limit)
    {
        std::vector<const SchemaAnalysis::CollectionEntry *> picked;
        for (const auto &c : collections) {
            if (isAnalyzable(c) && !(hasFreshSnapshot && hasFreshSnapshot(c.name)))
                picked.push_back(&c);
        }
        // Small collections are analyzed whole and cheaply: they go first.
        std::stable_sort(picked.begin(), picked.end(), [](const auto *a, const auto *b) {
            if ((a->documents < 0) != (b->documents < 0))
                return b->documents < 0;
            return a->documents < b->documents;
        });
        QStringList names;
        for (const auto *c : picked) {
            if (names.size() >= limit)
                break;
            names << c->name;
        }
        return names;
    }

    void SchemaCollector::databaseInUse(const QString &connectionId,
                                        std::shared_ptr<const ConnectionSettings> connection,
                                        const QString &database, const Options &options)
    {
        if (options.mode == Mode::Off || !_service || !connection || !isBackgroundDatabase(database))
            return;
        const DatabaseState &state = _databases[key(connectionId, database)];
        if (state.listed || state.listing)
            return;
        list(connectionId, std::move(connection), database, options);
    }

    void SchemaCollector::collectionInUse(const QString &connectionId,
                                          std::shared_ptr<const ConnectionSettings> connection,
                                          const QString &database, const QString &collection,
                                          const Options &options)
    {
        if (options.mode == Mode::Off || !_service || !connection || collection.startsWith(QLatin1String("system.")))
            return;
        const QString hint = key(connectionId, database) + QLatin1Char('\n') + collection;
        if (_hinted.contains(hint))
            return;
        if (enqueue(connectionId, std::move(connection), database, collection, options,
                    static_cast<int>(SchemaService::Priority::Hint)))
            _hinted.insert(hint);
    }

    void SchemaCollector::analyzeDatabase(const QString &connectionId,
                                          std::shared_ptr<const ConnectionSettings> connection,
                                          const QString &database, const Options &options)
    {
        if (!_service || !connection)
            return;
        DatabaseState &state = _databases[key(connectionId, database)];
        state.explicitRequest = true;
        if (state.listing)
            return;   // the listing in flight queues everything
        // Listed again: collections may have been added since.
        list(connectionId, std::move(connection), database, options);
    }

    void SchemaCollector::databasesListed(const QString &connectionId,
                                          std::shared_ptr<const ConnectionSettings> connection,
                                          const QStringList &databases, const Options &options)
    {
        if (options.mode != Mode::WholeDatabase)
            return;
        for (const QString &database : databases)
            databaseInUse(connectionId, connection, database, options);
    }

    void SchemaCollector::disconnected(const QString &connectionId)
    {
        ++_generations[connectionId];
        const QString prefix = connectionId + QLatin1Char('\n');
        for (auto it = _databases.begin(); it != _databases.end();) {
            if (it->first.startsWith(prefix))
                it = _databases.erase(it);
            else
                ++it;
        }
        for (auto it = _hinted.begin(); it != _hinted.end();) {
            if (it->startsWith(prefix))
                it = _hinted.erase(it);
            else
                ++it;
        }
    }

    void SchemaCollector::list(const QString &connectionId, std::shared_ptr<const ConnectionSettings> connection,
                               const QString &database, const Options &options)
    {
        _databases[key(connectionId, database)].listing = true;
        emit progressChanged(connectionId, database);
        std::thread([self = QPointer<SchemaCollector>(this), connectionId, connection, database, options,
                     generation = _generations[connectionId], listFunction = _list]() {
            const SchemaAnalysis::CollectionList result = listFunction(*connection, database);
            QMetaObject::invokeMethod(self, [self, connectionId, connection, database, options, generation, result]() {
                // A disconnect in the meantime drops the listing.
                if (self && self->_generations[connectionId] == generation)
                    self->onListed(connectionId, connection, database, options, result);
            }, Qt::QueuedConnection);
        }).detach();
    }

    void SchemaCollector::onListed(const QString &connectionId, std::shared_ptr<const ConnectionSettings> connection,
                                   const QString &database, const Options &options,
                                   const SchemaAnalysis::CollectionList &list)
    {
        DatabaseState &state = _databases[key(connectionId, database)];
        const bool explicitRequest = state.explicitRequest;
        state.listing = false;
        state.explicitRequest = false;
        state.listed = true;   // also on error: no retry loop
        if (!list.ok) {
            LOG_MSG(QStringLiteral("Schema analysis of %1: could not list the collections: %2")
                        .arg(database, list.error),
                    LogSeverity::Warning);
            emit progressChanged(connectionId, database);
            return;
        }

        state.collections.clear();
        for (const auto &c : list.collections) {
            if (isAnalyzable(c))
                state.collections << c.name;
        }
        const QDateTime now = QDateTime::currentDateTimeUtc();
        auto fresh = [this, &connectionId, &database, &options, &now](const QString &collection) {
            if (!_catalog)
                return false;
            const auto snapshot = _catalog->find(connectionId, database, collection);
            return snapshot && !SchemaCatalog::isStale(*snapshot, now)
                   && !(options.valuesIncluded && !snapshot->valuesIncluded);
        };
        const int limit = explicitRequest ? INT_MAX : std::max(0, options.maxCollections - state.queued);
        // The user asked: Hint jobs, which a permission stop does not block.
        const auto priority = explicitRequest ? SchemaService::Priority::Hint : SchemaService::Priority::Background;
        int queued = 0;
        for (const QString &collection : backgroundCandidates(list.collections, fresh, limit)) {
            if (enqueue(connectionId, connection, database, collection, options, static_cast<int>(priority)))
                ++queued;
        }
        if (!explicitRequest)
            state.queued += queued;
        if (queued > 0) {
            state.active = true;
            LOG_MSG(QStringLiteral("Schema analysis of %1: %2 of %3 collections queued")
                        .arg(database).arg(queued).arg(state.collections.size()),
                    LogSeverity::Info);
        }
        emit progressChanged(connectionId, database);
    }

    quint64 SchemaCollector::enqueue(const QString &connectionId, std::shared_ptr<const ConnectionSettings> connection,
                                     const QString &database, const QString &collection, const Options &options,
                                     int priority)
    {
        SchemaService::Request request;
        request.connectionId = connectionId;
        request.connection = std::move(connection);
        request.database = database;
        request.collection = collection;
        request.priority = static_cast<SchemaService::Priority>(priority);
        request.sampleSize = options.sampleSize;
        request.onlyIfStale = true;
        request.valuesIncluded = options.valuesIncluded;
        request.serverVersion = options.serverVersion;
        request.uuidEncoding = options.uuidEncoding;
        request.timeZone = options.timeZone;
        return _service->enqueue(request);
    }

    void SchemaCollector::onQueueChanged(const QString &connectionId)
    {
        const QString prefix = connectionId + QLatin1Char('\n');
        for (auto &[k, state] : _databases) {
            if (!k.startsWith(prefix))
                continue;
            const QString database = k.mid(prefix.size());
            if (state.active && _service->pending(connectionId, database) == 0) {
                state.active = false;
                LOG_MSG(QStringLiteral("Schema analysis of %1 finished: %2 of %3 collections analyzed")
                            .arg(database)
                            .arg(analyzedCount(connectionId, database, state.collections))
                            .arg(state.collections.size()),
                        LogSeverity::Info);
            }
            emit progressChanged(connectionId, database);
        }
    }

    int SchemaCollector::analyzedCount(const QString &connectionId, const QString &database,
                                       const QStringList &collections) const
    {
        if (!_catalog)
            return 0;
        int n = 0;
        for (const QString &c : _catalog->analyzedCollections(connectionId, database)) {
            if (collections.contains(c))
                ++n;
        }
        return n;
    }

    SchemaCollector::Progress SchemaCollector::progress(const QString &connectionId, const QString &database) const
    {
        Progress p;
        auto it = _databases.find(key(connectionId, database));
        if (it != _databases.end() && it->second.listed) {
            p.total = static_cast<int>(it->second.collections.size());
            p.analyzed = analyzedCount(connectionId, database, it->second.collections);
        }
        if (_service)
            p.pending = _service->pending(connectionId, database);
        return p;
    }
}
