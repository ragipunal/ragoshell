#include "ragoshell/core/utils/SchemaService.h"

#include <algorithm>
#include <optional>
#include <thread>

#include <QDateTime>
#include <QPointer>

#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/utils/SchemaCatalog.h"

namespace RagoShell
{
    namespace
    {
        SchemaService *g_instance = nullptr;

        constexpr int kCountMaxTimeMs = 5000;
        /// A failed background job is tried once more, at the end of the queue.
        constexpr int kBackgroundRetries = 1;

        QString blockKey(const QString &connectionId, const QString &database)
        {
            return connectionId + QLatin1Char('\n') + database;
        }
    }

    SchemaService::SchemaService(SchemaCatalog *catalog, AnalyzeFunction analyze, CountFunction count,
                                 QObject *parent)
        : QObject(parent),
          _catalog(catalog),
          _analyze(std::move(analyze)),
          _count(std::move(count))
    {
        if (!_analyze) {
            _analyze = [](const ConnectionSettings &connection, const QString &database, const QString &collection,
                          int sampleSize, int maxTimeMs, UUIDEncoding uuidEncoding, SupportedTimes timeZone) {
                return SchemaAnalysis::analyzeCollection(connection, database, collection, sampleSize, maxTimeMs,
                                                         uuidEncoding, timeZone);
            };
        }
        if (!_count) {
            _count = [](const ConnectionSettings &connection, const QString &database, const QString &collection) {
                return SchemaAnalysis::estimatedDocumentCount(connection, database, collection, kCountMaxTimeMs);
            };
        }
    }

    SchemaService *SchemaService::instance()
    {
        return g_instance;
    }

    void SchemaService::setInstance(SchemaService *service)
    {
        g_instance = service;
    }

    QString SchemaService::key(const QString &database, const QString &collection)
    {
        return database + QLatin1Char('\n') + collection;
    }

    void SchemaService::merge(Request &into, const Request &from)
    {
        into.priority = std::max(into.priority, from.priority);
        into.sampleSize = std::max(into.sampleSize, from.sampleSize);
        into.maxTimeMs = std::max(into.maxTimeMs, from.maxTimeMs);
        into.onlyIfStale = into.onlyIfStale && from.onlyIfStale;
        into.valuesIncluded = into.valuesIncluded || from.valuesIncluded;
        if (into.serverVersion.isEmpty())
            into.serverVersion = from.serverVersion;
    }

    bool SchemaService::covers(const Request &running, const Request &wanted)
    {
        return running.sampleSize >= wanted.sampleSize
               && (!running.onlyIfStale || wanted.onlyIfStale)
               && (running.valuesIncluded || !wanted.valuesIncluded);
    }

    bool SchemaService::isAuthorizationError(const QString &error)
    {
        const QString e = error.toLower();
        return e.contains(QLatin1String("not authorized")) || e.contains(QLatin1String("unauthorized"))
               || e.contains(QLatin1String("requires authentication"))
               || e.contains(QLatin1String("authentication failed"));
    }

    quint64 SchemaService::enqueue(const Request &request)
    {
        if (!request.connection || request.connectionId.isEmpty() || request.database.isEmpty()
            || request.collection.isEmpty())
            return 0;
        if (request.priority == Priority::Background
            && _blocked.contains(blockKey(request.connectionId, request.database)))
            return 0;

        const quint64 ticket = ++_nextTicket;
        Lane &lane = _lanes[request.connectionId];
        const QString k = key(request.database, request.collection);

        if (lane.running && key(lane.running->request.database, lane.running->request.collection) == k
            && covers(lane.running->request, request)) {
            lane.running->tickets.push_back(ticket);
            return ticket;
        }

        auto queued = std::find_if(lane.queue.begin(), lane.queue.end(), [&k](const Job &job) {
            return key(job.request.database, job.request.collection) == k;
        });
        if (queued != lane.queue.end()) {
            merge(queued->request, request);
            queued->tickets.push_back(ticket);
        } else {
            Job job;
            job.request = request;
            job.tickets = {ticket};
            job.order = ++_nextOrder;
            lane.queue.push_back(std::move(job));
        }
        emit queueChanged(request.connectionId);
        startNext(request.connectionId);
        return ticket;
    }

    void SchemaService::setBackgroundPaused(bool paused)
    {
        if (_backgroundPaused == paused)
            return;
        _backgroundPaused = paused;
        if (!paused) {
            std::vector<QString> connections;
            for (const auto &lane : _lanes)
                connections.push_back(lane.first);
            for (const QString &connectionId : connections)
                startNext(connectionId);
        }
    }

    void SchemaService::startNext(const QString &connectionId)
    {
        auto it = _lanes.find(connectionId);
        if (it == _lanes.end())
            return;
        Lane &lane = it->second;
        if (lane.running || lane.queue.empty())
            return;

        // Highest priority first, oldest first within a priority.
        auto next = std::max_element(lane.queue.begin(), lane.queue.end(), [](const Job &a, const Job &b) {
            if (a.request.priority != b.request.priority)
                return a.request.priority < b.request.priority;
            return a.order > b.order;
        });
        if (_backgroundPaused && next->request.priority == Priority::Background)
            return;   // only background jobs are left
        lane.running = std::make_unique<Job>(std::move(*next));
        lane.queue.erase(next);

        const Request request = lane.running->request;
        std::optional<SchemaSnapshot> stored;
        if (request.onlyIfStale && _catalog)
            stored = _catalog->find(connectionId, request.database, request.collection);
        // Values are wanted but were not kept: sample regardless of age.
        const bool checkFreshness = stored && !(request.valuesIncluded && !stored->valuesIncluded);

        std::thread([self = QPointer<SchemaService>(this), connectionId, generation = lane.generation, request,
                     stored, checkFreshness, analyze = _analyze, count = _count]() {
            SchemaJobOutcome outcome;
            bool needed = true;
            if (checkFreshness) {
                const qint64 documents = count(*request.connection, request.database, request.collection);
                needed = SchemaCatalog::isStale(*stored, QDateTime::currentDateTimeUtc(), documents);
            }
            if (needed) {
                outcome.sampled = true;
                outcome.result = analyze(*request.connection, request.database, request.collection,
                                         request.sampleSize, request.maxTimeMs, request.uuidEncoding,
                                         request.timeZone);
            }
            QMetaObject::invokeMethod(self, [self, connectionId, generation, outcome]() {
                if (self)
                    self->onDone(connectionId, generation, outcome);
            }, Qt::QueuedConnection);
        }).detach();
    }

    void SchemaService::onDone(const QString &connectionId, quint64 generation, const SchemaJobOutcome &outcome)
    {
        auto it = _lanes.find(connectionId);
        if (it == _lanes.end() || !it->second.running)
            return;
        Lane &lane = it->second;
        std::unique_ptr<Job> job = std::move(lane.running);
        const Request &request = job->request;

        if (generation != lane.generation) {
            SchemaJobOutcome cancelled;
            cancelled.cancelled = true;
            for (const quint64 ticket : job->tickets)
                emit finished(ticket, cancelled);
            startNext(connectionId);
            emit queueChanged(connectionId);
            return;
        }

        if (outcome.sampled && outcome.result.ok) {
            store(request, outcome.result);
        } else if (outcome.sampled) {
            if (isAuthorizationError(outcome.result.error)) {
                // Stop background work for the database for this session; a
                // dialog the user opens still tries (Interactive).
                _blocked.insert(blockKey(connectionId, request.database));
                LOG_MSG(QStringLiteral("Schema analysis stopped for %1: %2")
                            .arg(request.database, outcome.result.error),
                        LogSeverity::Warning);
                std::vector<Job> dropped;
                auto &queue = lane.queue;
                for (auto q = queue.begin(); q != queue.end();) {
                    if (q->request.database == request.database && q->request.priority == Priority::Background) {
                        dropped.push_back(std::move(*q));
                        q = queue.erase(q);
                    } else {
                        ++q;
                    }
                }
                for (const Job &d : dropped)
                    for (const quint64 ticket : d.tickets)
                        emit finished(ticket, outcome);
            } else if (request.priority == Priority::Background && job->retries < kBackgroundRetries) {
                ++job->retries;
                job->order = ++_nextOrder;
                lane.queue.push_back(std::move(*job));
                startNext(connectionId);
                emit queueChanged(connectionId);
                return;
            } else if (request.priority == Priority::Background) {
                LOG_MSG(QStringLiteral("Schema analysis of %1.%2 failed: %3")
                            .arg(request.database, request.collection, outcome.result.error),
                        LogSeverity::Warning);
            }
        }

        for (const quint64 ticket : job->tickets)
            emit finished(ticket, outcome);
        startNext(connectionId);
        emit queueChanged(connectionId);
    }

    void SchemaService::store(const Request &request, const SchemaResult &result)
    {
        if (!_catalog)
            return;
        const auto stored = _catalog->find(request.connectionId, request.database, request.collection);
        // A smaller sample than a fresh stored one would only make the catalog worse.
        if (stored && !SchemaCatalog::isStale(*stored, QDateTime::currentDateTimeUtc())
            && stored->result.sampledDocuments > result.sampledDocuments
            && !(request.valuesIncluded && !stored->valuesIncluded))
            return;

        SchemaSnapshot snapshot;
        snapshot.database = request.database;
        snapshot.collection = request.collection;
        snapshot.serverVersion = request.serverVersion;
        snapshot.sampleSize = request.sampleSize;
        snapshot.valuesIncluded = request.valuesIncluded;
        snapshot.result = result;
        QString error;
        if (!_catalog->store(request.connectionId, snapshot, &error)) {
            LOG_MSG(QStringLiteral("Could not save the schema catalog: %1").arg(error), LogSeverity::Warning);
            return;
        }
        emit snapshotReady(request.connectionId, request.database, request.collection);
    }

    void SchemaService::cancel(const QString &connectionId)
    {
        // A reconnect may use other credentials: forget the permission stops.
        for (auto b = _blocked.begin(); b != _blocked.end();) {
            if (b->startsWith(connectionId + QLatin1Char('\n')))
                b = _blocked.erase(b);
            else
                ++b;
        }
        auto it = _lanes.find(connectionId);
        if (it == _lanes.end())
            return;
        Lane &lane = it->second;
        ++lane.generation;   // the running job finishes as cancelled
        std::vector<Job> dropped = std::move(lane.queue);
        lane.queue.clear();
        if (!lane.running)
            _lanes.erase(it);

        SchemaJobOutcome cancelled;
        cancelled.cancelled = true;
        for (const Job &job : dropped)
            for (const quint64 ticket : job.tickets)
                emit finished(ticket, cancelled);
        emit queueChanged(connectionId);
    }

    int SchemaService::pending(const QString &connectionId, const QString &database) const
    {
        auto it = _lanes.find(connectionId);
        if (it == _lanes.end())
            return 0;
        const Lane &lane = it->second;
        int n = static_cast<int>(std::count_if(lane.queue.begin(), lane.queue.end(), [&database](const Job &job) {
            return job.request.database == database;
        }));
        if (lane.running && lane.running->request.database == database)
            ++n;
        return n;
    }

    bool SchemaService::isBusy(const QString &connectionId) const
    {
        auto it = _lanes.find(connectionId);
        return it != _lanes.end() && (it->second.running || !it->second.queue.empty());
    }

    bool SchemaService::isBlocked(const QString &connectionId, const QString &database) const
    {
        return _blocked.contains(blockKey(connectionId, database));
    }
}
