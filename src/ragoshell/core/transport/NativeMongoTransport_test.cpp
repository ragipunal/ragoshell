#include <gtest/gtest.h>

#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

#include "ragoshell/core/transport/NativeMongoTransport.h"

using namespace RagoShell;

TEST(NativeMongoTransportTest, reports_live)
{
    NativeMongoTransport t;
    EXPECT_TRUE(t.isLive());
}

TEST(NativeMongoTransportTest, run_command_before_connect_errors)
{
    NativeMongoTransport t;
    const auto r = t.runCommand(QStringLiteral("admin"), QStringLiteral(R"({"ping":1})"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, QStringLiteral("not connected"));
}

TEST(NativeMongoTransportTest, connect_rejects_bad_uri_without_network)
{
    NativeMongoTransport t;
    // URI parse fails synchronously — no server needed.
    const auto r = t.connect(QStringLiteral(R"({"uri":"not-a-uri"})"));
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

TEST(NativeMongoTransportTest, connect_rejects_missing_uri)
{
    NativeMongoTransport t;
    const auto r = t.connect(QStringLiteral(R"({"timeoutMS":5000})"));
    EXPECT_FALSE(r.ok);
}

TEST(NativeMongoTransportTest, canonical_ejson_round_trip_preserves_types)
{
    // Relaxed EJSON loses $oid/$date/$numberLong typing; the wire must be
    // canonical so bson->ejson->bson is identity (docs/TransportSeam.md §5).
    const auto in = bsoncxx::from_json(
        R"({"_id":{"$oid":"64f0a7e28f0a2d6a5c3e0001"},)"
        R"("n":{"$numberLong":"9223372036854775807"},)"
        R"("t":{"$date":{"$numberLong":"1700000000000"}}})");
    const auto ejson = bsoncxx::to_json(in.view(), bsoncxx::ExtendedJsonMode::k_canonical);
    const auto back = bsoncxx::from_json(ejson);
    EXPECT_EQ(in.view()["_id"].get_oid().value.to_string(),
              back.view()["_id"].get_oid().value.to_string());
    EXPECT_EQ(in.view()["n"].get_int64(), back.view()["n"].get_int64());
    EXPECT_EQ(in.view()["t"].get_date(), back.view()["t"].get_date());
}

namespace
{
    // maxTimeMS the bridge would send for `ejson`; -1 when missing, not
    // int32, not the last key or present more than once.
    std::int64_t sentMaxTime(const char *ejson, std::string *firstKey = nullptr)
    {
        const QString out = NativeMongoTransport::withMaxTimeCap(QString::fromUtf8(ejson));
        if (out.isEmpty())
            return -1;
        const auto doc = bsoncxx::from_json(out.toStdString());
        int count = 0;
        std::string lastKey;
        std::int64_t value = -1;
        for (const auto &el : doc.view()) {
            if (firstKey && firstKey->empty())
                *firstKey = std::string(el.key());
            lastKey = std::string(el.key());
            if (el.key() == "maxTimeMS") {
                ++count;
                if (el.type() == bsoncxx::type::k_int32)
                    value = el.get_int32().value;
            }
        }
        return count == 1 && lastKey == "maxTimeMS" ? value : -1;
    }
}

TEST(NativeMongoTransportTest, max_time_ms_cap_cannot_be_bypassed)
{
    // 2026-11-04 review R4: maxTimeMS 0 means "no limit" to MongoDB and
    // used to pass the old clamp; NaN hit an undefined cast. Every
    // command leaves with exactly one int32 maxTimeMS in [1, cap].
    const std::int64_t cap = NativeMongoTransport::kMaxOpTimeMs;
    EXPECT_EQ(sentMaxTime(R"({"find":"c"})"), cap);
    EXPECT_EQ(sentMaxTime(R"({"find":"c","maxTimeMS":0})"), cap);
    EXPECT_EQ(sentMaxTime(R"({"find":"c","maxTimeMS":-5})"), cap);
    EXPECT_EQ(sentMaxTime(R"({"find":"c","maxTimeMS":{"$numberDouble":"NaN"}})"), cap);
    EXPECT_EQ(sentMaxTime(R"({"find":"c","maxTimeMS":{"$numberDouble":"Infinity"}})"), cap);
    EXPECT_EQ(sentMaxTime(R"({"find":"c","maxTimeMS":1.5})"), cap);
    EXPECT_EQ(sentMaxTime(R"({"find":"c","maxTimeMS":"10"})"), cap);
    EXPECT_EQ(sentMaxTime(R"({"find":"c","maxTimeMS":{"$numberLong":"99999999999"}})"), cap);
    EXPECT_EQ(sentMaxTime(R"({"find":"c","maxTimeMS":1000})"), 1000);
    EXPECT_EQ(sentMaxTime(R"({"find":"c","maxTimeMS":{"$numberLong":"2000"}})"), 2000);
    // Duplicates collapse to one key; a "0" twin cannot win.
    EXPECT_EQ(sentMaxTime(R"({"find":"c","maxTimeMS":0,"maxTimeMS":200})"), 200);
    // The command name stays the first key even when maxTimeMS came first.
    std::string first;
    EXPECT_EQ(sentMaxTime(R"({"maxTimeMS":0,"find":"c"})", &first), cap);
    EXPECT_EQ(first, "find");
    EXPECT_TRUE(NativeMongoTransport::withMaxTimeCap(QStringLiteral("not-json")).isEmpty());
}
