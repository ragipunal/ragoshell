#include "ragoshell/core/utils/AiQueryAssistantInternal.h"

#include <algorithm>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>

#include "ragoshell/core/utils/AppPaths.h"
#include "ragoshell/core/utils/QueryCodeExport.h"

namespace RagoShell::AiQueryAssistant
{
    namespace detail
    {
            // Refusals have no server-side fallback on Haiku.
            bool supportsFallback(const QString &model)
            {
                return !model.startsWith(QLatin1String("claude-haiku"));
            }

            QString baseUrl(Provider provider)
            {
                QString url = productEnv("AI_BASE_URL");
                if (url.isEmpty())
                    return provider == Provider::Gemini
                               ? QStringLiteral("https://generativelanguage.googleapis.com")
                               : QStringLiteral("https://api.anthropic.com");
                while (url.endsWith('/'))
                    url.chop(1);
                return url;
            }

            QJsonObject outputSchema()
            {
                QJsonObject kind{{"type", "string"},
                                 {"enum", QJsonArray{"find", "aggregate", "unsupported"}}};
                QJsonObject props{{"kind", kind},
                                  {"script", QJsonObject{{"type", "string"}}},
                                  {"explanation", QJsonObject{{"type", "string"}}}};
                return QJsonObject{{"type", "object"},
                                   {"properties", props},
                                   {"required", QJsonArray{"kind", "script", "explanation"}},
                                   {"additionalProperties", false}};
            }

            // Cloud requests go through /v1/ai/generate instead.
            // Gemini's responseSchema is an OpenAPI subset: upper-case types, no
            // additionalProperties.
            QJsonObject geminiOutputSchema()
            {
                QJsonObject kind{{"type", "STRING"},
                                 {"enum", QJsonArray{"find", "aggregate", "unsupported"}}};
                QJsonObject props{{"kind", kind},
                                  {"script", QJsonObject{{"type", "STRING"}}},
                                  {"explanation", QJsonObject{{"type", "STRING"}}}};
                return QJsonObject{{"type", "OBJECT"},
                                   {"properties", props},
                                   {"required", QJsonArray{"kind", "script", "explanation"}},
                                   {"propertyOrdering", QJsonArray{"kind", "script", "explanation"}}};
            }

    }

    using namespace detail;

    namespace
    {
            int parseRetryAfter(const QByteArray &header)
            {
                bool ok = false;
                const int sec = header.trimmed().toInt(&ok);
                return ok && sec > 0 ? sec : 0;
            }

            // "37s", "0.5s" (google.protobuf.Duration in JSON).
            int parseDuration(const QString &text)
            {
                QString t = text.trimmed();
                if (!t.endsWith('s'))
                    return 0;
                t.chop(1);
                bool ok = false;
                const double sec = t.toDouble(&ok);
                return ok && sec > 0 ? static_cast<int>(std::ceil(sec)) : 0;
            }

            void anthropicError(Suggestion &s, int status, const QJsonObject &error,
                                const QByteArray &retryAfter)
            {
                const QString type = error.value("type").toString();
                const QString message = error.value("message").toString();
                QString hint;
                if (status == 401 || type == QLatin1String("authentication_error")) {
                    s.failure = Failure::Auth;
                    hint = QCoreApplication::translate("AiQueryAssistant", "The API key was rejected. Check it in Preferences.");
                } else if (status == 402 || type == QLatin1String("billing_error")
                           || message.contains(QLatin1String("credit balance"), Qt::CaseInsensitive)) {
                    s.failure = Failure::QuotaExhausted;
                    s.allModels = true;
                    hint = QStringLiteral("The Anthropic credit balance is used up. Add credits in the "
                                          "Anthropic Console, or switch to another provider in Preferences.");
                } else if (status == 403 || type == QLatin1String("permission_error")) {
                    s.failure = Failure::ModelUnavailable;
                    hint = QCoreApplication::translate("AiQueryAssistant", "This API key is not allowed to use the selected model.");
                } else if (status == 404 || type == QLatin1String("not_found_error")) {
                    s.failure = Failure::ModelUnavailable;
                    hint = QStringLiteral("The selected model is not available for this API key (it may "
                                          "have been retired). Refresh the model list in Preferences.");
                } else if (status == 429 || type == QLatin1String("rate_limit_error")) {
                    s.failure = Failure::RateLimited;
                    s.retryAfterSec = parseRetryAfter(retryAfter);
                    hint = s.retryAfterSec > 0
                               ? QCoreApplication::translate("AiQueryAssistant", "Rate limit reached. Try again in %1 s.").arg(s.retryAfterSec)
                               : QCoreApplication::translate("AiQueryAssistant", "Rate limit reached. Try again in a moment.");
                } else if (status == 529 || type == QLatin1String("overloaded_error")) {
                    s.failure = Failure::Overloaded;
                    hint = QCoreApplication::translate("AiQueryAssistant", "The API is temporarily overloaded. Try again in a moment.");
                } else if (status >= 500) {
                    s.failure = Failure::Overloaded;
                    hint = QCoreApplication::translate("AiQueryAssistant", "The API reported a server error. Try again in a moment.");
                } else {
                    s.failure = Failure::Other;
                }
                s.error = hint.isEmpty() ? QCoreApplication::translate("AiQueryAssistant", "Request failed (HTTP %1).").arg(status) : hint;
                if (!message.isEmpty())
                    s.error += QStringLiteral(" (%1)").arg(message);
            }

            // https://ai.google.dev/gemini-api/docs/troubleshooting: google.rpc.Status
            // with ErrorInfo / QuotaFailure / RetryInfo details.
            void geminiError(Suggestion &s, int status, const QJsonObject &error)
            {
                const QString state = error.value("status").toString();
                const QString message = error.value("message").toString();

                QString reason;
                QStringList quotaIds;
                bool quotaPerModel = false;
                for (const QJsonValue &v : error.value("details").toArray()) {
                    const QJsonObject d = v.toObject();
                    const QString type = d.value("@type").toString();
                    if (type.endsWith(QLatin1String("ErrorInfo")))
                        reason = d.value("reason").toString();
                    else if (type.endsWith(QLatin1String("RetryInfo")))
                        s.retryAfterSec = parseDuration(d.value("retryDelay").toString());
                    else if (type.endsWith(QLatin1String("QuotaFailure"))) {
                        for (const QJsonValue &q : d.value("violations").toArray()) {
                            const QJsonObject violation = q.toObject();
                            const QString id = violation.value("quotaId").toString();
                            quotaIds << id;
                            quotaPerModel |= id.contains(QLatin1String("PerModel"))
                                             || violation.value("quotaDimensions").toObject().contains("model");
                        }
                    }
                }

                QString hint;
                if (status == 401 || reason == QLatin1String("API_KEY_INVALID")
                    || (status == 400 && message.contains(QLatin1String("API key"), Qt::CaseInsensitive))) {
                    s.failure = Failure::Auth;
                    hint = QCoreApplication::translate("AiQueryAssistant", "The API key was rejected. Check it in Preferences.");
                } else if (status == 403 || state == QLatin1String("PERMISSION_DENIED")) {
                    s.failure = Failure::Auth;
                    hint = QStringLiteral("This API key may not use the Gemini API (check that the key is "
                                          "allowed to call the Generative Language API).");
                } else if (status == 404 || state == QLatin1String("NOT_FOUND")) {
                    s.failure = Failure::ModelUnavailable;
                    hint = QStringLiteral("The selected model is not available (it may have been retired). "
                                          "Refresh the model list in Preferences.");
                } else if (status == 429 || state == QLatin1String("RESOURCE_EXHAUSTED")) {
                    const bool perDay = std::any_of(quotaIds.cbegin(), quotaIds.cend(), [](const QString &id) {
                        return id.contains(QLatin1String("PerDay"), Qt::CaseInsensitive);
                    });
                    // Every quota message says "check your plan and billing details";
                    // only credit / spend-cap messages without quota details are billing.
                    static const QRegularExpression billingText(
                        QStringLiteral("prepay|credits? (are |is )?(depleted|exhausted)|spend(ing)? (cap|limit)"),
                        QRegularExpression::CaseInsensitiveOption);
                    const bool billing = quotaIds.isEmpty() && billingText.match(message).hasMatch();
                    s.allModels = billing || (!quotaIds.isEmpty() && !quotaPerModel);
                    if (perDay || billing) {
                        s.failure = Failure::QuotaExhausted;
                        if (billing)
                            hint = QStringLiteral("The Gemini API credits or spending limit for this project "
                                                  "are used up. Check billing in Google AI Studio.");
                        else
                            hint = s.allModels
                                       ? QStringLiteral("The daily Gemini quota for this project is used up. It "
                                                        "resets at midnight Pacific time.")
                                       : QStringLiteral("The daily quota for this model is used up. It resets "
                                                        "at midnight Pacific time; another model may still work.");
                    } else {
                        s.failure = Failure::RateLimited;
                        hint = s.retryAfterSec > 0
                                   ? QCoreApplication::translate("AiQueryAssistant", "Rate limit reached. Try again in %1 s.").arg(s.retryAfterSec)
                                   : QCoreApplication::translate("AiQueryAssistant", "Rate limit reached. Try again in a moment.");
                    }
                } else if (status == 503 || state == QLatin1String("UNAVAILABLE")) {
                    s.failure = Failure::Overloaded;
                    hint = QCoreApplication::translate("AiQueryAssistant", "The model is overloaded right now. Try again in a moment.");
                } else if (status >= 500) {
                    s.failure = Failure::Overloaded;
                    hint = QCoreApplication::translate("AiQueryAssistant", "The API reported a server error. Try again in a moment.");
                } else {
                    s.failure = Failure::Other;
                }
                s.error = hint.isEmpty() ? QCoreApplication::translate("AiQueryAssistant", "Request failed (HTTP %1).").arg(status) : hint;
                // Quota messages are long and full of links; the hint says it all.
                if (!message.isEmpty() && s.failure != Failure::QuotaExhausted
                    && s.failure != Failure::RateLimited)
                    s.error += QStringLiteral(" (%1)").arg(message.left(300));
            }

            void httpError(Suggestion &s, Provider provider, int status, const QJsonObject &root,
                           const QByteArray &retryAfter)
            {
                if (provider == Provider::Gemini)
                    geminiError(s, status, root.value("error").toObject());
                else
                    anthropicError(s, status, root.value("error").toObject(), retryAfter);
                if (s.retryAfterSec == 0)
                    s.retryAfterSec = parseRetryAfter(retryAfter);
                s.retryable = s.failure == Failure::RateLimited || s.failure == Failure::Overloaded;
            }

            // The structured answer, shared by both providers.
            void readAnswer(Suggestion &s, const QString &text, const QString &database,
                            const QStringList &knownCollections)
            {
                const QJsonObject answer = QJsonDocument::fromJson(text.toUtf8()).object();
                if (answer.isEmpty()) {
                    s.failure = Failure::Other;
                    s.error = QCoreApplication::translate("AiQueryAssistant", "The model did not return a query.");
                    return;
                }

                s.kind = answer.value("kind").toString();
                s.explanation = answer.value("explanation").toString().trimmed();
                s.script = answer.value("script").toString().trimmed();
                while (s.script.endsWith(';'))
                    s.script = s.script.chopped(1).trimmed();

                if (s.kind == QLatin1String("unsupported") || s.script.isEmpty()) {
                    s.failure = Failure::Other;
                    s.error = s.explanation.isEmpty()
                                  ? QCoreApplication::translate("AiQueryAssistant", "The model could not turn this request into a query.")
                                  : s.explanation;
                    return;
                }

                // The same parser as Export Query to Language: one find() chain or aggregate([...]).
                const QueryCodeExport::Result check = QueryCodeExport::exportScript(
                    s.script, database, QStringLiteral("mongodb://localhost:27017"),
                    QueryCodeExport::Language::NodeJs);
                if (!check.ok) {
                    s.failure = Failure::Other;
                    s.error = QCoreApplication::translate("AiQueryAssistant", "The generated query could not be checked: %1").arg(check.error);
                    return;
                }

                // A join may only read the collections the model was told about.
                if (!knownCollections.isEmpty()) {
                    for (const QString &name : referencedCollections(s.script)) {
                        if (!knownCollections.contains(name)) {
                            s.failure = Failure::Other;
                            s.error = QCoreApplication::translate(
                                          "AiQueryAssistant",
                                          "The generated query reads the collection \"%1\", which was not described to the model.")
                                          .arg(name);
                            return;
                        }
                    }
                }

                static const QRegularExpression writeStage(QStringLiteral("[\"']?\\$(out|merge)[\"']?\\s*:"));
                s.writesData = writeStage.match(s.script).hasMatch();
                s.ok = true;
            }

            void anthropicAnswer(Suggestion &s, const QJsonObject &root, const QString &database,
                                 const QStringList &knownCollections)
            {
                const QString stopReason = root.value("stop_reason").toString();
                if (stopReason == QLatin1String("refusal")) {
                    s.failure = Failure::Blocked;
                    s.error = QCoreApplication::translate("AiQueryAssistant", "The model declined this request.");
                    return;
                }
                if (stopReason == QLatin1String("max_tokens")) {
                    s.failure = Failure::Other;
                    s.error = QCoreApplication::translate("AiQueryAssistant", "The answer was cut off. Try a shorter or simpler request.");
                    return;
                }
                QString text;
                for (const QJsonValue &block : root.value("content").toArray()) {
                    const QJsonObject b = block.toObject();
                    if (b.value("type").toString() == QLatin1String("text"))
                        text += b.value("text").toString();
                }
                readAnswer(s, text, database, knownCollections);
            }

            void geminiAnswer(Suggestion &s, const QJsonObject &root, const QString &database,
                              const QStringList &knownCollections)
            {
                const QString blockReason = root.value("promptFeedback").toObject().value("blockReason").toString();
                if (!blockReason.isEmpty()) {
                    s.failure = Failure::Blocked;
                    s.error = QCoreApplication::translate("AiQueryAssistant", "The request was blocked (%1).").arg(blockReason);
                    return;
                }
                const QJsonObject candidate = root.value("candidates").toArray().first().toObject();
                const QString finish = candidate.value("finishReason").toString();
                if (finish == QLatin1String("MAX_TOKENS")) {
                    s.failure = Failure::Other;
                    s.error = QCoreApplication::translate("AiQueryAssistant", "The answer was cut off. Try a shorter or simpler request.");
                    return;
                }
                static const QStringList blocked{"SAFETY", "RECITATION", "BLOCKLIST", "PROHIBITED_CONTENT",
                                                 "SPII", "LANGUAGE"};
                if (blocked.contains(finish)) {
                    s.failure = Failure::Blocked;
                    s.error = QCoreApplication::translate("AiQueryAssistant", "The model declined this request (%1).").arg(finish);
                    return;
                }
                QString text;
                for (const QJsonValue &part : candidate.value("content").toObject().value("parts").toArray()) {
                    const QJsonObject p = part.toObject();
                    if (!p.value("thought").toBool())
                        text += p.value("text").toString();
                }
                readAnswer(s, text, database, knownCollections);
            }

            // "gemini-3.8-flash" → 3.8; aliases without a number sort first.
            double geminiVersion(const QString &id)
            {
                static const QRegularExpression version(QStringLiteral("^gemini-(\\d+(?:\\.\\d+)?)"));
                const QRegularExpressionMatch m = version.match(id);
                return m.hasMatch() ? m.captured(1).toDouble() : 1000.0;
            }

            bool usableGeminiModel(const QString &id, const QJsonArray &methods)
            {
                if (!id.startsWith(QLatin1String("gemini-")) || !methods.contains(QStringLiteral("generateContent")))
                    return false;
                static const QStringList excluded{"embedding", "tts", "image", "audio", "live", "robotics",
                                                  "computer-use", "aqa"};
                for (const QString &word : excluded)
                    if (id.contains(word))
                        return false;
                return true;
            }

            QString key(Provider provider, const QString &model)
            {
                return providerId(provider) + '/' + model;
            }
        }

        QStringList providerIds()
        {
            return {QStringLiteral("ragoshell_cloud"), QStringLiteral("anthropic"),
                    QStringLiteral("gemini")};
        }

        QString providerId(Provider provider)
        {
            if (provider == Provider::RagoCloud)
                return QStringLiteral("ragoshell_cloud");
            return provider == Provider::Gemini ? QStringLiteral("gemini") : QStringLiteral("anthropic");
        }

        Provider providerFromId(const QString &id)
        {
            if (id == QLatin1String("gemini"))
                return Provider::Gemini;
            if (id == QLatin1String("ragoshell_cloud"))
                return Provider::RagoCloud;
            return Provider::Anthropic;
        }

        QString providerName(Provider provider)
        {
            if (provider == Provider::RagoCloud)
                return QStringLiteral("RagoShell AI");
            return provider == Provider::Gemini ? QStringLiteral("Google Gemini")
                                                : QStringLiteral("Anthropic Claude");
        }

        QStringList apiKeyEnvironmentVariables(Provider provider)
        {
            if (provider == Provider::RagoCloud)
                return {};
            if (provider == Provider::Gemini)
                return {QStringLiteral("GEMINI_API_KEY"), QStringLiteral("GOOGLE_API_KEY")};
            return {QStringLiteral("ANTHROPIC_API_KEY")};
        }

        QString apiKeyFromEnvironment(Provider provider)
        {
            for (const QString &name : apiKeyEnvironmentVariables(provider)) {
                const QString value = qEnvironmentVariable(name.toUtf8().constData()).trimmed();
                if (!value.isEmpty())
                    return value;
            }
            return {};
        }

        QStringList modelIds(Provider provider)
        {
            if (provider == Provider::RagoCloud) {
                // Server-side allowlist decides; shown as one stable choice.
                return {QStringLiteral("ragoshell-ai")};
            }
            if (provider == Provider::Gemini) {
                // gemini-flash-latest follows each new Flash release, so it
                // survives model retirements; flash-lite first after it, as it
                // has its own quota.
                return {QStringLiteral("gemini-flash-latest"), QStringLiteral("gemini-3.5-flash-lite"),
                        QStringLiteral("gemini-3.8-flash"), QStringLiteral("gemini-4-argon")};
            }
            return {QStringLiteral("claude-opus-5-5"), QStringLiteral("claude-sonnet-5-5"),
                    QStringLiteral("claude-haiku-5-5")};
        }

        QString defaultModel(Provider provider)
        {
            return modelIds(provider).first();
        }

        QStringList fallbackCandidates(Provider provider, const QString &preferred,
                                       const QStringList &available, int maxModels)
        {
            QStringList models;
            if (!preferred.isEmpty())
                models << preferred;
            for (const QString &m : modelIds(provider)) {
                if (models.size() >= maxModels)
                    break;
                // Aliases such as gemini-flash-latest are not listed by the API.
                const bool known = available.isEmpty() || available.contains(m) || m.endsWith(QLatin1String("-latest"));
                if (known && !models.contains(m))
                    models << m;
            }
            return models;
        }

        QString endpointUrl(Provider provider, const QString &model)
        {
            if (provider == Provider::RagoCloud) {
                // Resolved by AiAssistant from CloudClient::effectiveApiUrl();
                // the env override still applies to tests.
                QString base = productEnv("AI_BASE_URL");
                while (base.endsWith('/'))
                    base.chop(1);
                return base + QStringLiteral("/v1/ai/generate");
            }
            if (provider == Provider::Gemini) {
                const QString m = model.isEmpty() ? defaultModel(provider) : model;
                return baseUrl(provider) + QStringLiteral("/v1beta/models/%1:generateContent").arg(m);
            }
            return baseUrl(provider) + QStringLiteral("/v1/messages");
        }

        QString modelsUrl(Provider provider)
        {
            return baseUrl(provider) + (provider == Provider::Gemini
                                            ? QStringLiteral("/v1beta/models?pageSize=1000")
                                            : QStringLiteral("/v1/models?limit=1000"));
        }

        std::vector<std::pair<QByteArray, QByteArray>> requestHeaders(const QString &apiKey,
                                                                      const QString &model)
        {
            return requestHeaders(Provider::Anthropic, apiKey, model);
        }

        std::vector<std::pair<QByteArray, QByteArray>> requestHeaders(Provider provider,
                                                                      const QString &apiKey,
                                                                      const QString &model)
        {
            if (provider == Provider::Gemini)
                return {{"content-type", "application/json"}, {"x-goog-api-key", apiKey.toUtf8()}};
            std::vector<std::pair<QByteArray, QByteArray>> headers{
                {"content-type", "application/json"},
                {"x-api-key", apiKey.toUtf8()},
                {"anthropic-version", "2026-10-21"},
            };
            if (!model.isEmpty() && supportsFallback(model))
                headers.emplace_back("anthropic-beta", "server-side-fallback-2026-07-01");
            return headers;
        }

        Suggestion parseResponse(int httpStatus, const QByteArray &body, const QString &database)
        {
            return parseResponse(Provider::Anthropic, httpStatus, body, database);
        }

        Suggestion parseResponse(Provider provider, int httpStatus, const QByteArray &body,
                                 const QString &database, const QByteArray &retryAfter,
                                 const QStringList &knownCollections)
        {
            Suggestion s;
            QJsonParseError parseError;
            QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
            // Gemini errors sometimes come wrapped in a one-element array.
            const QJsonObject root = doc.isArray() ? doc.array().first().toObject() : doc.object();

            if (httpStatus != 200) {
                httpError(s, provider, httpStatus, root, retryAfter);
                return s;
            }
            if (parseError.error != QJsonParseError::NoError) {
                s.failure = Failure::Other;
                s.error = QCoreApplication::translate("AiQueryAssistant", "The API returned an unreadable response.");
                return s;
            }
            if (provider == Provider::Gemini)
                geminiAnswer(s, root, database, knownCollections);
            else
                anthropicAnswer(s, root, database, knownCollections);
            return s;
        }

        ModelList parseModelList(Provider provider, int httpStatus, const QByteArray &body)
        {
            ModelList list;
            const QJsonObject root = QJsonDocument::fromJson(body).object();
            if (httpStatus != 200) {
                Suggestion s;
                httpError(s, provider, httpStatus, root, {});
                list.error = s.error;
                return list;
            }

            if (provider == Provider::Gemini) {
                for (const QJsonValue &v : root.value("models").toArray()) {
                    const QJsonObject m = v.toObject();
                    const QString id = m.value("name").toString().section('/', -1);
                    if (usableGeminiModel(id, m.value("supportedGenerationMethods").toArray())
                        && !list.models.contains(id))
                        list.models << id;
                }
                // Newest version first, stable before preview / experimental.
                std::stable_sort(list.models.begin(), list.models.end(), [](const QString &a, const QString &b) {
                    const double va = geminiVersion(a), vb = geminiVersion(b);
                    if (va != vb)
                        return va > vb;
                    const bool pa = a.contains(QLatin1String("preview")) || a.contains(QLatin1String("exp"));
                    const bool pb = b.contains(QLatin1String("preview")) || b.contains(QLatin1String("exp"));
                    if (pa != pb)
                        return !pa;
                    return a < b;
                });
            } else {
                // Already newest first.
                for (const QJsonValue &v : root.value("data").toArray()) {
                    const QString id = v.toObject().value("id").toString();
                    if (id.startsWith(QLatin1String("claude-")))
                        list.models << id;
                }
            }
            list.ok = !list.models.isEmpty();
            if (!list.ok)
                list.error = QCoreApplication::translate("AiQueryAssistant", "The API listed no usable models.");
            return list;
        }

        QDateTime dailyQuotaReset(const QDateTime &now)
        {
            QTimeZone pacific("America/Los_Angeles");
            if (!pacific.isValid())
                pacific = QTimeZone(-8 * 3600);
            const QDateTime local = now.toTimeZone(pacific);
            return QDateTime(local.date().addDays(1), QTime(0, 0), pacific).toUTC();
        }

        QDateTime ModelCooldowns::record(Provider provider, const QString &model, const Suggestion &failure,
                                         const QDateTime &now)
        {
            int seconds = 0;
            QDateTime until;
            switch (failure.failure) {
            case Failure::RateLimited:
                seconds = failure.retryAfterSec > 0 ? failure.retryAfterSec : kRateLimitCooldownSec;
                break;
            case Failure::Overloaded:
                seconds = failure.retryAfterSec > 0 ? failure.retryAfterSec : kOverloadCooldownSec;
                break;
            case Failure::ModelUnavailable:
                seconds = kUnavailableCooldownSec;
                break;
            case Failure::QuotaExhausted:
                // Gemini sends a RetryInfo of a few seconds even for daily quotas.
                if (provider == Provider::Gemini)
                    until = dailyQuotaReset(now);
                else
                    seconds = failure.retryAfterSec > 0 ? failure.retryAfterSec : kCreditCooldownSec;
                break;
            default:
                return {};
            }
            if (!until.isValid())
                until = now.addSecs(seconds);
            const QString k = key(provider, failure.allModels ? QStringLiteral("*") : model);
            _entries[k] = Entry{until, failure.failure};
            return until;
        }

        ModelCooldowns::Entry ModelCooldowns::blocked(Provider provider, const QString &model,
                                                      const QDateTime &now) const
        {
            Entry result;
            for (const QString &k : {key(provider, model), key(provider, QStringLiteral("*"))}) {
                const auto it = _entries.find(k);
                if (it != _entries.end() && it->second.until > now
                    && (!result.until.isValid() || it->second.until > result.until))
                    result = it->second;
            }
            return result;
        }

        void ModelCooldowns::clear(Provider provider, const QString &model)
        {
            _entries.erase(key(provider, model));
            _entries.erase(key(provider, QStringLiteral("*")));
        }

        QString ModelCooldowns::pick(Provider provider, const QStringList &candidates, const QStringList &tried,
                                     const QDateTime &now) const
        {
            for (const QString &m : candidates)
                if (!tried.contains(m) && !blocked(provider, m, now).until.isValid())
                    return m;
            return {};
        }

        QString failureSummary(const Suggestion &failure, const QDateTime &until)
        {
            QString when;
            if (until.isValid()) {
                const QDateTime local = until.toLocalTime();
                const QString time = QLocale().toString(local.time(), QLocale::ShortFormat);
                when = local.date() == QDate::currentDate()
                           ? time
                           : QLocale().toString(local.date(), QStringLiteral("ddd")) + ' ' + time;
            }
            switch (failure.failure) {
            case Failure::QuotaExhausted:
                return when.isEmpty() ? QStringLiteral("quota used up")
                                      : QStringLiteral("quota used up until %1").arg(when);
            case Failure::RateLimited:
                return failure.retryAfterSec > 0
                           ? QStringLiteral("rate limit reached (free again in %1 s)").arg(failure.retryAfterSec)
                           : QStringLiteral("rate limit reached");
            case Failure::Overloaded:
                return QStringLiteral("temporarily overloaded");
            case Failure::ModelUnavailable:
                return QStringLiteral("not available (retired or not enabled for this key)");
            default:
                return failure.error;
            }
        }

        QByteArray cloudRequestBody(const Context &context, const QString &question)
        {
            QJsonObject body{
                {"provider", QStringLiteral("anthropic")},
                {"messages", QJsonArray{
                    QJsonObject{{"role", "system"}, {"content", systemPrompt()}},
                    QJsonObject{{"role", "user"}, {"content", userPrompt(context, question)}}}},
                {"max_tokens", 4096},
            };
            return QJsonDocument(body).toJson(QJsonDocument::Compact);
        }

        Suggestion parseCloudResponse(int httpStatus, const QByteArray &body,
                                      const QString &database, const QByteArray &retryAfter,
                                      const QStringList &knownCollections)
        {
            Suggestion s;
            const QJsonObject root = QJsonDocument::fromJson(body).object();
            if (httpStatus != 200) {
                const QString code = root.value("error").toObject().value("code").toString();
                if (httpStatus == 402 || code == QLatin1String("ai_quota_exceeded")) {
                    s.failure = Failure::QuotaExhausted;
                    s.allModels = true;
                    s.error = QCoreApplication::translate(
                        "AiQueryAssistant",
                        "The monthly RagoShell AI quota is used up. Upgrade or wait for the next month.");
                } else if (httpStatus == 429 || code == QLatin1String("rate_limited")) {
                    s.failure = Failure::RateLimited;
                    s.error = QCoreApplication::translate(
                        "AiQueryAssistant", "Too many requests; retry shortly.");
                } else if (httpStatus == 403 || code == QLatin1String("ai_entitlement_required")) {
                    s.failure = Failure::Auth;
                    s.error = QCoreApplication::translate(
                        "AiQueryAssistant", "RagoShell AI requires a Pro or Team plan.");
                } else if (httpStatus == 401) {
                    s.failure = Failure::Auth;
                    s.error = QCoreApplication::translate(
                        "AiQueryAssistant", "The RagoShell session expired; sign in again.");
                } else {
                    s.failure = Failure::Overloaded;
                    s.error = QCoreApplication::translate(
                        "AiQueryAssistant", "The AI service is temporarily unavailable.");
                }
                s.retryAfterSec = parseRetryAfter(retryAfter);
                s.retryable = s.failure == Failure::RateLimited || s.failure == Failure::Overloaded;
                return s;
            }
            const QString text = root.value("content").toString();
            if (text.isEmpty()) {
                s.failure = Failure::Other;
                s.error = QCoreApplication::translate(
                    "AiQueryAssistant", "The API returned an unreadable response.");
                return s;
            }
            readAnswer(s, text, database, knownCollections);
            return s;
        }

}
