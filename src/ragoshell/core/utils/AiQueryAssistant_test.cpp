#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>

#include "ragoshell/core/utils/AiQueryAssistant.h"

using namespace RagoShell;

namespace
{
    AiQueryAssistant::Context sampleContext(bool includeValues)
    {
        AiQueryAssistant::Context c;
        c.database = "shop";
        c.collection = "orders";
        c.serverVersion = "4.0.28";
        c.sampledDocuments = 200;
        c.includeValues = includeValues;

        SchemaField id;
        id.path = id.name = "_id";
        id.presence = 1.0;
        id.types = {{"ObjectId", 200}};
        SchemaField status;
        status.path = status.name = "status";
        status.presence = 0.9;
        status.types = {{"String", 170}, {"Null", 10}};
        status.topValues = {{"\"shipped\"", 120}, {"\"secret-customer@example.com\"", 1}};
        SchemaField total;
        total.path = total.name = "total";
        total.presence = 1.0;
        total.types = {{"Double", 200}};
        total.minValue = "1.5";
        total.maxValue = "999";
        SchemaField items;
        items.path = items.name = "items";
        items.presence = 1.0;
        items.types = {{"Array", 200}};
        items.itemTypes = {{"Object", 600}};
        c.fields = {id, status, total, items};
        return c;
    }

    QByteArray okResponse(const QString &kind, const QString &script, const QString &explanation)
    {
        const QJsonObject answer{{"kind", kind}, {"script", script}, {"explanation", explanation}};
        const QJsonObject root{
            {"type", "message"},
            {"role", "assistant"},
            {"stop_reason", "end_turn"},
            {"content", QJsonArray{QJsonObject{
                            {"type", "text"},
                            {"text", QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact))}}}},
        };
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }
}

TEST(AiQueryAssistantTest, schema_summary_without_values_sends_no_data)
{
    const QString s = AiQueryAssistant::schemaSummary(sampleContext(false));
    EXPECT_TRUE(s.contains("Collection: orders"));
    EXPECT_TRUE(s.contains("MongoDB server version: 4.0.28"));
    EXPECT_TRUE(s.contains("_id: ObjectId, 100%"));
    EXPECT_TRUE(s.contains("status: String 94% | Null 6%, 90%"));
    EXPECT_TRUE(s.contains("items: Array, 100%, array items: Object"));
    EXPECT_FALSE(s.contains("shipped"));
    EXPECT_FALSE(s.contains("secret-customer"));
    EXPECT_FALSE(s.contains("999"));
}

TEST(AiQueryAssistantTest, schema_summary_with_values_opt_in)
{
    const QString s = AiQueryAssistant::schemaSummary(sampleContext(true));
    EXPECT_TRUE(s.contains("frequent values: \"shipped\""));
    EXPECT_TRUE(s.contains("total: Double, 100%, range 1.5 .. 999"));
}

TEST(AiQueryAssistantTest, schema_summary_sizes_and_indexes)
{
    AiQueryAssistant::Context c = sampleContext(false);
    EXPECT_TRUE(AiQueryAssistant::schemaSummary(c).contains("Sampled documents: 200"));   // size unknown

    c.totalDocuments = 25359;
    c.sampledDocuments = 1000;
    c.recentDocuments = 300;
    c.indexes = {R"({ "status" : 1 })", R"({ "customer.country" : 1, "total" : -1 })"};
    const QString s = AiQueryAssistant::schemaSummary(c);
    EXPECT_TRUE(s.contains("Documents: about 25359; sampled 1000 (300 newest, the rest random)")) << s.toStdString();
    EXPECT_TRUE(s.contains(R"(Indexes: { "status": 1 }; { "customer.country": 1, "total": -1 })")) << s.toStdString();

    c.fullScan = true;
    c.sampledDocuments = c.totalDocuments = 431;
    c.recentDocuments = 0;
    EXPECT_TRUE(AiQueryAssistant::schemaSummary(c).contains("Documents: 431 (all analyzed)"));
}

TEST(AiQueryAssistantTest, schema_summary_categories_case_and_newest_fields)
{
    AiQueryAssistant::Context c = sampleContext(true);
    SchemaField &status = c.fields[1];
    status.categorical = true;
    status.letterCase = "upper";
    status.topValues.clear();
    for (const char *v : {"SHIPPED", "PENDING", "CANCELLED", "RETURNED", "DELIVERED", "LOST", "HELD"})
        status.topValues.emplace_back(QStringLiteral("\"%1\"").arg(v), 10);
    c.fields[3].onlyRecent = true;   // items

    const QString s = AiQueryAssistant::schemaSummary(c);
    // A category is listed completely (more than the 5 frequent values).
    EXPECT_TRUE(s.contains(R"(all values: "SHIPPED", "PENDING", "CANCELLED", "RETURNED", "DELIVERED", "LOST", "HELD", values are UPPERCASE)"))
        << s.toStdString();
    EXPECT_TRUE(s.contains("items: Array, 100%, array items: Object, only in the newest documents"));

    // Without values: no values, no case, but the structure stays.
    c.includeValues = false;
    const QString without = AiQueryAssistant::schemaSummary(c);
    EXPECT_FALSE(without.contains("SHIPPED"));
    EXPECT_FALSE(without.contains("UPPERCASE"));
    EXPECT_TRUE(without.contains("only in the newest documents"));
}

TEST(AiQueryAssistantTest, context_from_analysis)
{
    SchemaResult r;
    r.ok = true;
    r.sampledDocuments = 1000;
    r.recentDocuments = 300;
    r.totalDocuments = 5001;
    r.indexes = {R"({ "a" : 1 })"};
    r.fields = sampleContext(false).fields;
    const AiQueryAssistant::Context c = AiQueryAssistant::contextFrom(r, "shop", "orders", "7.0.16", true);
    EXPECT_EQ(c.database, QStringLiteral("shop"));
    EXPECT_EQ(c.collection, QStringLiteral("orders"));
    EXPECT_EQ(c.serverVersion, QStringLiteral("7.0.16"));
    EXPECT_EQ(c.sampledDocuments, 1000);
    EXPECT_EQ(c.recentDocuments, 300);
    EXPECT_EQ(c.totalDocuments, 5001);
    EXPECT_EQ(c.indexes, r.indexes);
    EXPECT_EQ(c.fields.size(), r.fields.size());
    EXPECT_TRUE(c.includeValues);
    EXPECT_TRUE(AiQueryAssistant::systemPrompt().contains("letter case"));
}

TEST(AiQueryAssistantTest, request_body_shape)
{
    const QByteArray body = AiQueryAssistant::requestBody("", sampleContext(false), "  shipped orders over 100  ");
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    EXPECT_EQ(o.value("model").toString(), AiQueryAssistant::defaultModel());
    EXPECT_EQ(o.value("fallbacks").toString(), QStringLiteral("default"));
    EXPECT_GT(o.value("max_tokens").toInt(), 1000);
    EXPECT_FALSE(o.contains("temperature"));
    EXPECT_FALSE(o.contains("thinking"));

    const QJsonObject format = o.value("output_config").toObject().value("format").toObject();
    EXPECT_EQ(format.value("type").toString(), QStringLiteral("json_schema"));
    const QJsonObject schema = format.value("schema").toObject();
    EXPECT_FALSE(schema.value("additionalProperties").toBool(true));
    EXPECT_EQ(schema.value("required").toArray().size(), 3);

    const QJsonArray messages = o.value("messages").toArray();
    ASSERT_EQ(messages.size(), 1);
    const QString content = messages[0].toObject().value("content").toString();
    EXPECT_TRUE(content.endsWith("Request:\nshipped orders over 100"));
    EXPECT_TRUE(content.startsWith(AiQueryAssistant::schemaSummary(sampleContext(false))));

    // Haiku has no server-side fallback: neither the parameter nor the beta header.
    const QJsonObject haiku = QJsonDocument::fromJson(
        AiQueryAssistant::requestBody("claude-haiku-5-5", sampleContext(false), "x")).object();
    EXPECT_FALSE(haiku.contains("fallbacks"));
    bool hasBeta = false;
    for (const auto &h : AiQueryAssistant::requestHeaders("k", "claude-haiku-5-5"))
        hasBeta |= h.first == "anthropic-beta";
    EXPECT_FALSE(hasBeta);
}

TEST(AiQueryAssistantTest, request_headers)
{
    QMap<QByteArray, QByteArray> h;
    for (const auto &p : AiQueryAssistant::requestHeaders("sk-test", "claude-opus-5-5"))
        h.insert(p.first, p.second);
    EXPECT_EQ(h.value("x-api-key"), QByteArray("sk-test"));
    EXPECT_EQ(h.value("anthropic-version"), QByteArray("2026-10-21"));
    EXPECT_EQ(h.value("anthropic-beta"), QByteArray("server-side-fallback-2026-07-01"));
    EXPECT_EQ(h.value("content-type"), QByteArray("application/json"));
}

TEST(AiQueryAssistantTest, parse_find_and_aggregate)
{
    auto s = AiQueryAssistant::parseResponse(
        200, okResponse("find", "db.getCollection('orders').find({ status: 'shipped', total: { $gt: 100 } }).sort({ total: -1 });",
                        "Shipped orders above 100."), "shop");
    EXPECT_TRUE(s.ok) << s.error.toStdString();
    EXPECT_EQ(s.kind, QStringLiteral("find"));
    EXPECT_FALSE(s.script.endsWith(';'));
    EXPECT_FALSE(s.writesData);
    EXPECT_EQ(s.explanation, QStringLiteral("Shipped orders above 100."));

    s = AiQueryAssistant::parseResponse(
        200, okResponse("aggregate", "db.getCollection('orders').aggregate([{ $group: { _id: '$status', n: { $sum: 1 } } }])", ""),
        "shop");
    EXPECT_TRUE(s.ok) << s.error.toStdString();

    s = AiQueryAssistant::parseResponse(
        200, okResponse("aggregate", "db.getCollection('orders').aggregate([{ $match: {} }, { $out: 'copy' }])", ""),
        "shop");
    EXPECT_TRUE(s.ok);
    EXPECT_TRUE(s.writesData);
}

TEST(AiQueryAssistantTest, parse_rejections)
{
    auto s = AiQueryAssistant::parseResponse(
        200, okResponse("unsupported", "", "This would delete data."), "shop");
    EXPECT_FALSE(s.ok);
    EXPECT_EQ(s.error, QStringLiteral("This would delete data."));

    s = AiQueryAssistant::parseResponse(
        200, okResponse("find", "db.orders.deleteMany({})", "x"), "shop");
    EXPECT_FALSE(s.ok);
    EXPECT_TRUE(s.error.startsWith("The generated query could not be checked"));

    s = AiQueryAssistant::parseResponse(200, R"({"stop_reason":"refusal","content":[]})", "shop");
    EXPECT_FALSE(s.ok);
    EXPECT_TRUE(s.error.contains("declined"));

    s = AiQueryAssistant::parseResponse(200, R"({"stop_reason":"max_tokens","content":[{"type":"text","text":"{\"ki"}]})", "shop");
    EXPECT_FALSE(s.ok);
    EXPECT_TRUE(s.error.contains("cut off"));
}

TEST(AiQueryAssistantTest, parse_api_errors)
{
    auto s = AiQueryAssistant::parseResponse(
        401, R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})", "shop");
    EXPECT_FALSE(s.ok);
    EXPECT_FALSE(s.retryable);
    EXPECT_TRUE(s.error.contains("API key was rejected"));
    EXPECT_TRUE(s.error.contains("invalid x-api-key"));

    s = AiQueryAssistant::parseResponse(
        529, R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})", "shop");
    EXPECT_TRUE(s.retryable);
    EXPECT_TRUE(s.error.contains("overloaded"));

    s = AiQueryAssistant::parseResponse(502, "<html>bad gateway</html>", "shop");
    EXPECT_TRUE(s.retryable);
    EXPECT_TRUE(s.error.contains("server error"));
}

namespace
{
    using AiQueryAssistant::Failure;
    using AiQueryAssistant::Provider;

    QByteArray geminiOk(const QString &kind, const QString &script, const QString &explanation)
    {
        const QJsonObject answer{{"kind", kind}, {"script", script}, {"explanation", explanation}};
        const QJsonArray parts{
            QJsonObject{{"text", "thinking..."}, {"thought", true}},
            QJsonObject{{"text", QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact))}},
        };
        const QJsonObject candidate{{"finishReason", "STOP"},
                                    {"content", QJsonObject{{"role", "model"}, {"parts", parts}}}};
        return QJsonDocument(QJsonObject{{"candidates", QJsonArray{candidate}}}).toJson(QJsonDocument::Compact);
    }

    QByteArray geminiQuota(const QString &quotaId, const QString &retryDelay, bool modelDimension = true)
    {
        QJsonObject violation{{"quotaMetric", "generativelanguage.googleapis.com/generate_content_free_tier_requests"},
                              {"quotaId", quotaId}};
        if (modelDimension)
            violation.insert("quotaDimensions", QJsonObject{{"location", "global"}, {"model", "gemini-4-argon"}});
        QJsonArray details{QJsonObject{{"@type", "type.googleapis.com/google.rpc.QuotaFailure"},
                                       {"violations", QJsonArray{violation}}}};
        if (!retryDelay.isEmpty())
            details.append(QJsonObject{{"@type", "type.googleapis.com/google.rpc.RetryInfo"}, {"retryDelay", retryDelay}});
        const QJsonObject error{{"code", 429},
                                {"message", "You exceeded your current quota, please check your plan and billing details."},
                                {"status", "RESOURCE_EXHAUSTED"},
                                {"details", details}};
        return QJsonDocument(QJsonObject{{"error", error}}).toJson(QJsonDocument::Compact);
    }
}

TEST(AiQueryAssistantTest, providers)
{
    EXPECT_EQ(AiQueryAssistant::providerFromId("gemini"), Provider::Gemini);
    EXPECT_EQ(AiQueryAssistant::providerFromId(""), Provider::Anthropic);
    EXPECT_EQ(AiQueryAssistant::providerFromId("bogus"), Provider::Anthropic);
    EXPECT_EQ(AiQueryAssistant::providerId(Provider::Gemini), QStringLiteral("gemini"));
    EXPECT_TRUE(AiQueryAssistant::defaultModel(Provider::Gemini).startsWith("gemini-"));
    EXPECT_TRUE(AiQueryAssistant::defaultModel().startsWith("claude-"));
    EXPECT_TRUE(AiQueryAssistant::apiKeyEnvironmentVariables(Provider::Gemini).contains("GEMINI_API_KEY"));
}

TEST(AiQueryAssistantTest, gemini_request)
{
    EXPECT_EQ(AiQueryAssistant::endpointUrl(Provider::Gemini, "gemini-3.8-flash"),
              QStringLiteral("https://generativelanguage.googleapis.com/v1beta/models/gemini-3.8-flash:generateContent"));
    EXPECT_EQ(AiQueryAssistant::modelsUrl(Provider::Gemini),
              QStringLiteral("https://generativelanguage.googleapis.com/v1beta/models?pageSize=1000"));

    QMap<QByteArray, QByteArray> h;
    for (const auto &p : AiQueryAssistant::requestHeaders(Provider::Gemini, "AIza-test", "gemini-3.8-flash"))
        h.insert(p.first, p.second);
    EXPECT_EQ(h.value("x-goog-api-key"), QByteArray("AIza-test"));
    EXPECT_FALSE(h.contains("x-api-key"));
    EXPECT_FALSE(h.contains("anthropic-version"));

    const QJsonObject o = QJsonDocument::fromJson(
        AiQueryAssistant::requestBody(Provider::Gemini, "gemini-3.8-flash", sampleContext(false), " count by status ")).object();
    EXPECT_FALSE(o.contains("model"));
    EXPECT_EQ(o.value("systemInstruction").toObject().value("parts").toArray()[0].toObject().value("text").toString(),
              AiQueryAssistant::systemPrompt());
    const QString content = o.value("contents").toArray()[0].toObject().value("parts").toArray()[0]
                                .toObject().value("text").toString();
    EXPECT_TRUE(content.endsWith("Request:\ncount by status"));
    EXPECT_FALSE(content.contains("shipped"));
    const QJsonObject config = o.value("generationConfig").toObject();
    EXPECT_EQ(config.value("responseMimeType").toString(), QStringLiteral("application/json"));
    EXPECT_EQ(config.value("responseSchema").toObject().value("type").toString(), QStringLiteral("OBJECT"));
    EXPECT_FALSE(config.value("responseSchema").toObject().contains("additionalProperties"));
}

TEST(AiQueryAssistantTest, gemini_parse)
{
    auto s = AiQueryAssistant::parseResponse(
        Provider::Gemini, 200, geminiOk("find", "db.getCollection('orders').find({ status: 'shipped' })", "Shipped."), "shop");
    EXPECT_TRUE(s.ok) << s.error.toStdString();
    EXPECT_EQ(s.kind, QStringLiteral("find"));

    s = AiQueryAssistant::parseResponse(Provider::Gemini, 200, R"({"promptFeedback":{"blockReason":"SAFETY"}})", "shop");
    EXPECT_FALSE(s.ok);
    EXPECT_EQ(s.failure, Failure::Blocked);

    s = AiQueryAssistant::parseResponse(
        Provider::Gemini, 200, R"({"candidates":[{"finishReason":"MAX_TOKENS","content":{"parts":[{"text":"{\"ki"}]}}]})", "shop");
    EXPECT_FALSE(s.ok);
    EXPECT_TRUE(s.error.contains("cut off"));
}

TEST(AiQueryAssistantTest, gemini_quota_errors)
{
    // Daily per-model quota: another model may work; skipped until midnight Pacific.
    auto s = AiQueryAssistant::parseResponse(
        Provider::Gemini, 429, geminiQuota("GenerateRequestsPerDayPerProjectPerModel-FreeTier", "41s"), "shop");
    EXPECT_FALSE(s.ok);
    EXPECT_EQ(s.failure, Failure::QuotaExhausted);
    EXPECT_FALSE(s.allModels);
    EXPECT_FALSE(s.retryable);
    EXPECT_TRUE(s.error.contains("daily quota"));

    // Per minute: rate limited, with the server's retry delay.
    s = AiQueryAssistant::parseResponse(
        Provider::Gemini, 429, geminiQuota("GenerateRequestsPerMinutePerProjectPerModel-FreeTier", "17.2s"), "shop");
    EXPECT_EQ(s.failure, Failure::RateLimited);
    EXPECT_EQ(s.retryAfterSec, 18);
    EXPECT_TRUE(s.retryable);
    EXPECT_TRUE(s.error.contains("18 s"));

    // A project-wide quota stops every model.
    s = AiQueryAssistant::parseResponse(
        Provider::Gemini, 429, geminiQuota("GenerateContentInputTokensPerDayPerProject", "", false), "shop");
    EXPECT_EQ(s.failure, Failure::QuotaExhausted);
    EXPECT_TRUE(s.allModels);

    // Paid tier out of credit: no quota details, every model stops.
    s = AiQueryAssistant::parseResponse(
        Provider::Gemini, 429,
        R"({"error":{"code":429,"message":"Your prepayment credits are depleted. Please go to AI Studio to manage your billing.","status":"RESOURCE_EXHAUSTED"}})",
        "shop");
    EXPECT_EQ(s.failure, Failure::QuotaExhausted);
    EXPECT_TRUE(s.allModels);
    EXPECT_TRUE(s.error.contains("credits"));

    // A bare 429 without details is a rate limit.
    s = AiQueryAssistant::parseResponse(
        Provider::Gemini, 429, R"({"error":{"code":429,"message":"Resource has been exhausted (e.g. check quota).","status":"RESOURCE_EXHAUSTED"}})",
        "shop");
    EXPECT_EQ(s.failure, Failure::RateLimited);
    EXPECT_FALSE(s.allModels);

    s = AiQueryAssistant::parseResponse(
        Provider::Gemini, 404,
        R"({"error":{"code":404,"message":"models/gemini-2.0-flash is not found for API version v1beta","status":"NOT_FOUND"}})",
        "shop");
    EXPECT_EQ(s.failure, Failure::ModelUnavailable);
    EXPECT_TRUE(s.error.contains("retired"));

    s = AiQueryAssistant::parseResponse(
        Provider::Gemini, 400,
        R"({"error":{"code":400,"message":"API key not valid. Please pass a valid API key.","status":"INVALID_ARGUMENT","details":[{"@type":"type.googleapis.com/google.rpc.ErrorInfo","reason":"API_KEY_INVALID"}]}})",
        "shop");
    EXPECT_EQ(s.failure, Failure::Auth);
    EXPECT_TRUE(s.error.contains("API key was rejected"));

    s = AiQueryAssistant::parseResponse(
        Provider::Gemini, 503, R"({"error":{"code":503,"message":"The model is overloaded.","status":"UNAVAILABLE"}})", "shop");
    EXPECT_EQ(s.failure, Failure::Overloaded);
    EXPECT_TRUE(s.retryable);
}

TEST(AiQueryAssistantTest, anthropic_quota_errors)
{
    auto s = AiQueryAssistant::parseResponse(
        Provider::Anthropic, 429, R"({"type":"error","error":{"type":"rate_limit_error","message":"slow down"}})", "shop", "23");
    EXPECT_EQ(s.failure, Failure::RateLimited);
    EXPECT_EQ(s.retryAfterSec, 23);
    EXPECT_TRUE(s.retryable);

    s = AiQueryAssistant::parseResponse(
        Provider::Anthropic, 400,
        R"({"type":"error","error":{"type":"invalid_request_error","message":"Your credit balance is too low to access the Anthropic API."}})",
        "shop");
    EXPECT_EQ(s.failure, Failure::QuotaExhausted);
    EXPECT_TRUE(s.allModels);
    EXPECT_FALSE(s.retryable);

    s = AiQueryAssistant::parseResponse(
        Provider::Anthropic, 404, R"({"type":"error","error":{"type":"not_found_error","message":"model: claude-old"}})", "shop");
    EXPECT_EQ(s.failure, Failure::ModelUnavailable);
}

TEST(AiQueryAssistantTest, model_lists)
{
    const QByteArray gemini = R"({"models":[
        {"name":"models/gemini-2.5-flash","supportedGenerationMethods":["generateContent","countTokens"]},
        {"name":"models/gemini-3.1-pro-preview","supportedGenerationMethods":["generateContent"]},
        {"name":"models/gemini-3.8-flash","supportedGenerationMethods":["generateContent"]},
        {"name":"models/gemini-4-argon","supportedGenerationMethods":["generateContent"]},
        {"name":"models/gemini-embedding-001","supportedGenerationMethods":["embedContent"]},
        {"name":"models/gemini-3.8-flash-tts","supportedGenerationMethods":["generateContent"]},
        {"name":"models/imagen-5","supportedGenerationMethods":["predict"]}]})";
    auto list = AiQueryAssistant::parseModelList(Provider::Gemini, 200, gemini);
    ASSERT_TRUE(list.ok);
    EXPECT_EQ(list.models, (QStringList{"gemini-4-argon", "gemini-3.8-flash", "gemini-3.1-pro-preview", "gemini-2.5-flash"}));

    list = AiQueryAssistant::parseModelList(
        Provider::Anthropic, 200,
        R"({"data":[{"type":"model","id":"claude-opus-5-5"},{"type":"model","id":"claude-haiku-5-5"}],"has_more":false})");
    ASSERT_TRUE(list.ok);
    EXPECT_EQ(list.models, (QStringList{"claude-opus-5-5", "claude-haiku-5-5"}));

    list = AiQueryAssistant::parseModelList(
        Provider::Anthropic, 401, R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})");
    EXPECT_FALSE(list.ok);
    EXPECT_TRUE(list.error.contains("API key was rejected"));
}

TEST(AiQueryAssistantTest, fallback_candidates)
{
    // Availability unknown: preferred first, then the built-in order.
    auto c = AiQueryAssistant::fallbackCandidates(Provider::Gemini, "gemini-4-argon", {}, 3);
    EXPECT_EQ(c, (QStringList{"gemini-4-argon", "gemini-flash-latest", "gemini-3.5-flash-lite"}));

    // Built-ins the provider no longer lists are left out; aliases stay.
    c = AiQueryAssistant::fallbackCandidates(Provider::Gemini, "gemini-3.1-pro-preview",
                                             {"gemini-3.1-pro-preview", "gemini-4-argon"}, 3);
    EXPECT_EQ(c, (QStringList{"gemini-3.1-pro-preview", "gemini-flash-latest", "gemini-4-argon"}));

    EXPECT_EQ(AiQueryAssistant::fallbackCandidates(Provider::Anthropic, "claude-sonnet-5-5", {}, 1),
              QStringList{"claude-sonnet-5-5"});
}

TEST(AiQueryAssistantTest, cooldowns)
{
    AiQueryAssistant::ModelCooldowns cd;
    const QDateTime now(QDate(2026, 10, 25), QTime(20, 0), QTimeZone::UTC);
    const QStringList models{"gemini-4-argon", "gemini-flash-latest", "gemini-3.5-flash-lite"};

    AiQueryAssistant::Suggestion daily;
    daily.failure = Failure::QuotaExhausted;
    daily.retryAfterSec = 40;   // Gemini's RetryInfo; the daily reset wins
    const QDateTime until = cd.record(Provider::Gemini, "gemini-4-argon", daily, now);
    // 20:00 UTC on 25 Oct 2026 is 13:00 PDT; midnight Pacific is 07:00 UTC (PDT until 1 Nov).
    EXPECT_EQ(until, QDateTime(QDate(2026, 10, 26), QTime(7, 0), QTimeZone::UTC));
    EXPECT_TRUE(cd.blocked(Provider::Gemini, "gemini-4-argon", now).until.isValid());
    EXPECT_FALSE(cd.blocked(Provider::Gemini, "gemini-4-argon", until.addSecs(1)).until.isValid());
    EXPECT_FALSE(cd.blocked(Provider::Anthropic, "gemini-4-argon", now).until.isValid());
    EXPECT_EQ(cd.pick(Provider::Gemini, models, {}, now), QStringLiteral("gemini-flash-latest"));
    EXPECT_EQ(cd.pick(Provider::Gemini, models, {"gemini-flash-latest"}, now), QStringLiteral("gemini-3.5-flash-lite"));

    AiQueryAssistant::Suggestion minute;
    minute.failure = Failure::RateLimited;
    minute.retryAfterSec = 30;
    EXPECT_EQ(cd.record(Provider::Gemini, "gemini-flash-latest", minute, now), now.addSecs(30));
    EXPECT_EQ(cd.pick(Provider::Gemini, models, {}, now.addSecs(31)), QStringLiteral("gemini-flash-latest"));

    // A project-wide quota pauses every model of the provider.
    AiQueryAssistant::Suggestion project = daily;
    project.allModels = true;
    cd.record(Provider::Gemini, "gemini-3.5-flash-lite", project, now);
    EXPECT_TRUE(cd.pick(Provider::Gemini, models, {}, now).isEmpty());

    // Errors another model cannot fix are not remembered.
    AiQueryAssistant::Suggestion auth;
    auth.failure = Failure::Auth;
    EXPECT_FALSE(cd.record(Provider::Anthropic, "claude-opus-5-5", auth, now).isValid());
    EXPECT_FALSE(cd.blocked(Provider::Anthropic, "claude-opus-5-5", now).until.isValid());

    cd.clear(Provider::Gemini, "gemini-4-argon");
    EXPECT_EQ(cd.pick(Provider::Gemini, models, {}, now), QStringLiteral("gemini-4-argon"));
}

namespace
{
    SchemaField relatedField(const char *path, double presence, const char *type = "String")
    {
        SchemaField f;
        f.path = QString::fromLatin1(path);
        f.name = f.path.section(QLatin1Char('.'), -1);
        f.presence = presence;
        f.types = {{QString::fromLatin1(type), 100}};
        return f;
    }

    SchemaResult relatedResult(std::vector<SchemaField> fields, qint64 documents = 100)
    {
        SchemaResult r;
        r.ok = true;
        r.totalDocuments = documents;
        r.sampledDocuments = documents;
        r.fields = std::move(fields);
        return r;
    }
}

TEST(AiQueryAssistantTest, related_collections_are_ranked_by_relevance)
{
    const std::vector<SchemaField> orders{relatedField("_id", 1, "ObjectId"), relatedField("customerId", 1, "ObjectId"),
                                          relatedField("total", 1, "Double")};
    std::map<QString, SchemaResult> others;
    others["customers"] = relatedResult({relatedField("_id", 1, "ObjectId"), relatedField("name", 1)});
    others["invoices"] = relatedResult({relatedField("_id", 1, "ObjectId"), relatedField("orderId", 1, "ObjectId"),
                                        relatedField("amount", 1, "Double")});
    others["tags"] = relatedResult({relatedField("label", 1)});
    others["warehouses"] = relatedResult({relatedField("city", 1), relatedField("capacity", 1, "Int32")});
    others["orders"] = relatedResult({});   // the open collection itself is never related

    auto names = [](const std::vector<AiQueryAssistant::RelatedCollection> &related) {
        QStringList n;
        for (const auto &r : related)
            n << r.name;
        return n;
    };
    // Named in the question (singular is enough), then what orders points to,
    // then what points to orders, then the rest smallest first.
    EXPECT_EQ(names(AiQueryAssistant::relatedCollections("orders", orders, "orders per warehouse", others)),
              (QStringList{"warehouses", "customers", "invoices", "tags"}));
    EXPECT_EQ(names(AiQueryAssistant::relatedCollections("orders", orders, "", others)),
              (QStringList{"customers", "invoices", "tags", "warehouses"}));
}

TEST(AiQueryAssistantTest, related_collections_stay_within_the_budget)
{
    std::vector<SchemaField> wide;
    for (int i = 0; i < 40; ++i)
        wide.push_back(relatedField(qPrintable(QStringLiteral("field%1").arg(i, 2, 10, QLatin1Char('0'))),
                                    i < 5 ? 1.0 : 0.5));
    std::map<QString, SchemaResult> others;
    others["customers"] = relatedResult(wide);
    others["tags"] = relatedResult({relatedField("label", 1)});

    // A budget for about ten fields: customers is cut to its common fields, then nothing fits.
    const auto related = AiQueryAssistant::relatedCollections("orders", {}, "customers", others, 400);
    ASSERT_EQ(related.size(), 1u);
    EXPECT_EQ(related[0].name, QStringLiteral("customers"));
    EXPECT_GT(related[0].omittedFields, 0);
    EXPECT_EQ(related[0].fields.size() + related[0].omittedFields, 40u);
    EXPECT_EQ(related[0].fields.front().path, QStringLiteral("field00"));   // the common ones are kept

    AiQueryAssistant::Context c = sampleContext(false);
    c.related = related;
    const QString summary = AiQueryAssistant::schemaSummary(c);
    EXPECT_TRUE(summary.contains("Related collections of the same database"));
    EXPECT_TRUE(summary.contains("Collection: customers (about 100 documents)"));
    EXPECT_TRUE(summary.contains(QStringLiteral("... %1 more field path(s) not listed").arg(related[0].omittedFields)));
    EXPECT_TRUE(AiQueryAssistant::schemaSummary(sampleContext(false)).indexOf("Related collections") < 0);

    EXPECT_TRUE(AiQueryAssistant::relatedCollections("orders", {}, "", others, 0).empty());
}

TEST(AiQueryAssistantTest, lookups_must_use_described_collections)
{
    EXPECT_EQ(AiQueryAssistant::referencedCollections(
                  "db.getCollection('orders').aggregate([{ $lookup: { from: 'customers', localField: 'customerId', "
                  "foreignField: '_id', as: 'c' } }, { $unionWith: 'archive' }, "
                  "{ $unionWith: { coll: \"old\", pipeline: [] } }, { \"$graphLookup\": { \"from\": \"tree\" } }])"),
              (QStringList{"customers", "tree", "archive", "old"}));
    EXPECT_TRUE(AiQueryAssistant::referencedCollections("db.getCollection('orders').find({})").isEmpty());

    const QByteArray join = okResponse(
        "aggregate",
        "db.getCollection('orders').aggregate([{ $lookup: { from: 'customers', localField: 'customerId', "
        "foreignField: '_id', as: 'customer' } }])",
        "Orders with their customer.");
    auto s = AiQueryAssistant::parseResponse(AiQueryAssistant::Provider::Anthropic, 200, join, "shop", {},
                                             {"orders", "customers"});
    EXPECT_TRUE(s.ok) << s.error.toStdString();
    s = AiQueryAssistant::parseResponse(AiQueryAssistant::Provider::Anthropic, 200, join, "shop", {}, {"orders"});
    EXPECT_FALSE(s.ok);
    EXPECT_TRUE(s.error.contains("customers"));
    // No list: not checked (as before).
    EXPECT_TRUE(AiQueryAssistant::parseResponse(200, join, "shop").ok);
    EXPECT_TRUE(AiQueryAssistant::systemPrompt().contains("related collections listed"));
}

TEST(AiQueryAssistantTest, cloud_provider_identity)
{
    EXPECT_EQ(AiQueryAssistant::providerId(AiQueryAssistant::Provider::RagoCloud),
              QStringLiteral("ragoshell_cloud"));
    EXPECT_EQ(AiQueryAssistant::providerFromId(QStringLiteral("ragoshell_cloud")),
              AiQueryAssistant::Provider::RagoCloud);
    EXPECT_EQ(AiQueryAssistant::providerName(AiQueryAssistant::Provider::RagoCloud),
              QStringLiteral("RagoShell AI"));
    EXPECT_TRUE(AiQueryAssistant::providerIds().contains(QStringLiteral("ragoshell_cloud")));
    EXPECT_TRUE(AiQueryAssistant::apiKeyEnvironmentVariables(
                    AiQueryAssistant::Provider::RagoCloud).isEmpty());
    EXPECT_EQ(AiQueryAssistant::defaultModel(AiQueryAssistant::Provider::RagoCloud),
              QStringLiteral("ragoshell-ai"));
}

TEST(AiQueryAssistantTest, cloud_request_body_shape)
{
    const QByteArray body = AiQueryAssistant::cloudRequestBody(sampleContext(false),
                                                               QStringLiteral("recent orders"));
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    EXPECT_EQ(root.value("provider").toString(), QStringLiteral("anthropic"));
    EXPECT_EQ(root.value("max_tokens").toInteger(), qint64(4096));
    const QJsonArray msgs = root.value("messages").toArray();
    ASSERT_EQ(msgs.size(), 2);
    EXPECT_EQ(msgs[0].toObject().value("role").toString(), QStringLiteral("system"));
    EXPECT_EQ(msgs[1].toObject().value("role").toString(), QStringLiteral("user"));
    EXPECT_TRUE(msgs[1].toObject().value("content").toString()
                    .contains(QStringLiteral("recent orders")));
}

TEST(AiQueryAssistantTest, cloud_response_happy_path)
{
    const QByteArray body = R"({"content":"{\"kind\":\"find\",\"script\":\"db.orders.find({})\",
        \"explanation\":\"all orders\"}","promptTokens":120,"completionTokens":40,
        "remainingMonthlyQuota":1999800})";
    const auto s = AiQueryAssistant::parseCloudResponse(200, body, QStringLiteral("shop"),
                                                      {}, {QStringLiteral("orders")});
    ASSERT_TRUE(s.ok);
    EXPECT_EQ(s.kind, QStringLiteral("find"));
    EXPECT_EQ(s.script, QStringLiteral("db.orders.find({})"));
    EXPECT_EQ(s.explanation, QStringLiteral("all orders"));
}

TEST(AiQueryAssistantTest, cloud_response_error_mapping)
{
    const auto quota = AiQueryAssistant::parseCloudResponse(
        402, R"({"error":{"code":"ai_quota_exceeded","message":"x"},
                 "quota":{"limit":2000000,"used":2000000,"remaining":0}})",
        QStringLiteral("shop"), {}, {});
    EXPECT_EQ(quota.failure, AiQueryAssistant::Failure::QuotaExhausted);
    EXPECT_TRUE(quota.allModels);
    EXPECT_FALSE(quota.retryable);

    const auto entitled = AiQueryAssistant::parseCloudResponse(
        403, R"({"error":{"code":"ai_entitlement_required"}})", QStringLiteral("shop"), {}, {});
    EXPECT_EQ(entitled.failure, AiQueryAssistant::Failure::Auth);

    const auto unauth = AiQueryAssistant::parseCloudResponse(
        401, R"({"error":{"code":"unauthorized"}})", QStringLiteral("shop"), {}, {});
    EXPECT_EQ(unauth.failure, AiQueryAssistant::Failure::Auth);

    const auto limited = AiQueryAssistant::parseCloudResponse(
        429, R"({"error":{"code":"rate_limited"}})", QStringLiteral("shop"), "15", {});
    EXPECT_EQ(limited.failure, AiQueryAssistant::Failure::RateLimited);
    EXPECT_TRUE(limited.retryable);
    EXPECT_EQ(limited.retryAfterSec, 15);

    const auto upstream = AiQueryAssistant::parseCloudResponse(
        502, R"({"error":{"code":"ai_upstream_error"}})", QStringLiteral("shop"), {}, {});
    EXPECT_EQ(upstream.failure, AiQueryAssistant::Failure::Overloaded);
    EXPECT_TRUE(upstream.retryable);
}
