#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <QUrl>

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

// Preferences › Paste (docs: T9): one line of printable ASCII, trimmed.
TEST(AiQueryAssistantTest, pasted_key_checks)
{
    using AiQueryAssistant::PastedKey;
    QString key;
    EXPECT_EQ(AiQueryAssistant::checkPastedKey("  sk-ant-api03-AbC_d-1\n", &key), PastedKey::Ok);
    EXPECT_EQ(key, "sk-ant-api03-AbC_d-1");
    EXPECT_EQ(AiQueryAssistant::checkPastedKey("AIzaSyD-x_y", &key), PastedKey::Ok);
    EXPECT_EQ(key, "AIzaSyD-x_y");

    key = "unchanged";
    EXPECT_EQ(AiQueryAssistant::checkPastedKey("", &key), PastedKey::Empty);
    EXPECT_EQ(AiQueryAssistant::checkPastedKey(" \t\r\n ", &key), PastedKey::Empty);
    EXPECT_EQ(AiQueryAssistant::checkPastedKey("sk-one\nsk-two", &key), PastedKey::MultiLine);
    EXPECT_EQ(AiQueryAssistant::checkPastedKey("sk-one\rsk-two", &key), PastedKey::MultiLine);
    EXPECT_EQ(AiQueryAssistant::checkPastedKey("export KEY=sk", &key), PastedKey::InvalidCharacters);
    EXPECT_EQ(AiQueryAssistant::checkPastedKey("sk-a\tb", &key), PastedKey::InvalidCharacters);
    EXPECT_EQ(AiQueryAssistant::checkPastedKey(QString("sk-ab") + QChar(0x200B) + "c", &key), PastedKey::InvalidCharacters);
    EXPECT_EQ(AiQueryAssistant::checkPastedKey(QString("sk-") + QChar(0x00E7) + "ok", &key), PastedKey::InvalidCharacters);
    EXPECT_EQ(AiQueryAssistant::checkPastedKey(QString(AiQueryAssistant::kMaxApiKeyLength, 'a'), &key), PastedKey::Ok);
    key = "unchanged";
    EXPECT_EQ(AiQueryAssistant::checkPastedKey(QString(AiQueryAssistant::kMaxApiKeyLength + 1, 'a'), &key),
              PastedKey::TooLong);
    EXPECT_EQ(key, "unchanged"); // rejected text never reaches the field
    EXPECT_EQ(AiQueryAssistant::checkPastedKey("sk-null-ok", nullptr), PastedKey::Ok);
}

// Preferences › Test connection (docs: T5, T6, T10): one outcome per case.
TEST(AiQueryAssistantTest, connection_check_outcomes)
{
    using AiQueryAssistant::ConnectionStatus;
    auto c = AiQueryAssistant::checkConnection(
        Provider::Anthropic, 200, R"({"data":[{"id":"claude-opus-5-5"},{"id":"claude-haiku-5-5"}]})");
    EXPECT_EQ(c.status, ConnectionStatus::Connected);
    EXPECT_EQ(c.list.models.size(), 2);

    c = AiQueryAssistant::checkConnection(Provider::Anthropic, 200, R"({"data":[{"id":"other-model"}]})");
    EXPECT_EQ(c.status, ConnectionStatus::NoUsableModels);
    c = AiQueryAssistant::checkConnection(Provider::Gemini, 200, "not json");
    EXPECT_EQ(c.status, ConnectionStatus::NoUsableModels);

    c = AiQueryAssistant::checkConnection(
        Provider::Anthropic, 401, R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})");
    EXPECT_EQ(c.status, ConnectionStatus::KeyRejected);
    c = AiQueryAssistant::checkConnection(
        Provider::Anthropic, 403, R"({"type":"error","error":{"type":"permission_error","message":"no"}})");
    EXPECT_EQ(c.status, ConnectionStatus::KeyRejected);
    // Gemini answers an invalid key with 400 API_KEY_INVALID.
    c = AiQueryAssistant::checkConnection(
        Provider::Gemini, 400,
        R"({"error":{"code":400,"message":"API key not valid. Please pass a valid API key.","status":"INVALID_ARGUMENT",
            "details":[{"@type":"type.googleapis.com/google.rpc.ErrorInfo","reason":"API_KEY_INVALID"}]}})");
    EXPECT_EQ(c.status, ConnectionStatus::KeyRejected);

    c = AiQueryAssistant::checkConnection(Provider::Anthropic, 404, "<html>Not Found</html>");
    EXPECT_EQ(c.status, ConnectionStatus::NoModelList);
    c = AiQueryAssistant::checkConnection(Provider::Gemini, 429, R"({"error":{"code":429,"status":"RESOURCE_EXHAUSTED"}})");
    EXPECT_EQ(c.status, ConnectionStatus::RateLimited);
    c = AiQueryAssistant::checkConnection(Provider::Anthropic, 500, "");
    EXPECT_EQ(c.status, ConnectionStatus::ProviderError);
    EXPECT_FALSE(c.list.error.isEmpty());

    for (int status : {301, 302, 307, 308}) {
        c = AiQueryAssistant::checkConnection(Provider::Anthropic, status, "");
        EXPECT_EQ(c.status, ConnectionStatus::RedirectRefused) << status;
        EXPECT_TRUE(c.list.models.isEmpty());
    }
    c = AiQueryAssistant::checkConnection(Provider::Gemini, 0, "");
    EXPECT_EQ(c.status, ConnectionStatus::Unreachable);
}

TEST(AiQueryAssistantTest, api_key_pages_are_https_provider_pages)
{
    for (Provider p : {Provider::Anthropic, Provider::Gemini}) {
        const QUrl url(AiQueryAssistant::apiKeyPageUrl(p));
        EXPECT_EQ(url.scheme(), "https");
        EXPECT_TRUE(url.query().isEmpty());
    }
    EXPECT_EQ(QUrl(AiQueryAssistant::apiKeyPageUrl(Provider::Anthropic)).host(), "platform.claude.com");
    EXPECT_EQ(QUrl(AiQueryAssistant::apiKeyPageUrl(Provider::Gemini)).host(), "aistudio.google.com");
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

// BYOK only: the hosted "RagoShell AI" provider is gone. Requests go from
// the device straight to the chosen provider and never carry the RagoShell
// account token (docs: hosted AI removal, E1/E2).
TEST(AiQueryAssistantTest, byok_only_providers)
{
    EXPECT_EQ(AiQueryAssistant::providerIds(),
              (QStringList{QStringLiteral("anthropic"), QStringLiteral("gemini"), QStringLiteral("openai_compat"),
                           QStringLiteral("ollama")}));
    const QString removed = QString::fromLatin1(AiQueryAssistant::kRemovedHostedProviderId);
    EXPECT_FALSE(AiQueryAssistant::providerIds().contains(removed));
    // Unknown ids read as Anthropic; SettingsManager::migrateAiProvider
    // turns the assistant off before a saved "ragoshell_cloud" can get here.
    EXPECT_EQ(AiQueryAssistant::providerFromId(removed), Provider::Anthropic);
    for (const QString &id : AiQueryAssistant::providerIds())
        EXPECT_FALSE(AiQueryAssistant::providerName(AiQueryAssistant::providerFromId(id))
                         .contains(QStringLiteral("RagoShell")));
}

TEST(AiQueryAssistantTest, requests_never_carry_an_account_token)
{
    // Tests may point every provider at a local mock server.
    const bool overridden = qEnvironmentVariableIsSet("RAGOSHELL_AI_BASE_URL");
    for (const Provider p : {Provider::Anthropic, Provider::Gemini}) {
        const QString model = AiQueryAssistant::defaultModel(p);
        for (const auto &[name, value] : AiQueryAssistant::requestHeaders(p, QStringLiteral("user-key"), model)) {
            const QByteArray lower = name.toLower();
            EXPECT_NE(lower, QByteArrayLiteral("authorization")) << name.constData();
            EXPECT_FALSE(value.startsWith("Bearer ")) << name.constData();
        }
        // Straight to the provider, never through a RagoShell server.
        if (overridden)
            continue;
        for (const QString &url : {AiQueryAssistant::endpointUrl(p, model), AiQueryAssistant::modelsUrl(p)}) {
            const QString host = QUrl(url).host();
            EXPECT_TRUE(host == QLatin1String("api.anthropic.com")
                        || host == QLatin1String("generativelanguage.googleapis.com"))
                << url.toStdString();
            EXPECT_FALSE(url.contains(QStringLiteral("/v1/ai/generate"))) << url.toStdString();
        }
    }
}

// ---- B6: OpenAI-compatible endpoint ------------------------------------

// docs: T2, T3. One row per rule.
TEST(AiQueryAssistantTest, base_url_checks)
{
    using AiQueryAssistant::BaseUrlError;
    struct Row
    {
        const char *input;
        bool hasKey;
        bool allowPrivateHttp;
        BaseUrlError error;
        const char *normalized;
    };
    const Row rows[] = {
        {"https://api.openai.com/v1", true, true, BaseUrlError::None, "https://api.openai.com/v1"},
        {"  HTTPS://API.OpenAI.com/v1///  ", true, true, BaseUrlError::None, "https://api.openai.com/v1"},
        {"https://api.deepseek.com", true, true, BaseUrlError::None, "https://api.deepseek.com"},
        {"https://host.example:8443/api/v1/", true, true, BaseUrlError::None, "https://host.example:8443/api/v1"},
        // Loopback: plain http allowed, with or without a key, on every platform.
        {"http://localhost:1234/v1", true, false, BaseUrlError::None, "http://localhost:1234/v1"},
        {"http://127.0.0.1:11434/v1", true, false, BaseUrlError::None, "http://127.0.0.1:11434/v1"},
        {"http://[::1]:8080/v1", true, false, BaseUrlError::None, "http://[::1]:8080/v1"},
        // Private network: plain http only without a key, and not on Android.
        {"http://192.168.1.20:1234/v1", false, true, BaseUrlError::None, "http://192.168.1.20:1234/v1"},
        {"http://gpu-box.local:1234/v1", false, true, BaseUrlError::None, "http://gpu-box.local:1234/v1"},
        // A single-label name may be answered by any host (DNS suffix, LLMNR).
        {"http://gpu-box:1234/v1", false, true, BaseUrlError::PlainHttpPublic, ""},
        {"https://gpu-box:1234/v1", true, true, BaseUrlError::None, "https://gpu-box:1234/v1"},
        {"http://192.168.1.20:1234/v1", true, true, BaseUrlError::PlainHttpWithKey, ""},
        {"http://10.0.0.5/v1", false, false, BaseUrlError::PlainHttpBlocked, ""},
        // Public address over plain http: never.
        {"http://api.openai.com/v1", false, true, BaseUrlError::PlainHttpPublic, ""},
        {"http://8.8.8.8/v1", false, true, BaseUrlError::PlainHttpPublic, ""},
        // Shape.
        {"", true, true, BaseUrlError::Empty, ""},
        {"   ", true, true, BaseUrlError::Empty, ""},
        {"api.openai.com/v1", true, true, BaseUrlError::Malformed, ""},
        {"localhost:1234/v1", true, true, BaseUrlError::Malformed, ""},
        {"ftp://host.example/v1", true, true, BaseUrlError::Malformed, ""},
        {"file:///etc/passwd", true, true, BaseUrlError::Malformed, ""},
        {"javascript:alert(1)", true, true, BaseUrlError::Malformed, ""},
        {"https://", true, true, BaseUrlError::Malformed, ""},
        {"https://host .example/v1", true, true, BaseUrlError::Malformed, ""},
        {"https://host.example/v1\nX-Evil: 1", true, true, BaseUrlError::Malformed, ""},
        {"https://user:pass@host.example/v1", true, true, BaseUrlError::HasCredentials, ""},
        {"https://api.openai.com@evil.example/v1", true, true, BaseUrlError::HasCredentials, ""},
        {"https://host.example/v1?key=abc", true, true, BaseUrlError::HasQueryOrFragment, ""},
        {"https://host.example/v1#x", true, true, BaseUrlError::HasQueryOrFragment, ""},
    };
    for (const Row &r : rows) {
        const auto c = AiQueryAssistant::checkBaseUrl(QString::fromUtf8(r.input), r.hasKey, r.allowPrivateHttp);
        EXPECT_EQ(c.error, r.error) << r.input;
        EXPECT_EQ(c.normalized, QString::fromUtf8(r.normalized)) << r.input;
    }
    const QString longUrl = QStringLiteral("https://host.example/") + QString(AiQueryAssistant::kMaxBaseUrlLength, 'a');
    EXPECT_EQ(AiQueryAssistant::checkBaseUrl(longUrl, true, true).error, BaseUrlError::TooLong);
}

// docs: T1. The origin a saved key belongs to.
TEST(AiQueryAssistantTest, url_origins)
{
    EXPECT_EQ(AiQueryAssistant::urlOrigin("https://api.openai.com/v1"), "https://api.openai.com:443");
    EXPECT_EQ(AiQueryAssistant::urlOrigin("https://API.openai.com:443/other"), "https://api.openai.com:443");
    EXPECT_EQ(AiQueryAssistant::urlOrigin("http://localhost:1234/v1"), "http://localhost:1234");
    EXPECT_EQ(AiQueryAssistant::urlOrigin("http://[::1]:8080/v1"), "http://[::1]:8080");
    EXPECT_NE(AiQueryAssistant::urlOrigin("http://api.openai.com/v1"), AiQueryAssistant::urlOrigin("https://api.openai.com/v1"));
    EXPECT_TRUE(AiQueryAssistant::urlOrigin("").isEmpty());
    EXPECT_TRUE(AiQueryAssistant::urlOrigin("not a url").isEmpty());
    EXPECT_EQ(AiQueryAssistant::urlHost("https://openrouter.ai/api/v1"), "openrouter.ai");
}

// docs: T12. An environment key only at a preset's own address.
TEST(AiQueryAssistantTest, presets_and_environment_keys)
{
    const auto presets = AiQueryAssistant::openAiPresets();
    ASSERT_EQ(presets.size(), 4);
    for (const auto &p : presets) {
        // Every preset address passes the check unchanged.
        EXPECT_EQ(AiQueryAssistant::checkBaseUrl(p.baseUrl, false, true).normalized, p.baseUrl) << p.id.toStdString();
        if (!p.keyPageUrl.isEmpty())
            EXPECT_EQ(QUrl(p.keyPageUrl).scheme(), "https") << p.id.toStdString();
        EXPECT_EQ(AiQueryAssistant::presetForBaseUrl(p.baseUrl)->id, p.id);
    }
    const Provider c = Provider::OpenAiCompatible;
    EXPECT_EQ(AiQueryAssistant::apiKeyEnvironmentVariables(c, "https://api.openai.com/v1"),
              QStringList{"OPENAI_API_KEY"});
    EXPECT_TRUE(AiQueryAssistant::apiKeyEnvironmentVariables(c, "https://evil.example/v1").isEmpty());
    EXPECT_TRUE(AiQueryAssistant::apiKeyEnvironmentVariables(c, "https://api.openai.com/v1/x").isEmpty());
    EXPECT_TRUE(AiQueryAssistant::apiKeyEnvironmentVariables(c, "http://localhost:1234/v1").isEmpty());
    EXPECT_TRUE(AiQueryAssistant::apiKeyEnvironmentVariables(c, "").isEmpty());
    EXPECT_EQ(AiQueryAssistant::presetForBaseUrl("https://my.server/v1"), nullptr);

    EXPECT_EQ(AiQueryAssistant::apiKeyPageUrl(c, "https://openrouter.ai/api/v1"), "https://openrouter.ai/settings/keys");
    EXPECT_TRUE(AiQueryAssistant::apiKeyPageUrl(c, "http://localhost:1234/v1").isEmpty());
    EXPECT_TRUE(AiQueryAssistant::apiKeyPageUrl(c, "https://my.server/v1").isEmpty());
}

TEST(AiQueryAssistantTest, openai_compatible_request)
{
    const Provider c = Provider::OpenAiCompatible;
    EXPECT_EQ(AiQueryAssistant::providerFromId("openai_compat"), c);
    EXPECT_EQ(AiQueryAssistant::providerId(c), "openai_compat");
    EXPECT_TRUE(AiQueryAssistant::hasConfigurableBaseUrl(c));
    EXPECT_FALSE(AiQueryAssistant::hasConfigurableBaseUrl(Provider::Anthropic));
    EXPECT_TRUE(AiQueryAssistant::modelIds(c).isEmpty());
    EXPECT_TRUE(AiQueryAssistant::defaultModel(c).isEmpty());
    EXPECT_EQ(AiQueryAssistant::fallbackCandidates(c, "gpt-x", {}, 3), QStringList{"gpt-x"});

    EXPECT_EQ(AiQueryAssistant::endpointUrl(c, "m", "https://openrouter.ai/api/v1"),
              "https://openrouter.ai/api/v1/chat/completions");
    EXPECT_EQ(AiQueryAssistant::modelsUrl(c, "http://localhost:1234/v1"), "http://localhost:1234/v1/models");

    // Bearer only with a key; nothing else that identifies the user.
    auto headers = AiQueryAssistant::requestHeaders(c, QString(), "m");
    ASSERT_EQ(headers.size(), 1u);
    EXPECT_EQ(headers[0].first, QByteArray("content-type"));
    headers = AiQueryAssistant::requestHeaders(c, "sk-abc", "m");
    ASSERT_EQ(headers.size(), 2u);
    EXPECT_EQ(headers[1].first, QByteArray("authorization"));
    EXPECT_EQ(headers[1].second, QByteArray("Bearer sk-abc"));

    const QJsonObject body = QJsonDocument::fromJson(
        AiQueryAssistant::requestBody(c, "deepseek-chat", sampleContext(false), "orders shipped today")).object();
    EXPECT_EQ(body.value("model").toString(), "deepseek-chat");
    EXPECT_EQ(body.value("max_tokens").toInt(), 4096);
    EXPECT_FALSE(body.contains("response_format"));
    EXPECT_FALSE(body.contains("temperature"));
    EXPECT_FALSE(body.contains("fallbacks"));
    EXPECT_FALSE(body.contains("output_config"));
    const QJsonArray messages = body.value("messages").toArray();
    ASSERT_EQ(messages.size(), 2);
    EXPECT_EQ(messages[0].toObject().value("role").toString(), "system");
    EXPECT_TRUE(messages[0].toObject().value("content").toString().contains("Answer with one JSON object"));
    EXPECT_EQ(messages[1].toObject().value("role").toString(), "user");
    EXPECT_TRUE(messages[1].toObject().value("content").toString().contains("orders shipped today"));
}

TEST(AiQueryAssistantTest, openai_compatible_responses)
{
    const Provider c = Provider::OpenAiCompatible;
    auto answer = [](const QString &content, const QString &finish = QStringLiteral("stop")) {
        QJsonObject message{{"role", "assistant"}, {"content", content}};
        QJsonObject choice{{"index", 0}, {"message", message}, {"finish_reason", finish}};
        return QJsonDocument(QJsonObject{{"choices", QJsonArray{choice}}}).toJson();
    };
    const QString json = QStringLiteral(
        R"J({"kind":"find","script":"db.getCollection('orders').find({status: 'shipped'})","explanation":"ok"})J");

    auto s = AiQueryAssistant::parseResponse(c, 200, answer(json), "shop");
    EXPECT_TRUE(s.ok) << s.error.toStdString();
    // A Markdown fence or a leading sentence is tolerated.
    s = AiQueryAssistant::parseResponse(c, 200, answer("Here you go:\n```json\n" + json + "\n```"), "shop");
    EXPECT_TRUE(s.ok) << s.error.toStdString();
    EXPECT_TRUE(s.script.startsWith("db.getCollection('orders').find"));
    // The read-only check still applies.
    s = AiQueryAssistant::parseResponse(
        c, 200, answer(QStringLiteral(R"J({"kind":"find","script":"db.getCollection('orders').drop()","explanation":""})J")),
        "shop");
    EXPECT_FALSE(s.ok);
    s = AiQueryAssistant::parseResponse(c, 200, answer("not json at all"), "shop");
    EXPECT_FALSE(s.ok);
    s = AiQueryAssistant::parseResponse(c, 200, answer(json, "length"), "shop");
    EXPECT_FALSE(s.ok);
    s = AiQueryAssistant::parseResponse(c, 200, answer(json, "content_filter"), "shop");
    EXPECT_EQ(s.failure, Failure::Blocked);
    s = AiQueryAssistant::parseResponse(c, 200, "{}", "shop");
    EXPECT_FALSE(s.ok);

    s = AiQueryAssistant::parseResponse(c, 401, R"({"error":{"message":"Incorrect API key","type":"invalid_request_error"}})", "shop");
    EXPECT_EQ(s.failure, Failure::Auth);
    EXPECT_TRUE(s.error.contains("Incorrect API key"));
    s = AiQueryAssistant::parseResponse(c, 429, R"({"error":{"message":"quota","code":"insufficient_quota"}})", "shop");
    EXPECT_EQ(s.failure, Failure::QuotaExhausted);
    EXPECT_TRUE(s.allModels);
    s = AiQueryAssistant::parseResponse(c, 402, R"({"error":{"message":"Insufficient credits"}})", "shop");
    EXPECT_EQ(s.failure, Failure::QuotaExhausted);
    s = AiQueryAssistant::parseResponse(c, 429, "{}", "shop", "12");
    EXPECT_EQ(s.failure, Failure::RateLimited);
    EXPECT_EQ(s.retryAfterSec, 12);
    EXPECT_TRUE(s.retryable);
    s = AiQueryAssistant::parseResponse(c, 404, R"({"error":"model not found"})", "shop");
    EXPECT_EQ(s.failure, Failure::ModelUnavailable);
    EXPECT_TRUE(s.error.contains("model not found"));
    s = AiQueryAssistant::parseResponse(c, 503, "<html>", "shop");
    EXPECT_EQ(s.failure, Failure::Overloaded);
}

// docs: T6. A redirect is an error for every provider, never retried.
TEST(AiQueryAssistantTest, redirects_are_errors)
{
    for (const Provider p : {Provider::Anthropic, Provider::Gemini, Provider::OpenAiCompatible}) {
        for (int status : {301, 302, 303, 307, 308}) {
            const auto s = AiQueryAssistant::parseResponse(p, status, "", "shop");
            EXPECT_FALSE(s.ok);
            EXPECT_FALSE(s.retryable) << status;
            EXPECT_EQ(s.failure, Failure::Other) << status;
            EXPECT_TRUE(s.error.contains(QString::number(status)));
        }
    }
}

TEST(AiQueryAssistantTest, openai_compatible_model_list)
{
    const Provider c = Provider::OpenAiCompatible;
    const QByteArray body = R"({"object":"list","data":[
        {"id":"gpt-5.5"},{"id":"text-embedding-3-large"},{"id":"whisper-1"},{"id":"dall-e-3"},
        {"id":"tts-1"},{"id":"omni-moderation-latest"},{"id":"Deepseek-Chat"},{"id":"gpt-5.5"},{"id":""},
        {"id":"anthropic/claude-opus-5-5"}]})";
    auto list = AiQueryAssistant::parseModelList(c, 200, body);
    ASSERT_TRUE(list.ok);
    EXPECT_EQ(list.models, (QStringList{"anthropic/claude-opus-5-5", "Deepseek-Chat", "gpt-5.5"}));

    QJsonArray many;
    for (int i = 0; i < 1500; ++i)
        many.append(QJsonObject{{"id", QStringLiteral("model-%1").arg(i)}});
    list = AiQueryAssistant::parseModelList(c, 200, QJsonDocument(QJsonObject{{"data", many}}).toJson());
    EXPECT_EQ(list.models.size(), AiQueryAssistant::kMaxListedModels);

    // docs: T5. No /models endpoint: reachable, list unknown.
    EXPECT_EQ(AiQueryAssistant::checkConnection(c, 404, "Not Found").status,
              AiQueryAssistant::ConnectionStatus::NoModelList);
    EXPECT_EQ(AiQueryAssistant::checkConnection(c, 401, R"({"error":{"message":"bad key"}})").status,
              AiQueryAssistant::ConnectionStatus::KeyRejected);
}

// B6 review: the write / JavaScript checks look at the keys the server will
// see, so a JSON escape ($merge) cannot hide a stage.
TEST(AiQueryAssistantTest, write_and_javascript_checks_see_decoded_keys)
{
    auto parse = [](const QString &script) {
        QJsonObject answer{{"kind", "aggregate"}, {"script", script}, {"explanation", "x"}};
        QJsonObject message{{"role", "assistant"}, {"content", QString::fromUtf8(QJsonDocument(answer).toJson())}};
        QJsonObject choice{{"message", message}, {"finish_reason", "stop"}};
        return AiQueryAssistant::parseResponse(Provider::OpenAiCompatible, 200,
                                               QJsonDocument(QJsonObject{{"choices", QJsonArray{choice}}}).toJson(), "shop");
    };
    auto s = parse(QStringLiteral(R"S(db.getCollection('orders').aggregate([{"$merge": {into: "x"}}]))S"));
    EXPECT_TRUE(!s.ok || s.writesData) << "escaped $merge slipped through";
    s = parse(QStringLiteral(R"S(db.getCollection('orders').aggregate([{$match: {}}, {"$out": "x"}]))S"));
    EXPECT_TRUE(!s.ok || s.writesData) << "escaped $out slipped through";
    s = parse(QStringLiteral(R"S(db.getCollection('orders').aggregate([{$facet: {a: [{$merge: {into: "x"}}]}}]))S"));
    EXPECT_TRUE(!s.ok || s.writesData);

    // Server-side JavaScript is refused outright.
    s = parse(QStringLiteral(R"S(db.getCollection('orders').find({$where: "sleep(1000) || true"}))S"));
    EXPECT_FALSE(s.ok);
    s = parse(QStringLiteral(R"S(db.getCollection('orders').aggregate([{$addFields: {x: {"$function": {body: "1", args: [], lang: "js"}}}}]))S"));
    EXPECT_FALSE(s.ok);
    s = parse(QStringLiteral(R"S(db.getCollection('orders').aggregate([{$group: {_id: 1, x: {$accumulator: {}}}}]))S"));
    EXPECT_FALSE(s.ok);

    // Plain reads stay fine.
    s = parse(QStringLiteral(R"S(db.getCollection('orders').aggregate([{$match: {status: "shipped"}}, {$count: "n"}]))S"));
    EXPECT_TRUE(s.ok) << s.error.toStdString();
    EXPECT_FALSE(s.writesData);
}

TEST(AiQueryAssistantTest, log_redaction_and_lookalike_hosts)
{
    using AiQueryAssistant::redactSecrets;
    EXPECT_EQ(redactSecrets("Incorrect API key provided: sk-proj-abcdef123456."), "Incorrect API key provided: [redacted].");
    EXPECT_FALSE(redactSecrets("key AIzaSyD-0123456789abcdefghijk is invalid").contains("AIzaSy"));
    EXPECT_FALSE(redactSecrets("Authorization: Bearer abc.def").contains("abc.def"));
    EXPECT_FALSE(redactSecrets("token 0123456789abcdef0123456789abcdef!").contains("0123456789abcdef0123"));
    // Ordinary words and model names stay readable.
    EXPECT_EQ(redactSecrets("claude-sonnet-5-5 skipped: rate limit (HTTP 429), skeleton risk"),
              "claude-sonnet-5-5 skipped: rate limit (HTTP 429), skeleton risk");

    // A Cyrillic look-alike of api.openai.com is shown as punycode.
    const QString lookalike = QStringLiteral("https://") + QChar(0x0430) + QStringLiteral("pi.openai.com/v1");
    EXPECT_TRUE(AiQueryAssistant::urlHost(lookalike).startsWith("xn--")) << AiQueryAssistant::urlHost(lookalike).toStdString();
    EXPECT_EQ(AiQueryAssistant::urlHost("https://api.openai.com/v1"), "api.openai.com");
}

// docs: B7. Ollama: the OpenAI-compatible path at localhost, never a key.
TEST(AiQueryAssistantTest, ollama_defaults_and_keyless_requests)
{
    const Provider o = Provider::Ollama;
    EXPECT_EQ(AiQueryAssistant::providerFromId("ollama"), o);
    EXPECT_EQ(AiQueryAssistant::providerId(o), "ollama");
    EXPECT_EQ(AiQueryAssistant::providerName(o), "Ollama");
    EXPECT_TRUE(AiQueryAssistant::hasConfigurableBaseUrl(o));
    EXPECT_TRUE(AiQueryAssistant::usesOpenAiApi(o));
    EXPECT_FALSE(AiQueryAssistant::usesApiKey(o));
    EXPECT_TRUE(AiQueryAssistant::usesApiKey(Provider::OpenAiCompatible));
    EXPECT_FALSE(AiQueryAssistant::usesOpenAiApi(Provider::Gemini));

    // Default address passes the check as it is, without a key.
    const QString base = AiQueryAssistant::defaultBaseUrl(o);
    EXPECT_EQ(base, "http://localhost:11434/v1");
    EXPECT_TRUE(AiQueryAssistant::defaultBaseUrl(Provider::OpenAiCompatible).isEmpty());
    EXPECT_EQ(AiQueryAssistant::checkBaseUrl(base, false, false).normalized, base);
    EXPECT_EQ(AiQueryAssistant::endpointUrl(o, "llama3.2", base), "http://localhost:11434/v1/chat/completions");
    EXPECT_EQ(AiQueryAssistant::modelsUrl(o, base), "http://localhost:11434/v1/models");
    EXPECT_TRUE(AiQueryAssistant::modelIds(o).isEmpty());
    EXPECT_TRUE(AiQueryAssistant::defaultModel(o).isEmpty());

    // No Authorization header even when a key is passed by mistake, no
    // environment variable and no key page.
    auto headers = AiQueryAssistant::requestHeaders(o, "sk-should-not-go", "llama3.2");
    ASSERT_EQ(headers.size(), 1u);
    EXPECT_EQ(headers[0].first, QByteArray("content-type"));
    EXPECT_TRUE(AiQueryAssistant::apiKeyEnvironmentVariables(o, base).isEmpty());
    EXPECT_TRUE(AiQueryAssistant::apiKeyEnvironmentVariables(o, "https://api.openai.com/v1").isEmpty());
    EXPECT_TRUE(AiQueryAssistant::apiKeyPageUrl(o, base).isEmpty());

    // Same body and parsing as the OpenAI-compatible provider.
    const QJsonObject body = QJsonDocument::fromJson(
        AiQueryAssistant::requestBody(o, "llama3.2", sampleContext(false), "orders shipped today")).object();
    EXPECT_EQ(body.value("model").toString(), "llama3.2");
    EXPECT_EQ(body.value("messages").toArray().size(), 2);
    const QJsonObject message{{"role", "assistant"},
                              {"content", R"J({"kind":"find","script":"db.orders.find({})","explanation":"x"})J"}};
    const QByteArray answer = QJsonDocument(QJsonObject{
        {"choices", QJsonArray{QJsonObject{{"index", 0}, {"message", message}, {"finish_reason", "stop"}}}}}).toJson();
    const auto s = AiQueryAssistant::parseResponse(o, 200, answer, "shop");
    EXPECT_TRUE(s.ok) << s.error.toStdString();

    // Ollama's "model not found, try pulling it first" (404).
    const auto missing = AiQueryAssistant::parseResponse(
        o, 404, R"({"error":{"message":"model \"llama9\" not found, try pulling it first","type":"api_error"}})", "shop");
    EXPECT_EQ(missing.failure, AiQueryAssistant::Failure::ModelUnavailable);
    EXPECT_TRUE(missing.error.contains("try pulling it first"));
}

TEST(AiQueryAssistantTest, ollama_model_list_and_empty_hint)
{
    const Provider o = Provider::Ollama;
    using AiQueryAssistant::ConnectionStatus;
    const QByteArray listed = R"({"object":"list","data":[
        {"id":"qwen3:8b","object":"model","owned_by":"library"},
        {"id":"nomic-embed-text:latest","object":"model","owned_by":"library"},
        {"id":"llama3.2:latest","object":"model","owned_by":"library"}]})";
    const auto c = AiQueryAssistant::checkConnection(o, 200, listed);
    EXPECT_EQ(c.status, ConnectionStatus::Connected);
    EXPECT_EQ(c.list.models, (QStringList{"llama3.2:latest", "qwen3:8b"}));

    // Running, nothing pulled yet: "data": null (older Ollama) or [].
    EXPECT_EQ(AiQueryAssistant::checkConnection(o, 200, R"({"object":"list","data":null})").status,
              ConnectionStatus::NoModelsInstalled);
    EXPECT_EQ(AiQueryAssistant::checkConnection(o, 200, R"({"object":"list","data":[]})").status,
              ConnectionStatus::NoModelsInstalled);
    EXPECT_EQ(AiQueryAssistant::checkConnection(Provider::OpenAiCompatible, 200, R"({"data":[]})").status,
              ConnectionStatus::NoModelsInstalled);
    // Only embedding models: installed, but none usable.
    EXPECT_EQ(AiQueryAssistant::checkConnection(o, 200, R"({"data":[{"id":"nomic-embed-text"}]})").status,
              ConnectionStatus::NoUsableModels);
    // Anthropic / Gemini never report "nothing installed".
    EXPECT_EQ(AiQueryAssistant::checkConnection(Provider::Anthropic, 200, R"({"data":[]})").status,
              ConnectionStatus::NoUsableModels);
    // Not running: no HTTP response.
    EXPECT_EQ(AiQueryAssistant::checkConnection(o, 0, {}).status, ConnectionStatus::Unreachable);
}

// docs: T2. On the local network plain http is allowed only without a key;
// Ollama never has one, an OpenAI-compatible server may.
TEST(AiQueryAssistantTest, local_addresses_and_scope)
{
    using AiQueryAssistant::BaseUrlError;
    using AiQueryAssistant::HostScope;
    EXPECT_EQ(AiQueryAssistant::checkBaseUrl("http://192.168.1.20:11434/v1", false, true).error, BaseUrlError::None);
    EXPECT_EQ(AiQueryAssistant::checkBaseUrl("http://192.168.1.20:11434/v1", true, true).error,
              BaseUrlError::PlainHttpWithKey);
    EXPECT_EQ(AiQueryAssistant::checkBaseUrl("http://gpu-box.local:11434/v1", true, true).error,
              BaseUrlError::PlainHttpWithKey);
    // Android and the browser: only loopback over plain http.
    EXPECT_EQ(AiQueryAssistant::checkBaseUrl("http://192.168.1.20:11434/v1", false, false).error,
              BaseUrlError::PlainHttpBlocked);
    EXPECT_EQ(AiQueryAssistant::checkBaseUrl("http://127.0.0.1:11434/v1", true, false).error, BaseUrlError::None);
    EXPECT_EQ(AiQueryAssistant::checkBaseUrl("http://ollama.example.com:11434/v1", false, true).error,
              BaseUrlError::PlainHttpPublic);

    EXPECT_EQ(AiQueryAssistant::hostScope("http://localhost:11434/v1"), HostScope::ThisDevice);
    EXPECT_EQ(AiQueryAssistant::hostScope("http://[::1]:11434/v1"), HostScope::ThisDevice);
    EXPECT_EQ(AiQueryAssistant::hostScope("http://10.0.0.5:11434/v1"), HostScope::LocalNetwork);
    EXPECT_EQ(AiQueryAssistant::hostScope("https://gpu-box.local/v1"), HostScope::LocalNetwork);
    EXPECT_EQ(AiQueryAssistant::hostScope("https://gpu-box/v1"), HostScope::Internet);
    EXPECT_EQ(AiQueryAssistant::hostScope("https://api.openai.com/v1"), HostScope::Internet);
    EXPECT_EQ(AiQueryAssistant::hostScope("https://localhost.evil.example/v1"), HostScope::Internet);
    // Carrier-grade NAT space is shared with other customers: not local,
    // so no plain http there either.
    EXPECT_EQ(AiQueryAssistant::hostScope("http://100.64.1.2:11434/v1"), HostScope::Internet);
    EXPECT_EQ(AiQueryAssistant::checkBaseUrl("http://100.100.5.5:11434/v1", false, true).error,
              BaseUrlError::PlainHttpPublic);
    EXPECT_EQ(AiQueryAssistant::hostScope(""), HostScope::Internet);
}

TEST(AiQueryAssistantTest, answer_time_limits)
{
    EXPECT_EQ(AiQueryAssistant::defaultTimeoutSec(Provider::Ollama), 180);
    EXPECT_EQ(AiQueryAssistant::defaultTimeoutSec(Provider::OpenAiCompatible), 120);
    EXPECT_EQ(AiQueryAssistant::defaultTimeoutSec(Provider::Anthropic), 120);
    EXPECT_EQ(AiQueryAssistant::clampTimeoutSec(5), AiQueryAssistant::kMinTimeoutSec);
    EXPECT_EQ(AiQueryAssistant::clampTimeoutSec(90), 90);
    EXPECT_EQ(AiQueryAssistant::clampTimeoutSec(100000), AiQueryAssistant::kMaxTimeoutSec);
    EXPECT_EQ(AiQueryAssistant::kMinTimeoutSec, 30);
    EXPECT_EQ(AiQueryAssistant::kMaxTimeoutSec, 300);
}
