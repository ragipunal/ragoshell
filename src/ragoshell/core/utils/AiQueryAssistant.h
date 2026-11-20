#pragma once

#include <map>
#include <vector>

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

#include "ragoshell/core/utils/SchemaAnalysis.h"

namespace RagoShell
{
    /**
     * @brief Natural language → one read-only shell query, via the Claude
     * Messages API or the Gemini API (raw HTTPS; there is no C++ SDK).
     *
     * Only what schemaSummary() returns is sent: field paths, BSON types and
     * presence from a sample, plus frequent values and min/max when the user
     * opts in. Building the request, checking the answer and choosing a
     * fallback model when one runs out of quota live here so they can be unit
     * tested; the HTTP calls themselves are done by the UI layer.
     */
    namespace AiQueryAssistant
    {
        /// RagoCloud = hosted "RagoShell AI": no user key, the API proxy picks
        /// the upstream provider/model (roadmap 6.2 / A8).
        enum class Provider { Anthropic, Gemini, RagoCloud };

        /// Stable ids used in settings: "anthropic", "gemini".
        QStringList providerIds();
        QString providerId(Provider provider);
        Provider providerFromId(const QString &id);   ///< unknown → Anthropic
        /// "Anthropic Claude", "Google Gemini".
        QString providerName(Provider provider);
        /// Environment variables checked when no key is saved, in order.
        QStringList apiKeyEnvironmentVariables(Provider provider);
        QString apiKeyFromEnvironment(Provider provider);

        /// Built-in models, the default first, in fallback order. Providers
        /// retire models often, so the UI also offers the list fetched with
        /// modelsUrl() and accepts any model id.
        QStringList modelIds(Provider provider = Provider::Anthropic);
        QString defaultModel(Provider provider = Provider::Anthropic);

        /// Models to try for one request: preferred, then the built-in list
        /// (only those in available, when that is known), at most maxModels.
        QStringList fallbackCandidates(Provider provider, const QString &preferred,
                                       const QStringList &available, int maxModels = 3);

        /// Generation endpoint, overridable with RAGOSHELL_AI_BASE_URL
        /// (tests point it at a local mock server).
        QString endpointUrl(Provider provider = Provider::Anthropic, const QString &model = {});

        /// Model list endpoint (GET).
        QString modelsUrl(Provider provider);

        /// HTTP headers for a request with the given key and model.
        std::vector<std::pair<QByteArray, QByteArray>> requestHeaders(const QString &apiKey,
                                                                      const QString &model);
        std::vector<std::pair<QByteArray, QByteArray>> requestHeaders(Provider provider,
                                                                      const QString &apiKey,
                                                                      const QString &model);

        /// Another collection of the database, described for $lookup: field
        /// paths with types and presence, never values.
        struct RelatedCollection
        {
            QString name;
            qint64 documents = -1;          ///< estimated; -1 = unknown
            std::vector<SchemaField> fields;
            int omittedFields = 0;          ///< cut to stay within the budget
        };

        struct Context
        {
            QString database;
            QString collection;
            QString serverVersion;        ///< e.g. "4.0.28"; empty when unknown
            qint64 sampledDocuments = 0;
            qint64 recentDocuments = 0;   ///< of sampledDocuments, the newest
            qint64 totalDocuments = -1;   ///< estimated; -1 = unknown
            bool fullScan = false;        ///< every document was analyzed
            QStringList indexes;          ///< index keys as JSON
            std::vector<SchemaField> fields;
            bool includeValues = false;   ///< frequent values, min/max and letter case
            std::vector<RelatedCollection> related;   ///< relatedCollections(); may be empty
        };

        /// Characters of related-collection description sent with a request.
        constexpr int kRelatedBudget = 6000;

        /**
         * @brief The other analyzed collections worth describing, most
         * relevant first, until their description reaches budget characters:
         * collections named in the question; collections the open one
         * references (customerId → customers); collections that reference
         * the open one; then the rest, smallest first. Fields are cut by
         * presence when a collection does not fit whole.
         */
        std::vector<RelatedCollection> relatedCollections(const QString &openCollection,
                                                          const std::vector<SchemaField> &openFields,
                                                          const QString &question,
                                                          const std::map<QString, SchemaResult> &others,
                                                          int budget = kRelatedBudget);

        /// The collections a script reads besides its own ($lookup /
        /// $graphLookup from, $unionWith), in order of appearance.
        QStringList referencedCollections(const QString &script);

        /// The context of a schema analysis (fields, sizes, indexes).
        Context contextFrom(const SchemaResult &result, const QString &database, const QString &collection,
                            const QString &serverVersion, bool includeValues);

        /// Exactly the collection description that is sent (shown to the user).
        QString schemaSummary(const Context &context);

        QString systemPrompt();
        QString userPrompt(const Context &context, const QString &question);

        /// JSON body for the generation request (structured output).
        QByteArray requestBody(const QString &model, const Context &context,
                               const QString &question);
        QByteArray requestBody(Provider provider, const QString &model, const Context &context,
                               const QString &question);

        /// Why a request failed; decides whether another model is worth trying.
        enum class Failure
        {
            None,
            Auth,               ///< key rejected; no other model helps
            ModelUnavailable,   ///< retired, unknown or not allowed for this key
            RateLimited,        ///< per-minute limit; clears after retryAfterSec
            QuotaExhausted,     ///< daily quota or credit used up
            Overloaded,         ///< overload / server error
            Blocked,            ///< refusal or safety block
            Other,
        };

        struct Suggestion
        {
            bool ok = false;
            QString error;
            bool retryable = false;       ///< rate limit / overload / server error
            Failure failure = Failure::None;
            int retryAfterSec = 0;        ///< from Retry-After / RetryInfo; 0 = unknown
            bool allModels = false;       ///< the limit is per key/project, not per model
            QString kind;                 ///< "find" or "aggregate"
            QString script;
            QString explanation;
            bool writesData = false;      ///< $out / $merge stage
        };

        /// Interprets an HTTP response; the script is checked to be a single
        /// find(...) chain or aggregate([...]) statement. retryAfter is the
        /// raw Retry-After header, if any. When knownCollections is not
        /// empty, a script reading another collection must name one of them.
        Suggestion parseResponse(int httpStatus, const QByteArray &body,
                                 const QString &database);
        Suggestion parseResponse(Provider provider, int httpStatus, const QByteArray &body,
                                 const QString &database, const QByteArray &retryAfter = {},
                                 const QStringList &knownCollections = {});

        /// Hosted-proxy request envelope for POST /v1/ai/generate: system +
        /// user messages, max_tokens capped to the API limit.
        QByteArray cloudRequestBody(const Context &context, const QString &question);

        /// Interprets a hosted-proxy response: 200 carries `content` (the
        /// model's JSON answer), 402 ai_quota_exceeded, 403 entitlement,
        /// 429 rate_limited, 502/504 upstream.
        Suggestion parseCloudResponse(int httpStatus, const QByteArray &body,
                                      const QString &database,
                                      const QByteArray &retryAfter = {},
                                      const QStringList &knownCollections = {});

        struct ModelList
        {
            bool ok = false;
            QString error;
            QStringList models;           ///< usable for text generation, newest first
        };

        /// Interprets a modelsUrl() response.
        ModelList parseModelList(Provider provider, int httpStatus, const QByteArray &body);

        /// Next midnight in Pacific time, when Gemini daily quotas reset.
        QDateTime dailyQuotaReset(const QDateTime &now);

        /**
         * @brief Models that recently failed with a quota, rate-limit, overload
         * or "model not found" error, and until when to skip them. Shared by all
         * assistant dialogs of the process.
         */
        class ModelCooldowns
        {
        public:
            struct Entry
            {
                QDateTime until;
                Failure failure = Failure::None;
            };

            /// Remembers a failure; returns until when the model is skipped
            /// (invalid for failures that do not depend on the model).
            QDateTime record(Provider provider, const QString &model, const Suggestion &failure,
                             const QDateTime &now);
            /// Valid entry while the model (or the whole provider) is skipped.
            Entry blocked(Provider provider, const QString &model, const QDateTime &now) const;
            void clear(Provider provider, const QString &model);
            void clearAll() { _entries.clear(); }

            /// First model from candidates that is neither skipped nor in
            /// tried; empty when there is none.
            QString pick(Provider provider, const QStringList &candidates, const QStringList &tried,
                         const QDateTime &now) const;

        private:
            std::map<QString, Entry> _entries;   // "provider/model", "provider/*"
        };

        /// Short reason for a notice: "daily quota used up (resets 09:00)".
        QString failureSummary(const Suggestion &failure, const QDateTime &until);
    }
}
