#include "ragoshell/core/utils/AiQueryAssistantInternal.h"

#include <algorithm>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QTimeZone>

#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

#include "ragoshell/core/utils/AppPaths.h"
#include "ragoshell/core/utils/MongoJsonUtils.h"
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

            // OpenAI Chat Completions style: {"error":{"message","type","code"}};
            // some servers send "error" as a plain string.
            void openAiError(Suggestion &s, int status, const QJsonObject &root, const QByteArray &retryAfter)
            {
                const QJsonValue errorValue = root.value("error");
                const QJsonObject error = errorValue.toObject();
                const QString message = errorValue.isString() ? errorValue.toString()
                                                              : error.value("message").toString();
                const QString code = error.value("code").toVariant().toString();
                const QString type = error.value("type").toString();
                QString hint;
                if (status == 401 || status == 403) {
                    s.failure = Failure::Auth;
                    hint = QCoreApplication::translate("AiQueryAssistant", "The API key was rejected. Check it in Preferences.");
                } else if (status == 402 || code == QLatin1String("insufficient_quota")
                           || type == QLatin1String("insufficient_quota")) {
                    s.failure = Failure::QuotaExhausted;
                    s.allModels = true;
                    hint = QCoreApplication::translate("AiQueryAssistant", "The account at this provider has no credit or quota left.");
                } else if (status == 404) {
                    s.failure = Failure::ModelUnavailable;
                    hint = QCoreApplication::translate("AiQueryAssistant", "The server does not know this model or address. Check both in Preferences.");
                } else if (status == 429) {
                    s.failure = Failure::RateLimited;
                    s.retryAfterSec = parseRetryAfter(retryAfter);
                    hint = s.retryAfterSec > 0
                               ? QCoreApplication::translate("AiQueryAssistant", "Rate limit reached. Try again in %1 s.").arg(s.retryAfterSec)
                               : QCoreApplication::translate("AiQueryAssistant", "Rate limit reached. Try again in a moment.");
                } else if (status >= 500) {
                    s.failure = Failure::Overloaded;
                    hint = QCoreApplication::translate("AiQueryAssistant", "The API reported a server error. Try again in a moment.");
                } else {
                    s.failure = Failure::Other;
                }
                s.error = hint.isEmpty() ? QCoreApplication::translate("AiQueryAssistant", "Request failed (HTTP %1).").arg(status) : hint;
                if (!message.isEmpty())
                    s.error += QStringLiteral(" (%1)").arg(message.left(300));
            }

            void httpError(Suggestion &s, Provider provider, int status, const QJsonObject &root,
                           const QByteArray &retryAfter)
            {
                if (status >= 300 && status < 400) {
                    // Redirects are never followed: the key header would go
                    // along to the other server (docs: T6).
                    s.failure = Failure::Other;
                    s.error = QCoreApplication::translate("AiQueryAssistant",
                                                          "The server answered with a redirect (HTTP %1). It was not "
                                                          "followed, so nothing was sent anywhere else. Check the address.")
                                  .arg(status);
                    s.retryable = false;
                    return;
                }
                if (provider == Provider::Gemini)
                    geminiError(s, status, root.value("error").toObject());
                else if (usesOpenAiApi(provider))
                    openAiError(s, status, root, retryAfter);
                else
                    anthropicError(s, status, root.value("error").toObject(), retryAfter);
                if (s.retryAfterSec == 0)
                    s.retryAfterSec = parseRetryAfter(retryAfter);
                s.retryable = s.failure == Failure::RateLimited || s.failure == Failure::Overloaded;
            }

            // Operators found in the parsed query: the keys the server will see,
            // with any JSON escape already decoded (docs: B6 review).
            struct OperatorScan
            {
                bool writes = false;     ///< $out / $merge
                QString javascript;      ///< $where / $function / $accumulator
                bool tooDeep = false;
            };

            void scanArray(bsoncxx::array::view arr, OperatorScan &scan, int depth);

            void scanDocument(bsoncxx::document::view doc, OperatorScan &scan, int depth)
            {
                if (depth > 64) {
                    scan.tooDeep = true;
                    return;
                }
                for (auto &&el : doc) {
                    const std::string key(el.key());
                    if (key == "$out" || key == "$merge")
                        scan.writes = true;
                    else if (key == "$where" || key == "$function" || key == "$accumulator")
                        scan.javascript = QString::fromStdString(key);
                    if (el.type() == bsoncxx::type::k_document)
                        scanDocument(el.get_document().value, scan, depth + 1);
                    else if (el.type() == bsoncxx::type::k_array)
                        scanArray(el.get_array().value, scan, depth + 1);
                }
            }

            void scanArray(bsoncxx::array::view arr, OperatorScan &scan, int depth)
            {
                if (depth > 64) {
                    scan.tooDeep = true;
                    return;
                }
                for (auto &&el : arr) {
                    if (el.type() == bsoncxx::type::k_document)
                        scanDocument(el.get_document().value, scan, depth + 1);
                    else if (el.type() == bsoncxx::type::k_array)
                        scanArray(el.get_array().value, scan, depth + 1);
                }
            }

            // Parses the script the way the shell will and scans every part.
            OperatorScan scanScript(const QString &script)
            {
                OperatorScan scan;
                std::vector<bsoncxx::document::value> docs;
                const auto find = MongoJsonUtils::parseFindQueryChain(script);
                if (find.valid) {
                    for (const QString &part : {find.filter, find.projection, find.sort}) {
                        if (part.trimmed().isEmpty())
                            continue;
                        bsoncxx::document::value doc = bsoncxx::from_json("{}");
                        QString err;
                        if (MongoJsonUtils::parseSingleDocument(part, doc, err))
                            docs.push_back(std::move(doc));
                    }
                } else {
                    const auto agg = MongoJsonUtils::parseAggregateChain(script);
                    if (agg.valid)
                        MongoJsonUtils::parseDocuments(agg.pipeline, docs, nullptr);
                }
                for (const auto &doc : docs)
                    scanDocument(doc.view(), scan, 0);
                return scan;
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

                const OperatorScan scan = scanScript(s.script);
                if (!scan.javascript.isEmpty() || scan.tooDeep) {
                    s.failure = Failure::Other;
                    s.error = scan.tooDeep
                                  ? QCoreApplication::translate("AiQueryAssistant", "The generated query is nested too deeply.")
                                  : QCoreApplication::translate("AiQueryAssistant",
                                                                "The generated query uses %1, which runs JavaScript on the "
                                                                "server. It was not accepted.")
                                        .arg(scan.javascript);
                    return;
                }
                static const QRegularExpression writeStage(QStringLiteral("[\"']?\\$(out|merge)[\"']?\\s*:"));
                s.writesData = scan.writes || writeStage.match(s.script).hasMatch();
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

            // Without a schema-enforced response, models may wrap the JSON in a
            // Markdown fence or add a sentence: keep the outermost {...}.
            QString extractJsonObject(const QString &text)
            {
                const qsizetype open = text.indexOf(QLatin1Char('{'));
                const qsizetype close = text.lastIndexOf(QLatin1Char('}'));
                return open >= 0 && close > open ? text.mid(open, close - open + 1) : text;
            }

            void openAiAnswer(Suggestion &s, const QJsonObject &root, const QString &database,
                              const QStringList &knownCollections)
            {
                const QJsonObject choice = root.value("choices").toArray().first().toObject();
                const QJsonObject message = choice.value("message").toObject();
                const QString finish = choice.value("finish_reason").toString();
                if (!message.value("refusal").toString().isEmpty() || finish == QLatin1String("content_filter")) {
                    s.failure = Failure::Blocked;
                    s.error = QCoreApplication::translate("AiQueryAssistant", "The model declined this request.");
                    return;
                }
                if (finish == QLatin1String("length")) {
                    s.failure = Failure::Other;
                    s.error = QCoreApplication::translate("AiQueryAssistant", "The answer was cut off. Try a shorter or simpler request.");
                    return;
                }
                readAnswer(s, extractJsonObject(message.value("content").toString()), database, knownCollections);
            }

            // Chat models only: the OpenAI list also has embedding, speech,
            // image and moderation models.
            bool usableOpenAiModel(const QString &id)
            {
                static const QStringList excluded{"embed", "tts", "whisper", "dall-e", "moderation", "transcribe",
                                                  "image", "audio", "realtime", "speech"};
                for (const QString &word : excluded)
                    if (id.contains(word, Qt::CaseInsensitive))
                        return false;
                return !id.isEmpty();
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
            return {QStringLiteral("anthropic"), QStringLiteral("gemini"), QStringLiteral("openai_compat"),
                    QStringLiteral("ollama")};
        }

        QString providerId(Provider provider)
        {
            switch (provider) {
            case Provider::Gemini:
                return QStringLiteral("gemini");
            case Provider::OpenAiCompatible:
                return QStringLiteral("openai_compat");
            case Provider::Ollama:
                return QStringLiteral("ollama");
            case Provider::Anthropic:
                break;
            }
            return QStringLiteral("anthropic");
        }

        Provider providerFromId(const QString &id)
        {
            if (id == QLatin1String("gemini"))
                return Provider::Gemini;
            if (id == QLatin1String("openai_compat"))
                return Provider::OpenAiCompatible;
            if (id == QLatin1String("ollama"))
                return Provider::Ollama;
            return Provider::Anthropic;
        }

        QString providerName(Provider provider)
        {
            switch (provider) {
            case Provider::Gemini:
                return QStringLiteral("Google Gemini");
            case Provider::OpenAiCompatible:
                return QCoreApplication::translate("AiQueryAssistant", "OpenAI-compatible");
            case Provider::Ollama:
                return QStringLiteral("Ollama");
            case Provider::Anthropic:
                break;
            }
            return QStringLiteral("Anthropic Claude");
        }

        QStringList apiKeyEnvironmentVariables(Provider provider, const QString &baseUrl)
        {
            if (provider == Provider::Gemini)
                return {QStringLiteral("GEMINI_API_KEY"), QStringLiteral("GOOGLE_API_KEY")};
            if (!usesApiKey(provider))
                return {};
            if (provider == Provider::OpenAiCompatible) {
                const Preset *preset = presetForBaseUrl(baseUrl);
                return preset ? preset->envVars : QStringList();
            }
            return {QStringLiteral("ANTHROPIC_API_KEY")};
        }

        QString apiKeyFromEnvironment(Provider provider, const QString &baseUrl)
        {
            for (const QString &name : apiKeyEnvironmentVariables(provider, baseUrl)) {
                const QString value = qEnvironmentVariable(name.toUtf8().constData()).trimmed();
                if (!value.isEmpty())
                    return value;
            }
            return {};
        }

        QStringList modelIds(Provider provider)
        {
            if (provider == Provider::Gemini) {
                // gemini-flash-latest follows each new Flash release, so it
                // survives model retirements; flash-lite first after it, as it
                // has its own quota.
                return {QStringLiteral("gemini-flash-latest"), QStringLiteral("gemini-3.5-flash-lite"),
                        QStringLiteral("gemini-3.8-flash"), QStringLiteral("gemini-4-argon")};
            }
            if (usesOpenAiApi(provider))
                return {};   // every server has its own models: picked or typed in Preferences
            return {QStringLiteral("claude-opus-5-5"), QStringLiteral("claude-sonnet-5-5"),
                    QStringLiteral("claude-haiku-5-5")};
        }

        QString defaultModel(Provider provider)
        {
            const QStringList models = modelIds(provider);
            return models.isEmpty() ? QString() : models.first();
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

        QString endpointUrl(Provider provider, const QString &model, const QString &base)
        {
            if (usesOpenAiApi(provider))
                return base + QStringLiteral("/chat/completions");
            if (provider == Provider::Gemini) {
                const QString m = model.isEmpty() ? defaultModel(provider) : model;
                return baseUrl(provider) + QStringLiteral("/v1beta/models/%1:generateContent").arg(m);
            }
            return baseUrl(provider) + QStringLiteral("/v1/messages");
        }

        QString modelsUrl(Provider provider, const QString &base)
        {
            if (usesOpenAiApi(provider))
                return base + QStringLiteral("/models");
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
            if (usesOpenAiApi(provider)) {
                std::vector<std::pair<QByteArray, QByteArray>> headers{{"content-type", "application/json"}};
                if (!apiKey.isEmpty() && usesApiKey(provider))
                    headers.emplace_back("authorization", "Bearer " + apiKey.toUtf8());
                return headers;
            }
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
            else if (usesOpenAiApi(provider))
                openAiAnswer(s, root, database, knownCollections);
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
            } else if (usesOpenAiApi(provider)) {
                const QJsonArray data = root.value("data").toArray();
                for (const QJsonValue &v : data) {
                    const QString id = v.toObject().value("id").toString().trimmed();
                    if (usableOpenAiModel(id) && id.size() <= 200 && !list.models.contains(id))
                        list.models << id;
                    if (list.models.size() >= kMaxListedModels)
                        break;
                }
                list.models.sort(Qt::CaseInsensitive);
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

        QString apiKeyPageUrl(Provider provider, const QString &base)
        {
            if (!usesApiKey(provider))
                return {};
            if (provider == Provider::OpenAiCompatible) {
                const Preset *preset = presetForBaseUrl(base);
                return preset ? preset->keyPageUrl : QString();
            }
            if (provider == Provider::Gemini)
                return QStringLiteral("https://aistudio.google.com/apikey");
            return QStringLiteral("https://platform.claude.com/settings/keys");
        }

        PastedKey checkPastedKey(const QString &clipboardText, QString *key)
        {
            const QString text = clipboardText.trimmed();
            if (text.isEmpty())
                return PastedKey::Empty;
            if (text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r')))
                return PastedKey::MultiLine;
            if (text.size() > kMaxApiKeyLength)
                return PastedKey::TooLong;
            for (const QChar c : text) {
                // Printable ASCII, no space: catches tabs, zero-width and
                // other characters copied from a web page along with the key.
                if (c.unicode() < 0x21 || c.unicode() > 0x7e)
                    return PastedKey::InvalidCharacters;
            }
            if (key)
                *key = text;
            return PastedKey::Ok;
        }

        ConnectionCheck checkConnection(Provider provider, int httpStatus, const QByteArray &body)
        {
            ConnectionCheck c;
            if (httpStatus <= 0) {
                c.status = ConnectionStatus::Unreachable;
                return c;
            }
            if (httpStatus >= 300 && httpStatus < 400) {
                c.status = ConnectionStatus::RedirectRefused;
                return c;
            }
            c.list = parseModelList(provider, httpStatus, body);
            if (c.list.ok) {
                c.status = ConnectionStatus::Connected;
                return c;
            }
            if (httpStatus == 200) {
                // Ollama answers "data": null (older versions) or [] before
                // the first "ollama pull".
                const QJsonValue data = QJsonDocument::fromJson(body).object().value("data");
                const bool empty = data.isNull() || (data.isArray() && data.toArray().isEmpty());
                c.status = usesOpenAiApi(provider) && empty ? ConnectionStatus::NoModelsInstalled
                                                            : ConnectionStatus::NoUsableModels;
                return c;
            }

            Suggestion s;
            httpError(s, provider, httpStatus, QJsonDocument::fromJson(body).object(), {});
            if (httpStatus == 401 || httpStatus == 403 || s.failure == Failure::Auth)
                c.status = ConnectionStatus::KeyRejected;
            else if (httpStatus == 404)
                c.status = ConnectionStatus::NoModelList;
            else if (httpStatus == 429 || s.failure == Failure::RateLimited)
                c.status = ConnectionStatus::RateLimited;
            else
                c.status = ConnectionStatus::ProviderError;
            return c;
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

}
