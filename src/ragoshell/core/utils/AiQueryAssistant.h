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
        /// Bring-your-own-key only: requests go from the device straight to
        /// the provider the user chose, never through a RagoShell server.
        /// OpenAiCompatible talks to any server with the OpenAI Chat
        /// Completions API (OpenAI, OpenRouter, DeepSeek, LM Studio, ...) at a
        /// base address the user picks (docs: B6). Ollama uses the same API
        /// at http://localhost:11434/v1 by default and never sends a key
        /// (docs: B7).
        enum class Provider { Anthropic, Gemini, OpenAiCompatible, Ollama };

        /// True for providers whose base address is a setting.
        bool hasConfigurableBaseUrl(Provider provider);
        /// True for providers spoken to with the OpenAI Chat Completions API.
        bool usesOpenAiApi(Provider provider);
        /// False for Ollama: no key is stored, read from the environment or sent.
        bool usesApiKey(Provider provider);
        /// Address used while none is saved (Ollama); empty for the others.
        QString defaultBaseUrl(Provider provider);

        /// Where an address points, for "data stays on this device" notes.
        enum class HostScope { ThisDevice, LocalNetwork, Internet };
        HostScope hostScope(const QString &url);

        /// Seconds to wait for an answer. Settable for providers with a base
        /// address, as local models can be slow (docs: B7); fixed otherwise.
        inline constexpr int kMinTimeoutSec = 30;
        inline constexpr int kMaxTimeoutSec = 300;
        int defaultTimeoutSec(Provider provider);
        int clampTimeoutSec(int seconds);

        /// Ready-made base addresses for OpenAiCompatible. A preset only
        /// fills the address field; the user sees and may change it.
        struct Preset
        {
            QString id;            ///< "openai", "openrouter", "deepseek", "lmstudio"
            QString name;          ///< shown in Preferences
            QString baseUrl;       ///< normalized (see checkBaseUrl)
            QString keyPageUrl;    ///< where a key is created; empty = no key needed
            QStringList envVars;   ///< used only at exactly this baseUrl (docs: T12)
        };
        QList<Preset> openAiPresets();
        /// The preset whose baseUrl equals this (normalized) address, or
        /// nullptr: a custom address.
        const Preset *presetForBaseUrl(const QString &baseUrl);

        /// Base address check (docs: T2, T3). https everywhere; plain http
        /// only to the loopback address, or (key empty, platform allows it)
        /// to a private network address. No user name / password, query or
        /// fragment; at most kMaxBaseUrlLength; trailing '/' removed.
        enum class BaseUrlError
        {
            None,
            Empty,
            TooLong,
            Malformed,            ///< not an absolute http(s) address with a host
            HasCredentials,       ///< user:password@
            HasQueryOrFragment,
            PlainHttpPublic,      ///< http to an address outside loopback / private ranges
            PlainHttpWithKey,     ///< http to a private address while a key is set
            PlainHttpBlocked,     ///< http to a private address where the platform forbids it (Android)
        };
        inline constexpr int kMaxBaseUrlLength = 2048;
        struct BaseUrlCheck
        {
            BaseUrlError error = BaseUrlError::Empty;
            QString normalized;   ///< valid only when error == None
            bool ok() const { return error == BaseUrlError::None; }
        };
        /// hasKey: a key would be sent. allowPrivateHttp: the platform lets
        /// the app use plain http on the local network (not Android).
        BaseUrlCheck checkBaseUrl(const QString &input, bool hasKey, bool allowPrivateHttp);
        /// "scheme://host:port" of an address (port always written), empty
        /// when invalid. A saved key belongs to one origin (docs: T1).
        QString urlOrigin(const QString &url);
        /// Host shown as "data goes to <host>" (punycode for look-alike names).
        QString urlHost(const QString &url);
        /// Text for the log with key-looking parts and any active key replaced by "[redacted]".
        QString redactSecrets(const QString &text, const QString &activeSecret = {});

        /// Stable ids used in settings: "anthropic", "gemini", "openai_compat", "ollama".
        QStringList providerIds();
        QString providerId(Provider provider);
        Provider providerFromId(const QString &id);   ///< unknown → Anthropic
        /// Settings id of the hosted "RagoShell AI" provider, which was
        /// removed. A saved value is migrated by SettingsManager (assistant
        /// turned off) so it never silently becomes Anthropic.
        inline constexpr const char *kRemovedHostedProviderId = "ragoshell_cloud";
        /// "Anthropic Claude", "Google Gemini".
        QString providerName(Provider provider);
        /// Environment variables checked when no key is saved, in order. For
        /// OpenAiCompatible only those of the preset at exactly baseUrl, so a
        /// personal OPENAI_API_KEY never goes to a custom address (docs: T12).
        QStringList apiKeyEnvironmentVariables(Provider provider, const QString &baseUrl = {});
        QString apiKeyFromEnvironment(Provider provider, const QString &baseUrl = {});

        /// Built-in models, the default first, in fallback order. Providers
        /// retire models often, so the UI also offers the list fetched with
        /// modelsUrl() and accepts any model id.
        QStringList modelIds(Provider provider = Provider::Anthropic);
        QString defaultModel(Provider provider = Provider::Anthropic);

        /// Models to try for one request: preferred, then the built-in list
        /// (only those in available, when that is known), at most maxModels.
        QStringList fallbackCandidates(Provider provider, const QString &preferred,
                                       const QStringList &available, int maxModels = 3);

        /// Generation endpoint. Anthropic / Gemini: overridable with
        /// RAGOSHELL_AI_BASE_URL (tests point it at a local mock server).
        /// OpenAiCompatible: baseUrl (already checked) + "/chat/completions".
        QString endpointUrl(Provider provider = Provider::Anthropic, const QString &model = {},
                            const QString &baseUrl = {});

        /// Model list endpoint (GET).
        QString modelsUrl(Provider provider, const QString &baseUrl = {});

        /// Most bytes read from a generation / model list response (docs: T4).
        inline constexpr qint64 kMaxResponseBytes = 1024 * 1024;
        inline constexpr qint64 kMaxModelListBytes = 4 * 1024 * 1024;
        inline constexpr int kMaxListedModels = 1000;

        /// HTTP headers for a request with the given key and model.
        /// OpenAiCompatible: "Authorization: Bearer" only when the key is not
        /// empty, and no other identifying header.
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

        struct ModelList
        {
            bool ok = false;
            QString error;
            QStringList models;           ///< usable for text generation, newest first
        };

        /// Interprets a modelsUrl() response.
        ModelList parseModelList(Provider provider, int httpStatus, const QByteArray &body);

        /// Provider page where a user creates an API key (Preferences ›
        /// "How to get a key"). The only place these links are kept; for
        /// OpenAiCompatible the preset at baseUrl decides (empty for none).
        QString apiKeyPageUrl(Provider provider, const QString &baseUrl = {});

        /// Preferences › Paste: checks clipboard text before it becomes the
        /// key. The clipboard is read only when the user presses the button.
        enum class PastedKey { Ok, Empty, MultiLine, TooLong, InvalidCharacters };
        inline constexpr int kMaxApiKeyLength = 512;
        /// Trims the text; on Ok, *key receives it. A key is one line of
        /// printable ASCII without spaces, at most kMaxApiKeyLength long.
        PastedKey checkPastedKey(const QString &clipboardText, QString *key);

        /// Preferences › Test connection. Only the model list is requested:
        /// no schema, no prompt. httpStatus 0 means no HTTP response (DNS,
        /// TLS, refused, timeout). Redirects are not followed, so a 3xx
        /// arrives here as RedirectRefused.
        enum class ConnectionStatus {
            Connected,        ///< list.models is filled
            NoUsableModels,   ///< 200, but nothing usable in the list
            NoModelsInstalled,///< 200 with an empty list: a local server with no model yet
            KeyRejected,      ///< 401 / 403 or the provider's "invalid key"
            NoModelList,      ///< 404: reachable, no model list endpoint
            RateLimited,      ///< 429
            RedirectRefused,  ///< 3xx
            ProviderError,    ///< any other HTTP error; list.error has details
            Unreachable,      ///< no HTTP response
        };
        struct ConnectionCheck
        {
            ConnectionStatus status = ConnectionStatus::Unreachable;
            ModelList list;
        };
        ConnectionCheck checkConnection(Provider provider, int httpStatus, const QByteArray &body);

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
