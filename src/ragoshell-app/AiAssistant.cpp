#include "ragoshell-app/AiAssistant.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/utils/SchemaCatalog.h"
#include "ragoshell/core/utils/SchemaService.h"
#include "ragoshell-app/QmlBackend.h"
#include "ragoshell-app/QmlSingleton.h"

namespace RagoShell
{
    using AiQueryAssistant::Failure;
    using AiQueryAssistant::Provider;

    namespace
    {
        constexpr int kSampleSize = 1000;
        constexpr int kSampleMaxTimeMs = 30000;
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

        QString savedBaseUrl(Provider provider)
        {
            SettingsManager *s = settings();
            return s ? s->aiBaseUrl(AiQueryAssistant::providerId(provider)) : QString();
        }

        int timeoutMs(Provider provider)
        {
            SettingsManager *s = settings();
            return 1000 * (s ? s->aiTimeoutSec(AiQueryAssistant::providerId(provider))
                             : AiQueryAssistant::defaultTimeoutSec(provider));
        }

        QString savedOrEnvironmentKey(Provider provider)
        {
            if (!AiQueryAssistant::usesApiKey(provider))
                return {};
            SettingsManager *s = settings();
            const QString saved = s ? s->aiApiKey(AiQueryAssistant::providerId(provider)) : QString();
            return saved.isEmpty() ? AiQueryAssistant::apiKeyFromEnvironment(provider, savedBaseUrl(provider)) : saved;
        }

        // Never follow a redirect: the key header (and a POST body) would go
        // along to the other server (docs: T6).
        void noRedirects(QNetworkRequest &request)
        {
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        }

        // A limit on the whole request (not QNetworkRequest's transfer
        // timeout, which restarts with every byte, so a server sending slowly
        // could keep it open). The finished handler sees "timedOut" and the
        // limit in "limitSec", also when the cut happened mid-body.
        void deadline(QNetworkReply *reply, int ms)
        {
            reply->setProperty("limitSec", ms / 1000);
            auto *timer = new QTimer(reply);
            timer->setSingleShot(true);
            QObject::connect(timer, &QTimer::timeout, reply, [reply]() {
                reply->setProperty("timedOut", true);
                reply->abort();
            });
            timer->start(ms);
        }

        bool timedOut(const QNetworkReply *reply)
        {
            return reply->property("timedOut").toBool() || reply->error() == QNetworkReply::TimeoutError;
        }

        // Stops reading a response past max bytes (docs: T4); the finished
        // handler sees the "tooLarge" property.
        void limitResponse(QNetworkReply *reply, qint64 max)
        {
            QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [reply, max](qint64 received, qint64) {
                if (received > max && !reply->property("tooLarge").toBool()) {
                    reply->setProperty("tooLarge", true);
                    reply->abort();
                }
            });
        }
    }

    bool aiAllowsPrivateHttp()
    {
#if defined(Q_OS_ANDROID) || defined(Q_OS_WASM)
        return false;
#else
        return true;
#endif
    }

    QString aiUnreachableHint(Provider provider)
    {
        QStringList hints;
        if (provider == Provider::Ollama)
            hints << AiAssistant::tr("Is Ollama running? Start the Ollama app, or run \"ollama serve\" in a terminal.");
#ifdef Q_OS_WASM
        if (AiQueryAssistant::usesOpenAiApi(provider))
            hints << AiAssistant::tr("In the browser the server must also allow this page (CORS): for Ollama set "
                                     "OLLAMA_ORIGINS to this page's address; in LM Studio turn on CORS in the server "
                                     "settings.");
#endif
        return hints.join(QLatin1Char(' '));
    }

    QString aiBaseUrlErrorText(AiQueryAssistant::BaseUrlError error)
    {
        using AiQueryAssistant::BaseUrlError;
        switch (error) {
        case BaseUrlError::None:
            return {};
        case BaseUrlError::Empty:
            return AiAssistant::tr("Enter the server address.");
        case BaseUrlError::TooLong:
            return AiAssistant::tr("The address is too long.");
        case BaseUrlError::Malformed:
            return AiAssistant::tr("Enter a full address that starts with https://, for example https://api.openai.com/v1.");
        case BaseUrlError::HasCredentials:
            return AiAssistant::tr("The address must not contain a user name or password. Put the key in the key field.");
        case BaseUrlError::HasQueryOrFragment:
            return AiAssistant::tr("The address must not contain ? or #.");
        case BaseUrlError::PlainHttpPublic:
            return AiAssistant::tr("Use https://. Plain http:// is allowed only for this device (localhost) or, without a key, for the local network.");
        case BaseUrlError::PlainHttpWithKey:
            return AiAssistant::tr("A key is never sent over plain http:// on the network. Use https://, or clear the key for a local server that needs none.");
        case BaseUrlError::PlainHttpBlocked:
            return AiAssistant::tr("Plain http:// to the local network is blocked on this device. Use https:// (for example through a reverse proxy) or localhost.");
        }
        return {};
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

    QString AiAssistant::apiKey(Provider provider) const
    {
        return savedOrEnvironmentKey(provider);
    }

    AiQueryAssistant::BaseUrlCheck AiAssistant::endpointCheck(Provider provider) const
    {
        return AiQueryAssistant::checkBaseUrl(savedBaseUrl(provider), !apiKey(provider).isEmpty(),
                                              aiAllowsPrivateHttp());
    }

    bool AiAssistant::configured(Provider provider) const
    {
        if (AiQueryAssistant::hasConfigurableBaseUrl(provider))
            return endpointCheck(provider).ok() && !modelFor(provider).isEmpty();
        return !apiKey(provider).isEmpty();
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
        return s && s->aiAssistantEnabled() && configured(selectedProvider());
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

    QString AiAssistant::endpointHost() const
    {
        // The provider asked last (it may be the "instead" provider).
        const Provider p = _activeModel.isEmpty() ? selectedProvider() : _activeProvider;
        if (AiQueryAssistant::hasConfigurableBaseUrl(p)) {
            const AiQueryAssistant::BaseUrlCheck check = endpointCheck(p);
            return check.ok() ? AiQueryAssistant::urlHost(check.normalized) : QString();
        }
        return AiQueryAssistant::urlHost(AiQueryAssistant::endpointUrl(p, modelFor(p)));
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
        if (s && s->aiAssistantEnabled() && AiQueryAssistant::hasConfigurableBaseUrl(p)) {
            const AiQueryAssistant::BaseUrlCheck check = endpointCheck(p);
            QString problem;
            if (!check.ok())
                problem = aiBaseUrlErrorText(check.error);
            else if (modelFor(p).isEmpty())
                problem = tr("Pick or type a model for %1 in Preferences.").arg(AiQueryAssistant::providerName(p));
            if (!problem.isEmpty()) {
                _error = problem;
                emit stateChanged();
                return;
            }
        }
        if (!s || !s->aiAssistantEnabled() || !configured(p)) {
            _error = AiQueryAssistant::usesApiKey(p)
                         ? tr("Turn on the AI assistant and add an API key for %1 in Preferences.")
                               .arg(AiQueryAssistant::providerName(p))
                         : tr("Turn on the AI assistant and set up %1 in Preferences.")
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
        QString base;
        if (AiQueryAssistant::hasConfigurableBaseUrl(_activeProvider)) {
            // Checked again right before sending: the settings may have
            // been edited by hand.
            const AiQueryAssistant::BaseUrlCheck check = endpointCheck(_activeProvider);
            if (!check.ok()) {
                fail(aiBaseUrlErrorText(check.error), false);
                return;
            }
            base = check.normalized;
        }
        QNetworkRequest request{QUrl(AiQueryAssistant::endpointUrl(_activeProvider, model, base))};
        for (const auto &[name, value] : AiQueryAssistant::requestHeaders(_activeProvider, key, model))
            request.setRawHeader(name, value);
        noRedirects(request);

        if (!key.isEmpty() && AiQueryAssistant::checkPastedKey(key, nullptr) != AiQueryAssistant::PastedKey::Ok) {
            fail(tr("The saved API key has spaces or characters an API key does not contain. Enter it again in Preferences."),
                 false);
            return;
        }

        _activeModel = model;
        _tried << model;
        emit readyChanged();   // endpointHost follows the provider asked
        _reply = _network->post(request, AiQueryAssistant::requestBody(_activeProvider, model, _context, _question));
        deadline(_reply, timeoutMs(_activeProvider));
        limitResponse(_reply, AiQueryAssistant::kMaxResponseBytes);
        connect(_reply, &QNetworkReply::finished, this, [this, reply = _reply]() {
            if (reply)
                onFinished(reply);
        });
        LOG_MSG(QStringLiteral("AI assistant: asking %1 %2 at %3 about %4.%5")
                    .arg(AiQueryAssistant::providerName(_activeProvider), model, request.url().host(), _database,
                         _collection),
                LogSeverity::Info);
        emit stateChanged();
    }

    void AiAssistant::fail(const QString &error, bool retryable)
    {
        _error = error;
        _retryable = retryable;
        LOG_MSG(QStringLiteral("AI assistant: %1").arg(AiQueryAssistant::redactSecrets(_error)), LogSeverity::Warning);
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
            if (p == _activeProvider || !configured(p))
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

        if (reply->property("tooLarge").toBool()) {
            fail(tr("The server sent a response that is too large (over %1 MB).")
                     .arg(AiQueryAssistant::kMaxResponseBytes / (1024 * 1024)),
                 false);
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // Checked before the status: the limit may cut a response whose
        // headers already arrived.
        if (status == 0 || timedOut(reply)) {
            QString error;
            if (timedOut(reply)) {
                const int seconds = reply->property("limitSec").toInt();
                error = AiQueryAssistant::hasConfigurableBaseUrl(_activeProvider)
                            //: %1 = seconds. Local models can take long, especially while loading.
                            ? tr("No answer within %1 seconds. A local model can be slow, especially while it "
                                 "loads; you can raise the time limit in Preferences.").arg(seconds)
                            : tr("The request timed out.");
            } else {
                error = tr("Cannot reach the API: %1").arg(reply->errorString());
                const QString hint = aiUnreachableHint(_activeProvider);
                if (!hint.isEmpty())
                    error += QLatin1Char(' ') + hint;
            }
            fail(error, true);
            return;
        }

        // A join may read only the collections that were described.
        QStringList known{_collection};
        for (const AiQueryAssistant::RelatedCollection &r : _context.related)
            known << r.name;
        const AiQueryAssistant::Suggestion s = AiQueryAssistant::parseResponse(
            _activeProvider, status, reply->readAll(), _database, reply->rawHeader("retry-after"), known);
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
                            .arg(_activeModel, AiQueryAssistant::redactSecrets(s.error)), LogSeverity::Warning);
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

    void AiModelCatalog::clear()
    {
        if (_reply) {
            _reply->disconnect(this);
            _reply->abort();
            _reply->deleteLater();
            _reply = nullptr;
        }
        _error.clear();
        _result.clear();
        emit stateChanged();
    }

    void AiModelCatalog::refresh(const QString &providerId, const QString &apiKey, const QString &baseUrl)
    {
        if (_reply)
            return;
        const Provider provider = AiQueryAssistant::providerFromId(providerId);
        const bool configurable = AiQueryAssistant::hasConfigurableBaseUrl(provider);
        _error.clear();
        _result.clear();

        QString base;
        if (configurable) {
            // The environment key needs the address first (preset only).
            const AiQueryAssistant::BaseUrlCheck plain = AiQueryAssistant::checkBaseUrl(baseUrl, false, aiAllowsPrivateHttp());
            if (!plain.ok()) {
                _error = aiBaseUrlErrorText(plain.error);
                emit stateChanged();
                return;
            }
            base = plain.normalized;
        }
        const QString key = !AiQueryAssistant::usesApiKey(provider) ? QString()
                            : apiKey.trimmed().isEmpty() ? AiQueryAssistant::apiKeyFromEnvironment(provider, base)
                                                         : apiKey.trimmed();
        if (key.isEmpty() && !configurable) {
            _error = tr("Enter an API key first.");
            emit stateChanged();
            return;
        }
        // A typed key gets the same check as a pasted one before it goes
        // into a header.
        if (!key.isEmpty() && AiQueryAssistant::checkPastedKey(key, nullptr) != AiQueryAssistant::PastedKey::Ok) {
            _error = tr("The key has spaces or characters an API key does not contain.");
            emit stateChanged();
            return;
        }
        if (configurable && !key.isEmpty()) {
            const AiQueryAssistant::BaseUrlCheck withKey = AiQueryAssistant::checkBaseUrl(base, true, aiAllowsPrivateHttp());
            if (!withKey.ok()) {
                _error = aiBaseUrlErrorText(withKey.error);
                emit stateChanged();
                return;
            }
        }

        QNetworkRequest request{QUrl(AiQueryAssistant::modelsUrl(provider, base))};
        for (const auto &[name, value] : AiQueryAssistant::requestHeaders(provider, key, QString()))
            request.setRawHeader(name, value);
        noRedirects(request);
        _reply = _network->get(request);
        deadline(_reply, kModelListTimeoutMs);
        limitResponse(_reply, AiQueryAssistant::kMaxModelListBytes);
        connect(_reply, &QNetworkReply::finished, this, [this, reply = _reply, provider]() {
            if (!reply)
                return;
            reply->deleteLater();
            if (reply != _reply)
                return;
            _reply = nullptr;
            if (reply->property("tooLarge").toBool()) {
                _error = tr("The model list is too large (over %1 MB).")
                             .arg(AiQueryAssistant::kMaxModelListBytes / (1024 * 1024));
                LOG_MSG(QStringLiteral("AI assistant: connection test: %1").arg(AiQueryAssistant::redactSecrets(_error)), LogSeverity::Warning);
                emit stateChanged();
                return;
            }
            // A cut-off list reads as "not reachable in time", not as a bad list.
            const int status = timedOut(reply) ? 0 : reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const AiQueryAssistant::ConnectionCheck check =
                AiQueryAssistant::checkConnection(provider, status, status > 0 ? reply->readAll() : QByteArray());
            using AiQueryAssistant::ConnectionStatus;
            switch (check.status) {
            case ConnectionStatus::Connected: {
                const QString id = AiQueryAssistant::providerId(provider);
                if (SettingsManager *s = settings())
                    s->setAiAvailableModels(id, check.list.models);
                //: Preferences › AI › Test connection succeeded.
                _result = tr("Connected. %n model(s) available.", nullptr, int(check.list.models.size()));
                LOG_MSG(QStringLiteral("AI assistant: %1 lists %2 model(s).")
                            .arg(AiQueryAssistant::providerName(provider)).arg(check.list.models.size()),
                        LogSeverity::Info);
                emit stateChanged();
                emit refreshed(id, check.list.models);
                return;
            }
            case ConnectionStatus::NoUsableModels:
                _error = tr("Connected, but the provider listed no model this app can use.");
                break;
            case ConnectionStatus::NoModelsInstalled:
                _error = provider == Provider::Ollama
                             //: Preferences › AI › Test connection with Ollama. Keep "ollama pull <model>" as is.
                             ? tr("Ollama is running, but no model is installed. Download one in a terminal with "
                                  "\"ollama pull <model>\", then test again.")
                             : tr("The server is running but lists no models. Download or load a model on the "
                                  "server, then test again.");
                break;
            case ConnectionStatus::KeyRejected:
                _error = tr("The API key was rejected (HTTP %1). Check it and try again.").arg(status);
                break;
            case ConnectionStatus::NoModelList:
                _error = tr("The server is reachable but has no model list (HTTP 404).");
                break;
            case ConnectionStatus::RateLimited:
                _error = tr("The provider's rate limit was reached (HTTP 429). Try again in a moment.");
                break;
            case ConnectionStatus::RedirectRefused:
                _error = tr("The server answered with a redirect (HTTP %1). It was not followed, so the key "
                            "was not sent anywhere else.").arg(status);
                break;
            case ConnectionStatus::ProviderError:
                _error = check.list.error;
                break;
            case ConnectionStatus::Unreachable:
                if (timedOut(reply)) {
                    _error = tr("The server did not answer in time.");
                } else {
                    _error = tr("Cannot reach the server: %1").arg(reply->errorString());
                    const QString hint = aiUnreachableHint(provider);
                    if (!hint.isEmpty())
                        _error += QLatin1Char(' ') + hint;
                }
                break;
            }
            LOG_MSG(QStringLiteral("AI assistant: connection test: %1").arg(AiQueryAssistant::redactSecrets(_error)), LogSeverity::Warning);
            emit stateChanged();
        });
        emit stateChanged();
    }
}
