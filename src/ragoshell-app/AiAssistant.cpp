#include "ragoshell-app/AiAssistant.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/utils/SchemaCatalog.h"
#include "ragoshell/core/utils/SchemaService.h"
#include "ragoshell-app/QmlBackend.h"
#include "ragoshell-app/QmlSingleton.h"
#include "ragoshell-app/CloudService.h"

namespace RagoShell
{
    using AiQueryAssistant::Failure;
    using AiQueryAssistant::Provider;

    namespace
    {
        constexpr int kSampleSize = 1000;
        constexpr int kSampleMaxTimeMs = 30000;
        constexpr int kRequestTimeoutMs = 120000;
        constexpr int kModelListTimeoutMs = 30000;
        constexpr int kMaxModelsPerRequest = 3;
        // Longer waits are not offered as a Retry countdown.
        constexpr qint64 kMaxRetryWaitSec = 30 * 60;

        SettingsManager *settings()
        {
            return AppRegistry::instance().settingsManager();
        }

        // Shared by every assistant dialog, for the whole session.
        AiQueryAssistant::ModelCooldowns &cooldowns()
        {
            static AiQueryAssistant::ModelCooldowns instance;
            return instance;
        }

        bool triesAnotherModel(Failure failure)
        {
            return failure == Failure::QuotaExhausted || failure == Failure::RateLimited
                   || failure == Failure::Overloaded || failure == Failure::ModelUnavailable;
        }

        QString savedOrEnvironmentKey(Provider provider)
        {
            SettingsManager *s = settings();
            const QString saved = s ? s->aiApiKey(AiQueryAssistant::providerId(provider)) : QString();
            return saved.isEmpty() ? AiQueryAssistant::apiKeyFromEnvironment(provider) : saved;
        }
    }

    AiAssistant::AiAssistant(QObject *parent)
        : QObject(parent),
          _network(new QNetworkAccessManager(this))
    {
        if (SchemaService *service = SchemaService::instance()) {
            connect(service, &SchemaService::finished, this, &AiAssistant::onJobFinished);
            connect(service, &SchemaService::snapshotReady, this, &AiAssistant::onSnapshotReady);
        }
    }

    AiAssistant::~AiAssistant()
    {
        if (_reply) {
            _reply->disconnect(this);
            _reply->abort();
        }
    }

    CloudClient *AiAssistant::cloudClient() const
    {
        CloudService *svc = QmlSingletonInstance<CloudService>::peek();
        return svc ? svc->client() : nullptr;
    }

    QString AiAssistant::apiKey(Provider provider) const
    {
        // Hosted mode has no user key: the "key" is a live account session.
        if (provider == Provider::RagoCloud) {
            const CloudClient *c = cloudClient();
            return (c && c->isSignedIn()) ? QStringLiteral("cloud") : QString();
        }
        return savedOrEnvironmentKey(provider);
    }

    Provider AiAssistant::selectedProvider() const
    {
        SettingsManager *s = settings();
        return AiQueryAssistant::providerFromId(s ? s->aiProvider() : QString());
    }

    QString AiAssistant::modelFor(Provider provider) const
    {
        SettingsManager *s = settings();
        const QString m = s ? s->aiModel(AiQueryAssistant::providerId(provider)) : QString();
        return m.isEmpty() ? AiQueryAssistant::defaultModel(provider) : m;
    }

    bool AiAssistant::ready() const
    {
        SettingsManager *s = settings();
        return s && s->aiAssistantEnabled() && !apiKey(selectedProvider()).isEmpty();
    }

    QString AiAssistant::provider() const
    {
        return AiQueryAssistant::providerId(selectedProvider());
    }

    QString AiAssistant::providerName() const
    {
        return AiQueryAssistant::providerName(selectedProvider());
    }

    QString AiAssistant::model() const
    {
        return modelFor(selectedProvider());
    }

    QString AiAssistant::alternativeProviderName() const
    {
        return _alternative.isEmpty() ? QString()
                                      : AiQueryAssistant::providerName(AiQueryAssistant::providerFromId(_alternative));
    }

    bool AiAssistant::settingsError() const
    {
        if (_error.isEmpty())
            return false;
        const Failure f = _suggestion.failure;
        return !ready() || f == Failure::Auth || f == Failure::ModelUnavailable || f == Failure::QuotaExhausted;
    }

    QString AiAssistant::schemaText() const
    {
        return _schemaReady ? AiQueryAssistant::schemaSummary(_context) : QString();
    }

    bool AiAssistant::configure(const QString &serverUuid, const QString &database,
                                const QString &collection)
    {
        QmlBackend *backend = QmlSingletonInstance<QmlBackend>::peek();
        std::unique_ptr<ConnectionSettings> conn = backend ? backend->cloneConnection(serverUuid) : nullptr;
        if (!conn || database.isEmpty() || collection.isEmpty())
            return false;
        cancel();
        _connection = std::move(conn);
        _serverUuid = serverUuid;
        _database = database;
        _collection = collection;
        _context = AiQueryAssistant::Context();
        _context.database = database;
        _context.collection = collection;
        _context.serverVersion = backend->serverVersionString(serverUuid);
        _schemaReady = false;
        _schemaHasValues = false;
        _sampling = false;
        _sampleTicket = 0;   // results for the previous collection are dropped
        _refreshTicket = 0;
        _question.clear();
        _error.clear();
        _retryable = false;
        _retryAtMs = 0;
        _notices.clear();
        _alternative.clear();
        _activeModel.clear();
        _suggestion = AiQueryAssistant::Suggestion();
        emit targetChanged();
        refreshSettings();
        if (!useCatalog())
            sample();
        return true;
    }

    void AiAssistant::refreshSettings()
    {
        SettingsManager *s = settings();
        _context.includeValues = s && s->aiIncludeSampleValues();
        // Values were just allowed, but the stored schema was kept without them.
        if (_context.includeValues && _schemaReady && !_schemaHasValues && !_sampling)
            sample();
        emit readyChanged();
        emit stateChanged();
    }

    quint64 AiAssistant::enqueueSample(bool interactive)
    {
        SchemaService *service = SchemaService::instance();
        if (!_connection || !service)
            return 0;
        SettingsManager *s = settings();
        SchemaService::Request request;
        request.connectionId = _serverUuid;
        request.connection = _connection;
        request.database = _database;
        request.collection = _collection;
        request.priority = interactive ? SchemaService::Priority::Interactive : SchemaService::Priority::Hint;
        request.sampleSize = kSampleSize;
        request.maxTimeMs = kSampleMaxTimeMs;
        request.onlyIfStale = !interactive;   // a refresh samples only a stale snapshot
        // Values reach the disk only when the user allows sending them.
        request.valuesIncluded = s && s->aiIncludeSampleValues();
        request.serverVersion = _context.serverVersion;
        request.uuidEncoding = s ? s->uuidEncoding() : DefaultEncoding;
        request.timeZone = s ? s->timeZone() : Utc;
        return service->enqueue(request);
    }

    void AiAssistant::sample()
    {
        // Closing the dialog never waits for the sample.
        _sampleTicket = enqueueSample(/*interactive=*/true);
        _sampling = _sampleTicket != 0;
        emit stateChanged();
    }

    void AiAssistant::onJobFinished(quint64 ticket, const SchemaJobOutcome &outcome)
    {
        if (ticket != 0 && ticket == _sampleTicket) {
            _sampleTicket = 0;
            if (outcome.cancelled) {
                _sampling = false;
                _pendingQuestion.clear();
                emit stateChanged();
                return;
            }
            applySample(outcome.result);
        } else if (ticket != 0 && ticket == _refreshTicket) {
            _refreshTicket = 0;
            if (outcome.sampled)
                applySample(outcome.result, /*quiet=*/true);
        }
    }

    void AiAssistant::applySample(const SchemaResult &result, bool quiet)
    {
        if (quiet) {
            if (result.ok) {
                setSchema(result);
                emit stateChanged();
            }
            return;
        }
        _sampling = false;
        if (!result.ok) {
            _error = tr("Could not sample the collection: %1").arg(result.error);
            _pendingQuestion.clear();
            emit stateChanged();
            return;
        }
        setSchema(result);
        emit stateChanged();
        if (!_pendingQuestion.isEmpty())
            start(_pendingQuestion, _activeProvider);
    }

    void AiAssistant::setSchema(const SchemaResult &result)
    {
        _context = AiQueryAssistant::contextFrom(result, _database, _collection, _context.serverVersion,
                                                 _context.includeValues);
        _schemaReady = true;
        _schemaHasValues = true;   // what is in use now came straight from the database
        refreshRelated();
    }

    void AiAssistant::refreshRelated()
    {
        SchemaCatalog *catalog = SchemaCatalog::instance();
        std::map<QString, SchemaResult> others;
        if (catalog) {
            for (const QString &name : catalog->analyzedCollections(_serverUuid, _database)) {
                if (name == _collection)
                    continue;
                if (const auto snapshot = catalog->find(_serverUuid, _database, name))
                    others.emplace(name, snapshot->result);
            }
        }
        _context.related = AiQueryAssistant::relatedCollections(_collection, _context.fields, _question, others);
    }

    int AiAssistant::analyzedCollections() const
    {
        SchemaCatalog *catalog = SchemaCatalog::instance();
        return catalog && !_database.isEmpty()
                   ? static_cast<int>(catalog->analyzedCollections(_serverUuid, _database).size()) : 0;
    }

    int AiAssistant::databaseCollections() const
    {
        QmlBackend *backend = QmlSingletonInstance<QmlBackend>::peek();
        return backend && !_database.isEmpty() ? backend->schemaProgress(_serverUuid, _database).total : -1;
    }

    void AiAssistant::analyzeDatabase()
    {
        if (QmlBackend *backend = QmlSingletonInstance<QmlBackend>::peek())
            backend->analyzeDatabaseSchema(_serverUuid, _database);
    }

    void AiAssistant::onSnapshotReady(const QString &connectionId, const QString &database, const QString &collection)
    {
        // Another collection of this database: it may now be described.
        if (connectionId != _serverUuid || database != _database || collection == _collection || !_schemaReady)
            return;
        refreshRelated();
        emit stateChanged();
    }

    bool AiAssistant::useCatalog()
    {
        SchemaCatalog *catalog = SchemaCatalog::instance();
        if (!catalog)
            return false;
        const std::optional<SchemaSnapshot> snapshot = catalog->find(_serverUuid, _database, _collection);
        // Values are wanted but were not kept: sample again.
        if (!snapshot || (_context.includeValues && !snapshot->valuesIncluded))
            return false;
        setSchema(snapshot->result);
        _schemaHasValues = snapshot->valuesIncluded;
        emit stateChanged();
        // Sampled again only when the stored snapshot turns out to be stale.
        _refreshTicket = enqueueSample(/*interactive=*/false);
        return true;
    }

    void AiAssistant::ask(const QString &question)
    {
        askWithProvider(question, provider());
    }

    void AiAssistant::retry(const QString &question)
    {
        const Provider p = selectedProvider();
        SettingsManager *s = settings();
        const QStringList available = s ? s->aiAvailableModels(AiQueryAssistant::providerId(p)) : QStringList();
        for (const QString &m : AiQueryAssistant::fallbackCandidates(p, modelFor(p), available, kMaxModelsPerRequest))
            cooldowns().clear(p, m);
        ask(question);
    }

    void AiAssistant::askWithProvider(const QString &question, const QString &providerId)
    {
        if (question.trimmed().isEmpty() || _reply)
            return;
        refreshSettings();
        const Provider p = AiQueryAssistant::providerFromId(providerId);
        _error.clear();
        _retryable = false;
        _retryAtMs = 0;
        _notices.clear();
        _alternative.clear();
        _activeModel.clear();
        _suggestion = AiQueryAssistant::Suggestion();
        SettingsManager *s = settings();
        if (!s || !s->aiAssistantEnabled() || apiKey(p).isEmpty()) {
            _error = tr("Turn on the AI assistant and add an API key for %1 in Preferences.")
                         .arg(AiQueryAssistant::providerName(p));
            emit stateChanged();
            return;
        }
        _activeProvider = p;
        _pendingQuestion = question;
        if (!_schemaReady) {
            if (!_sampling)
                sample();
            emit stateChanged();
            return;
        }
        start(question, p);
    }

    void AiAssistant::start(const QString &question, Provider provider)
    {
        _pendingQuestion.clear();
        _question = question;
        _activeProvider = provider;
        refreshRelated();   // ranked for this question
        SettingsManager *s = settings();
        const QStringList available = s ? s->aiAvailableModels(AiQueryAssistant::providerId(provider))
                                        : QStringList();
        const bool fallback = !s || s->aiAutoFallback();
        _candidates = AiQueryAssistant::fallbackCandidates(provider, modelFor(provider), available,
                                                           fallback ? kMaxModelsPerRequest : 1);
        _tried.clear();
        send();
    }

    void AiAssistant::send()
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QString model;
        for (const QString &m : _candidates) {
            if (_tried.contains(m))
                continue;
            const auto blocked = cooldowns().blocked(_activeProvider, m, now);
            if (!blocked.until.isValid()) {
                model = m;
                break;
            }
            _tried << m;
            AiQueryAssistant::Suggestion previous;
            previous.failure = blocked.failure;
            _notices << tr("%1 skipped: %2.").arg(m, AiQueryAssistant::failureSummary(previous, blocked.until));
        }

        if (model.isEmpty()) {
            // Every model is paused by earlier errors in this session.
            QDateTime earliest;
            for (const QString &m : _candidates) {
                const auto blocked = cooldowns().blocked(_activeProvider, m, now);
                if (blocked.until.isValid() && (!earliest.isValid() || blocked.until < earliest))
                    earliest = blocked.until;
            }
            _retryAtMs = earliest.isValid() && now.secsTo(earliest) <= kMaxRetryWaitSec
                             ? earliest.toMSecsSinceEpoch() : 0;
            _alternative = findAlternative();
            fail(tr("No %1 model is available right now: they ran out of quota or hit a rate limit "
                    "earlier. Retry forgets that and asks again.")
                     .arg(AiQueryAssistant::providerName(_activeProvider)), true);
            return;
        }

        const QString key = apiKey(_activeProvider);
        if (_activeProvider == Provider::RagoCloud) {
            // Hosted path: bearer token instead of a stored API key; the
            // proxy chooses provider+model ("ragoshell-ai" is display-only).
            CloudClient *cloud = cloudClient();
            _activeModel = model;
            _tried << model;
            emit stateChanged();
            cloud->withValidAccessToken([this, cloud](bool ok, const QString &token, const QString &) {
                if (!ok) {
                    fail(tr("The RagoShell session expired; sign in again."), false);
                    return;
                }
                QNetworkRequest request{QUrl(cloud->effectiveApiUrl()
                                            + QStringLiteral("/v1/ai/generate"))};
                request.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
                request.setRawHeader("Content-Type", "application/json");
                request.setRawHeader("Accept", "application/json");
                request.setTransferTimeout(kRequestTimeoutMs);
                _reply = _network->post(request, AiQueryAssistant::cloudRequestBody(_context, _question));
                connect(_reply, &QNetworkReply::finished, this, [this, reply = _reply]() {
                    if (reply)
                        onFinished(reply);
                });
                emit stateChanged();
            });
            LOG_MSG(QStringLiteral("AI assistant: asking %1 %2 about %3.%4")
                        .arg(AiQueryAssistant::providerName(_activeProvider), model, _database, _collection),
                    LogSeverity::Info);
            return;
        }
        QNetworkRequest request{QUrl(AiQueryAssistant::endpointUrl(_activeProvider, model))};
        for (const auto &[name, value] : AiQueryAssistant::requestHeaders(_activeProvider, key, model))
            request.setRawHeader(name, value);
        request.setTransferTimeout(kRequestTimeoutMs);

        _activeModel = model;
        _tried << model;
        _reply = _network->post(request, AiQueryAssistant::requestBody(_activeProvider, model, _context, _question));
        connect(_reply, &QNetworkReply::finished, this, [this, reply = _reply]() {
            if (reply)
                onFinished(reply);
        });
        LOG_MSG(QStringLiteral("AI assistant: asking %1 %2 about %3.%4")
                    .arg(AiQueryAssistant::providerName(_activeProvider), model, _database, _collection),
                LogSeverity::Info);
        emit stateChanged();
    }

    void AiAssistant::fail(const QString &error, bool retryable)
    {
        _error = error;
        _retryable = retryable;
        LOG_MSG(QStringLiteral("AI assistant: %1").arg(_error), LogSeverity::Warning);
        emit stateChanged();
    }

    QString AiAssistant::findAlternative() const
    {
        SettingsManager *s = settings();
        if (!s)
            return {};
        const QDateTime now = QDateTime::currentDateTimeUtc();
        for (const QString &id : AiQueryAssistant::providerIds()) {
            const Provider p = AiQueryAssistant::providerFromId(id);
            if (p == _activeProvider || apiKey(p).isEmpty())
                continue;
            const QStringList candidates = AiQueryAssistant::fallbackCandidates(
                p, modelFor(p), s->aiAvailableModels(id), kMaxModelsPerRequest);
            if (!cooldowns().pick(p, candidates, {}, now).isEmpty())
                return id;
        }
        return {};
    }

    void AiAssistant::onFinished(QNetworkReply *reply)
    {
        reply->deleteLater();
        if (reply != _reply)
            return;   // cancelled
        _reply = nullptr;

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 0) {
            fail(reply->error() == QNetworkReply::OperationCanceledError
                     ? tr("The request timed out.")
                     : tr("Cannot reach the API: %1").arg(reply->errorString()),
                 true);
            return;
        }

        // A join may read only the collections that were described.
        QStringList known{_collection};
        for (const AiQueryAssistant::RelatedCollection &r : _context.related)
            known << r.name;
        const AiQueryAssistant::Suggestion s =
            _activeProvider == Provider::RagoCloud
                ? AiQueryAssistant::parseCloudResponse(status, reply->readAll(), _database,
                                                       reply->rawHeader("retry-after"), known)
                : AiQueryAssistant::parseResponse(
                      _activeProvider, status, reply->readAll(), _database,
                      reply->rawHeader("retry-after"), known);
        if (s.ok) {
            _suggestion = s;
            _error.clear();
            _retryable = false;
            if (!_candidates.isEmpty() && _activeModel != _candidates.first())
                _notices << tr("Answered by %1 instead of %2.").arg(_activeModel, _candidates.first());
            emit stateChanged();
            return;
        }

        const QDateTime now = QDateTime::currentDateTimeUtc();
        if (triesAnotherModel(s.failure)) {
            const QDateTime until = cooldowns().record(_activeProvider, _activeModel, s, now);
            const bool more = !s.allModels && !cooldowns().pick(_activeProvider, _candidates, _tried, now).isEmpty();
            if (more) {
                _notices << tr("%1: %2.").arg(_activeModel, AiQueryAssistant::failureSummary(s, until));
                LOG_MSG(QStringLiteral("AI assistant: %1 failed (%2), trying the next model.")
                            .arg(_activeModel, s.error), LogSeverity::Warning);
                send();
                return;
            }
            if (s.failure == Failure::RateLimited || s.failure == Failure::Overloaded) {
                if (until.isValid() && now.secsTo(until) <= kMaxRetryWaitSec)
                    _retryAtMs = until.toMSecsSinceEpoch();
            }
            if (s.failure == Failure::QuotaExhausted || s.failure == Failure::RateLimited
                || s.allModels || _tried.size() > 1)
                _alternative = findAlternative();
        }
        _suggestion = s;
        fail(s.error, s.retryable);
    }

    void AiAssistant::cancel()
    {
        _pendingQuestion.clear();
        if (_reply) {
            QNetworkReply *reply = _reply;
            _reply = nullptr;
            reply->abort();
        }
        emit stateChanged();
    }

    AiModelCatalog::AiModelCatalog(QObject *parent)
        : QObject(parent),
          _network(new QNetworkAccessManager(this))
    {
    }

    AiModelCatalog::~AiModelCatalog()
    {
        if (_reply) {
            _reply->disconnect(this);
            _reply->abort();
        }
    }

    bool AiModelCatalog::busy() const
    {
        return !_reply.isNull();
    }

    void AiModelCatalog::refresh(const QString &providerId, const QString &apiKey)
    {
        if (_reply)
            return;
        const Provider provider = AiQueryAssistant::providerFromId(providerId);
        const QString key = apiKey.trimmed().isEmpty() ? AiQueryAssistant::apiKeyFromEnvironment(provider)
                                                       : apiKey.trimmed();
        _error.clear();
        if (key.isEmpty()) {
            _error = tr("Enter an API key first.");
            emit stateChanged();
            return;
        }

        QNetworkRequest request{QUrl(AiQueryAssistant::modelsUrl(provider))};
        for (const auto &[name, value] : AiQueryAssistant::requestHeaders(provider, key, QString()))
            request.setRawHeader(name, value);
        request.setTransferTimeout(kModelListTimeoutMs);
        _reply = _network->get(request);
        connect(_reply, &QNetworkReply::finished, this, [this, reply = _reply, provider]() {
            if (!reply)
                return;
            reply->deleteLater();
            if (reply != _reply)
                return;
            _reply = nullptr;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 0) {
                _error = tr("Cannot reach the API: %1").arg(reply->errorString());
            } else {
                const AiQueryAssistant::ModelList list = AiQueryAssistant::parseModelList(provider, status, reply->readAll());
                if (list.ok) {
                    const QString id = AiQueryAssistant::providerId(provider);
                    if (SettingsManager *s = settings())
                        s->setAiAvailableModels(id, list.models);
                    LOG_MSG(QStringLiteral("AI assistant: %1 lists %2 model(s).")
                                .arg(AiQueryAssistant::providerName(provider)).arg(list.models.size()),
                            LogSeverity::Info);
                    emit stateChanged();
                    emit refreshed(id, list.models);
                    return;
                }
                _error = list.error;
            }
            LOG_MSG(QStringLiteral("AI assistant: model list: %1").arg(_error), LogSeverity::Warning);
            emit stateChanged();
        });
        emit stateChanged();
    }
}
