#include "ragoshell/core/transport/InMemoryDemoTransport.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/decimal128.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/oid.hpp>
#include <bsoncxx/stdx/string_view.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/types/bson_value/value.hpp>
#include <bsoncxx/types/bson_value/view.hpp>

namespace RagoShell
{
    namespace
    {
        namespace basic = bsoncxx::builder::basic;
        using basic::kvp;
        using basic::make_array;
        using basic::make_document;
        using BsonVal = bsoncxx::types::bson_value::value;
        using BsonView = bsoncxx::types::bson_value::view;
        using DocVal = bsoncxx::document::value;
        using DocView = bsoncxx::document::view;

        constexpr const char *kDemoServerVersion = "8.0.0-demo";

        IMongoTransport::Result okDoc(DocView doc)
        {
            IMongoTransport::Result r;
            r.ok = true;
            r.ejson = QString::fromStdString(
                bsoncxx::to_json(doc, bsoncxx::ExtendedJsonMode::k_canonical));
            return r;
        }

        IMongoTransport::Result okArray(bsoncxx::array::view arr)
        {
            IMongoTransport::Result r;
            r.ok = true;
            r.ejson = QString::fromStdString(
                bsoncxx::to_json(arr, bsoncxx::ExtendedJsonMode::k_canonical));
            return r;
        }

        IMongoTransport::Result errResult(const QString &msg, int code = 59,
                                          const QString &codeName = QStringLiteral("CommandNotFound"))
        {
            IMongoTransport::Result r;
            r.ok = false;
            r.error = msg;
            r.code = code;
            r.codeName = codeName;
            return r;
        }

        IMongoTransport::Result wrapCursor(const QString &ns, const std::vector<DocVal> &docs)
        {
            basic::array batch;
            for (const auto &d : docs)
                batch.append(d.view());
            return okDoc(make_document(
                kvp("cursor", make_document(
                    kvp("firstBatch", batch.extract()),
                    kvp("id", static_cast<std::int64_t>(0)),
                    kvp("ns", ns.toStdString()))),
                kvp("ok", 1.0)).view());
        }

        QString strFromView(bsoncxx::stdx::string_view sv)
        {
            return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
        }

        bool isNumericType(bsoncxx::type t)
        {
            return t == bsoncxx::type::k_int32 || t == bsoncxx::type::k_int64
                   || t == bsoncxx::type::k_double || t == bsoncxx::type::k_decimal128;
        }

        int bsonTypeOrder(bsoncxx::type t)
        {
            switch (t) {
            case bsoncxx::type::k_minkey:     return 1;
            case bsoncxx::type::k_undefined:
            case bsoncxx::type::k_null:       return 2;
            case bsoncxx::type::k_int32:
            case bsoncxx::type::k_int64:
            case bsoncxx::type::k_double:
            case bsoncxx::type::k_decimal128: return 3;
            case bsoncxx::type::k_string:
            case bsoncxx::type::k_symbol:     return 4;
            case bsoncxx::type::k_document:   return 5;
            case bsoncxx::type::k_array:      return 6;
            case bsoncxx::type::k_binary:     return 7;
            case bsoncxx::type::k_oid:        return 8;
            case bsoncxx::type::k_bool:       return 9;
            case bsoncxx::type::k_date:       return 10;
            case bsoncxx::type::k_timestamp:  return 11;
            case bsoncxx::type::k_regex:      return 12;
            case bsoncxx::type::k_maxkey:     return 14;
            default:                          return 13;
            }
        }

        long double asLongDouble(BsonView v, bool *isNaN = nullptr)
        {
            if (isNaN)
                *isNaN = false;
            switch (v.type()) {
            case bsoncxx::type::k_int32:
                return static_cast<long double>(v.get_int32().value);
            case bsoncxx::type::k_int64:
                return static_cast<long double>(v.get_int64().value);
            case bsoncxx::type::k_double: {
                const double d = v.get_double().value;
                if (std::isnan(d)) {
                    if (isNaN)
                        *isNaN = true;
                    return 0.0L;
                }
                return static_cast<long double>(d);
            }
            case bsoncxx::type::k_decimal128: {
                const std::string s = v.get_decimal128().value.to_string();
                if (s == "NaN" || s == "-NaN") {
                    if (isNaN)
                        *isNaN = true;
                    return 0.0L;
                }
                try {
                    return std::stold(s);
                } catch (const std::exception &) {
                    return 0.0L;
                }
            }
            default:
                return 0.0L;
            }
        }

        int compareNumbers(BsonView a, BsonView b)
        {
            // Exact 64-bit integer comparison when both sides are integer BSON types.
            if ((a.type() == bsoncxx::type::k_int32 || a.type() == bsoncxx::type::k_int64)
                && (b.type() == bsoncxx::type::k_int32 || b.type() == bsoncxx::type::k_int64)) {
                const std::int64_t ia = a.type() == bsoncxx::type::k_int32
                    ? static_cast<std::int64_t>(a.get_int32().value) : a.get_int64().value;
                const std::int64_t ib = b.type() == bsoncxx::type::k_int32
                    ? static_cast<std::int64_t>(b.get_int32().value) : b.get_int64().value;
                return ia < ib ? -1 : (ia > ib ? 1 : 0);
            }

            bool nanA = false, nanB = false;
            const long double da = asLongDouble(a, &nanA);
            const long double db = asLongDouble(b, &nanB);
            if (nanA || nanB)
                return nanA && nanB ? 0 : (nanA ? -1 : 1);
            if (da < db)
                return -1;
            if (da > db)
                return 1;
            // Tie-break exact Decimal128 strings when `long double` rounds equal.
            if (a.type() == bsoncxx::type::k_decimal128 && b.type() == bsoncxx::type::k_decimal128) {
                const std::string sa = a.get_decimal128().value.to_string();
                const std::string sb = b.get_decimal128().value.to_string();
                if (sa == sb)
                    return 0;
            }
            return 0;
        }

        int compareBsonValues(BsonView a, BsonView b, int depth = 0);

        int compareDocuments(DocView a, DocView b, int depth = 0)
        {
            if (depth > InMemoryDemoTransport::kMaxDepth)
                return 0;
            auto ia = a.begin();
            auto ib = b.begin();
            for (; ia != a.end() && ib != b.end(); ++ia, ++ib) {
                if (ia->key() != ib->key())
                    return ia->key() < ib->key() ? -1 : 1;
                const int vc = compareBsonValues(ia->get_value(), ib->get_value(), depth + 1);
                if (vc != 0)
                    return vc;
            }
            if (ia == a.end() && ib == b.end())
                return 0;
            return ia == a.end() ? -1 : 1;
        }

        int compareArrays(bsoncxx::array::view a, bsoncxx::array::view b, int depth = 0)
        {
            if (depth > InMemoryDemoTransport::kMaxDepth)
                return 0;
            auto ia = a.begin();
            auto ib = b.begin();
            for (; ia != a.end() && ib != b.end(); ++ia, ++ib) {
                const int vc = compareBsonValues(ia->get_value(), ib->get_value(), depth + 1);
                if (vc != 0)
                    return vc;
            }
            if (ia == a.end() && ib == b.end())
                return 0;
            return ia == a.end() ? -1 : 1;
        }

        int compareBsonValues(BsonView a, BsonView b, int depth)
        {
            if (depth > InMemoryDemoTransport::kMaxDepth)
                return 0;
            const int oa = bsonTypeOrder(a.type());
            const int ob = bsonTypeOrder(b.type());
            if (oa != ob)
                return oa < ob ? -1 : 1;

            switch (a.type()) {
            case bsoncxx::type::k_minkey:
            case bsoncxx::type::k_undefined:
            case bsoncxx::type::k_null:
            case bsoncxx::type::k_maxkey:
                return 0;
            case bsoncxx::type::k_int32:
            case bsoncxx::type::k_int64:
            case bsoncxx::type::k_double:
            case bsoncxx::type::k_decimal128:
                return compareNumbers(a, b);
            case bsoncxx::type::k_string:
            case bsoncxx::type::k_symbol: {
                const bsoncxx::stdx::string_view sa = a.type() == bsoncxx::type::k_string
                    ? a.get_string().value : a.get_symbol().symbol;
                const bsoncxx::stdx::string_view sb = b.type() == bsoncxx::type::k_string
                    ? b.get_string().value : b.get_symbol().symbol;
                return sa < sb ? -1 : (sa > sb ? 1 : 0);
            }
            case bsoncxx::type::k_document:
                return compareDocuments(a.get_document().value, b.get_document().value, depth + 1);
            case bsoncxx::type::k_array:
                return compareArrays(a.get_array().value, b.get_array().value, depth + 1);
            case bsoncxx::type::k_binary: {
                const auto ba = a.get_binary();
                const auto bb = b.get_binary();
                if (ba.size != bb.size)
                    return ba.size < bb.size ? -1 : 1;
                if (ba.sub_type != bb.sub_type)
                    return static_cast<int>(ba.sub_type) < static_cast<int>(bb.sub_type) ? -1 : 1;
                const int cmp = ba.size == 0 ? 0 : std::memcmp(ba.bytes, bb.bytes, ba.size);
                return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
            }
            case bsoncxx::type::k_oid:
                return a.get_oid().value < b.get_oid().value ? -1
                     : (a.get_oid().value > b.get_oid().value ? 1 : 0);
            case bsoncxx::type::k_bool:
                return a.get_bool().value == b.get_bool().value ? 0
                     : (a.get_bool().value ? 1 : -1);
            case bsoncxx::type::k_date: {
                const auto ma = a.get_date().to_int64();
                const auto mb = b.get_date().to_int64();
                return ma < mb ? -1 : (ma > mb ? 1 : 0);
            }
            case bsoncxx::type::k_timestamp: {
                const auto ta = a.get_timestamp();
                const auto tb = b.get_timestamp();
                if (ta.timestamp != tb.timestamp)
                    return ta.timestamp < tb.timestamp ? -1 : 1;
                return ta.increment < tb.increment ? -1 : (ta.increment > tb.increment ? 1 : 0);
            }
            case bsoncxx::type::k_regex: {
                const auto ra = a.get_regex();
                const auto rb = b.get_regex();
                if (ra.regex != rb.regex)
                    return ra.regex < rb.regex ? -1 : 1;
                return ra.options < rb.options ? -1 : (ra.options > rb.options ? 1 : 0);
            }
            default: {
                const std::string ja = bsoncxx::to_json(make_document(kvp("v", a)).view(),
                                                        bsoncxx::ExtendedJsonMode::k_canonical);
                const std::string jb = bsoncxx::to_json(make_document(kvp("v", b)).view(),
                                                        bsoncxx::ExtendedJsonMode::k_canonical);
                return ja < jb ? -1 : (ja > jb ? 1 : 0);
            }
            }
        }

        struct BsonValueLess
        {
            bool operator()(const BsonVal &a, const BsonVal &b) const
            {
                return compareBsonValues(a.view(), b.view()) < 0;
            }
        };

        std::int64_t doubleToInt64(double d, std::int64_t fallback = 0)
        {
            if (!std::isfinite(d))
                return fallback;
            constexpr double kMinI64 = -9223372036854775808.0;
            constexpr double kMaxI64 = 9223372036854774784.0;
            if (d <= kMinI64)
                return INT64_MIN;
            if (d >= kMaxI64)
                return INT64_MAX;
            return static_cast<std::int64_t>(std::trunc(d));
        }

        std::int64_t toInt64Option(const bsoncxx::document::element &el, std::int64_t fallback = 0)
        {
            if (!el)
                return fallback;
            switch (el.type()) {
            case bsoncxx::type::k_int32:  return el.get_int32().value;
            case bsoncxx::type::k_int64:  return el.get_int64().value;
            case bsoncxx::type::k_double: return doubleToInt64(el.get_double().value, fallback);
            default:                      return fallback;
            }
        }

        std::int64_t toInt64Value(BsonView v, std::int64_t fallback = 0)
        {
            switch (v.type()) {
            case bsoncxx::type::k_int32:  return v.get_int32().value;
            case bsoncxx::type::k_int64:  return v.get_int64().value;
            case bsoncxx::type::k_double: return doubleToInt64(v.get_double().value, fallback);
            default:                      return fallback;
            }
        }

        bool toBoolOption(const bsoncxx::document::element &el, bool fallback = false)
        {
            if (!el)
                return fallback;
            switch (el.type()) {
            case bsoncxx::type::k_bool:   return el.get_bool().value;
            case bsoncxx::type::k_int32:  return el.get_int32().value != 0;
            case bsoncxx::type::k_int64:  return el.get_int64().value != 0;
            case bsoncxx::type::k_double: return el.get_double().value != 0.0;
            default:                      return fallback;
            }
        }

        bool matchesTypeFilter(BsonView v, BsonView typeSpec)
        {
            if (typeSpec.type() == bsoncxx::type::k_int32 || typeSpec.type() == bsoncxx::type::k_int64
                || typeSpec.type() == bsoncxx::type::k_double) {
                const int code = static_cast<int>(toInt64Value(typeSpec, -999));
                switch (code) {
                case 1:  return v.type() == bsoncxx::type::k_double;
                case 2:  return v.type() == bsoncxx::type::k_string;
                case 3:  return v.type() == bsoncxx::type::k_document;
                case 4:  return v.type() == bsoncxx::type::k_array;
                case 5:  return v.type() == bsoncxx::type::k_binary;
                case 7:  return v.type() == bsoncxx::type::k_oid;
                case 8:  return v.type() == bsoncxx::type::k_bool;
                case 9:  return v.type() == bsoncxx::type::k_date;
                case 10: return v.type() == bsoncxx::type::k_null;
                case 11: return v.type() == bsoncxx::type::k_regex;
                case 16: return v.type() == bsoncxx::type::k_int32;
                case 17: return v.type() == bsoncxx::type::k_timestamp;
                case 18: return v.type() == bsoncxx::type::k_int64;
                case 19: return v.type() == bsoncxx::type::k_decimal128;
                default: return false;
                }
            }
            if (typeSpec.type() == bsoncxx::type::k_string) {
                const bsoncxx::stdx::string_view name = typeSpec.get_string().value;
                if (name == "number")     return isNumericType(v.type());
                if (name == "double")     return v.type() == bsoncxx::type::k_double;
                if (name == "string")     return v.type() == bsoncxx::type::k_string;
                if (name == "object")     return v.type() == bsoncxx::type::k_document;
                if (name == "array")      return v.type() == bsoncxx::type::k_array;
                if (name == "binData")    return v.type() == bsoncxx::type::k_binary;
                if (name == "objectId")   return v.type() == bsoncxx::type::k_oid;
                if (name == "bool")       return v.type() == bsoncxx::type::k_bool;
                if (name == "date")       return v.type() == bsoncxx::type::k_date;
                if (name == "null")       return v.type() == bsoncxx::type::k_null;
                if (name == "regex")      return v.type() == bsoncxx::type::k_regex;
                if (name == "int")        return v.type() == bsoncxx::type::k_int32;
                if (name == "timestamp")  return v.type() == bsoncxx::type::k_timestamp;
                if (name == "long")       return v.type() == bsoncxx::type::k_int64;
                if (name == "decimal")    return v.type() == bsoncxx::type::k_decimal128;
            }
            return false;
        }

        constexpr qsizetype kMaxRegexPatternChars = 512;
        constexpr std::size_t kMaxRegexSubjectBytes = 65536;

        // Rejects nested-quantifier patterns (e.g. `(a+)+`, `(.*)*`, `(a|aa)+`) that can trigger
        // exponential PCRE2 backtracking inside the WASM single-threaded worker.
        bool hasCatastrophicNestedQuantifier(const QString &pattern)
        {
            std::vector<bool> groupHasQuantifier;
            bool escaped = false;
            bool inCharClass = false;
            bool prevClosedQuantifiedGroup = false;

            auto isQuantifierChar = [](QChar ch) {
                return ch == QLatin1Char('+') || ch == QLatin1Char('*') || ch == QLatin1Char('{');
            };

            for (qsizetype i = 0; i < pattern.size(); ++i) {
                const QChar ch = pattern[i];
                if (escaped) {
                    escaped = false;
                    prevClosedQuantifiedGroup = false;
                    continue;
                }
                if (ch == QLatin1Char('\\')) {
                    escaped = true;
                    prevClosedQuantifiedGroup = false;
                    continue;
                }
                if (inCharClass) {
                    if (ch == QLatin1Char(']'))
                        inCharClass = false;
                    prevClosedQuantifiedGroup = false;
                    continue;
                }
                if (ch == QLatin1Char('[')) {
                    inCharClass = true;
                    prevClosedQuantifiedGroup = false;
                    continue;
                }
                if (ch == QLatin1Char('(')) {
                    groupHasQuantifier.push_back(false);
                    prevClosedQuantifiedGroup = false;
                    continue;
                }
                if (ch == QLatin1Char('|')) {
                    // Alternation inside a quantified group (`(a|aa)+`) can also backtrack exponentially.
                    if (!groupHasQuantifier.empty())
                        groupHasQuantifier.back() = true;
                    prevClosedQuantifiedGroup = false;
                    continue;
                }
                if (ch == QLatin1Char(')')) {
                    bool hadInner = false;
                    if (!groupHasQuantifier.empty()) {
                        hadInner = groupHasQuantifier.back();
                        groupHasQuantifier.pop_back();
                        if (hadInner && !groupHasQuantifier.empty())
                            groupHasQuantifier.back() = true;
                    }
                    prevClosedQuantifiedGroup = hadInner;
                    continue;
                }
                if (isQuantifierChar(ch)) {
                    if (prevClosedQuantifiedGroup)
                        return true;
                    if (!groupHasQuantifier.empty())
                        groupHasQuantifier.back() = true;
                    prevClosedQuantifiedGroup = false;
                    continue;
                }
                prevClosedQuantifiedGroup = false;
            }
            return false;
        }

        std::optional<QRegularExpression> compileSafeRegex(const QString &pattern, const QString &options)
        {
            if (pattern.size() > kMaxRegexPatternChars || hasCatastrophicNestedQuantifier(pattern))
                return std::nullopt;
            QRegularExpression::PatternOptions opts = QRegularExpression::NoPatternOption;
            if (options.contains(QLatin1Char('i')))
                opts |= QRegularExpression::CaseInsensitiveOption;
            if (options.contains(QLatin1Char('m')))
                opts |= QRegularExpression::MultilineOption;
            if (options.contains(QLatin1Char('s')))
                opts |= QRegularExpression::DotMatchesEverythingOption;
            if (options.contains(QLatin1Char('x')))
                opts |= QRegularExpression::ExtendedPatternSyntaxOption;
            QRegularExpression re(pattern, opts);
            if (!re.isValid())
                return std::nullopt;
            return re;
        }

        bool matchesCompiledRegex(BsonView candidate, const QRegularExpression &re)
        {
            if (candidate.type() != bsoncxx::type::k_string)
                return false;
            const auto sv = candidate.get_string().value;
            if (sv.size() > kMaxRegexSubjectBytes)
                return false;
            return re.match(strFromView(sv)).hasMatch();
        }

        bool matchesRegex(BsonView candidate, const QString &pattern, const QString &options)
        {
            if (candidate.type() != bsoncxx::type::k_string)
                return false;
            const auto re = compileSafeRegex(pattern, options);
            return re ? matchesCompiledRegex(candidate, *re) : false;
        }

        // Collects all values reachable via `segments[idx..]` (walking nested documents
        // and array elements just like MongoDB's dotted path lookup).
        void collectPathValues(DocView doc, const QStringList &segments, int idx,
                               std::vector<BsonVal> &out, bool *foundAnyPath, bool *sawMissingLeaf,
                               int depth = 0)
        {
            if (idx >= segments.size() || depth > InMemoryDemoTransport::kMaxDepth
                || segments.size() > InMemoryDemoTransport::kMaxDepth) {
                *sawMissingLeaf = true;
                return;
            }
            const QByteArray segUtf8 = segments[idx].toUtf8();
            const bsoncxx::stdx::string_view seg(segUtf8.constData(), static_cast<std::size_t>(segUtf8.size()));
            const auto el = doc[seg];
            if (!el) {
                *sawMissingLeaf = true;
                return;
            }
            if (idx + 1 == segments.size()) {
                *foundAnyPath = true;
                out.emplace_back(el.get_value());
                return;
            }
            if (el.type() == bsoncxx::type::k_document) {
                collectPathValues(el.get_document().value, segments, idx + 1,
                                  out, foundAnyPath, sawMissingLeaf, depth + 1);
            } else if (el.type() == bsoncxx::type::k_array) {
                bool okIndex = false;
                const int arrIdx = segments[idx + 1].toInt(&okIndex);
                const auto arr = el.get_array().value;
                if (okIndex && arrIdx >= 0) {
                    int pos = 0;
                    for (const auto &item : arr) {
                        if (pos == arrIdx) {
                            if (idx + 2 == segments.size()) {
                                *foundAnyPath = true;
                                out.emplace_back(item.get_value());
                            } else if (item.type() == bsoncxx::type::k_document) {
                                collectPathValues(item.get_document().value, segments, idx + 2,
                                                  out, foundAnyPath, sawMissingLeaf, depth + 1);
                            } else {
                                *sawMissingLeaf = true;
                            }
                            return;
                        }
                        ++pos;
                    }
                    *sawMissingLeaf = true;
                    return;
                }
                bool anyElem = false;
                for (const auto &item : arr) {
                    anyElem = true;
                    if (item.type() == bsoncxx::type::k_document) {
                        collectPathValues(item.get_document().value, segments, idx + 1,
                                          out, foundAnyPath, sawMissingLeaf, depth + 1);
                    } else {
                        *sawMissingLeaf = true;
                    }
                }
                if (!anyElem)
                    *sawMissingLeaf = true;
            } else {
                *sawMissingLeaf = true;
            }
        }

        bool isOperatorObject(DocView d)
        {
            if (d.empty())
                return false;
            for (const auto &el : d) {
                if (el.key().empty() || el.key()[0] != '$')
                    return false;
            }
            return true;
        }

        bool matchesDocument(DocView doc, DocView filter, int depth = 0);
        bool matchesFieldCondition(const std::vector<BsonVal> &values, bool exists,
                                   bool sawMissingLeaf, BsonView cond, int depth = 0);

        bool matchesScalarOrElem(BsonView candidate, BsonView target)
        {
            if (target.type() == bsoncxx::type::k_regex) {
                const auto r = target.get_regex();
                if (matchesRegex(candidate, strFromView(r.regex), strFromView(r.options)))
                    return true;
            }
            if (compareBsonValues(candidate, target) == 0)
                return true;
            if (candidate.type() == bsoncxx::type::k_array && target.type() != bsoncxx::type::k_array) {
                for (const auto &item : candidate.get_array().value) {
                    if (target.type() == bsoncxx::type::k_regex) {
                        const auto r = target.get_regex();
                        if (matchesRegex(item.get_value(), strFromView(r.regex), strFromView(r.options)))
                            return true;
                    } else if (compareBsonValues(item.get_value(), target) == 0) {
                        return true;
                    }
                }
            }
            return false;
        }

        bool candidateSatisfiesComparison(BsonView candidate, bsoncxx::stdx::string_view op, BsonView operand)
        {
            auto cmpOne = [&](BsonView v) -> bool {
                if (op == "$gt" || op == "$gte" || op == "$lt" || op == "$lte") {
                    // In MongoDB, range operators only match values of the same BSON type bracket
                    // (except MinKey/MaxKey bounds).
                    if (operand.type() != bsoncxx::type::k_minkey
                        && operand.type() != bsoncxx::type::k_maxkey
                        && bsonTypeOrder(v.type()) != bsonTypeOrder(operand.type()))
                        return false;
                    const int c = compareBsonValues(v, operand);
                    if (op == "$gt")  return c > 0;
                    if (op == "$gte") return c >= 0;
                    if (op == "$lt")  return c < 0;
                    return c <= 0;
                }
                return false;
            };
            if (cmpOne(candidate))
                return true;
            if (candidate.type() == bsoncxx::type::k_array) {
                for (const auto &item : candidate.get_array().value) {
                    if (cmpOne(item.get_value()))
                        return true;
                }
            }
            return false;
        }

        bool matchesFieldCondition(const std::vector<BsonVal> &values, bool exists,
                                   bool sawMissingLeaf, BsonView cond, int depth)
        {
            if (depth > InMemoryDemoTransport::kMaxDepth)
                return false;
            if (cond.type() == bsoncxx::type::k_document && isOperatorObject(cond.get_document().value)) {
                const DocView ops = cond.get_document().value;
                QString regexPat;
                QString regexOpts;
                bool hasRegex = false;
                if (const auto rx = ops["$regex"]) {
                    hasRegex = true;
                    if (rx.type() == bsoncxx::type::k_string) {
                        regexPat = strFromView(rx.get_string().value);
                    } else if (rx.type() == bsoncxx::type::k_regex) {
                        regexPat = strFromView(rx.get_regex().regex);
                        regexOpts = strFromView(rx.get_regex().options);
                    }
                }
                if (const auto ro = ops["$options"]; ro && ro.type() == bsoncxx::type::k_string)
                    regexOpts += strFromView(ro.get_string().value);

                for (const auto &opEl : ops) {
                    const bsoncxx::stdx::string_view op = opEl.key();
                    const BsonView arg = opEl.get_value();
                    if (op == "$options")
                        continue;
                    if (op == "$regex") {
                        if (!hasRegex)
                            return false;
                        const auto compiledRe = compileSafeRegex(regexPat, regexOpts);
                        if (!compiledRe)
                            return false;
                        bool any = false;
                        for (const auto &v : values) {
                            if (matchesCompiledRegex(v.view(), *compiledRe))
                                any = true;
                            else if (v.view().type() == bsoncxx::type::k_array) {
                                for (const auto &it : v.view().get_array().value) {
                                    if (matchesCompiledRegex(it.get_value(), *compiledRe))
                                        any = true;
                                }
                            }
                        }
                        if (!any)
                            return false;
                    } else if (op == "$eq") {
                        if (arg.type() == bsoncxx::type::k_null) {
                            bool anyNull = !exists || sawMissingLeaf;
                            for (const auto &v : values) {
                                if (matchesScalarOrElem(v.view(), arg))
                                    anyNull = true;
                            }
                            if (!anyNull)
                                return false;
                        } else {
                            bool any = false;
                            for (const auto &v : values) {
                                if (matchesScalarOrElem(v.view(), arg))
                                    any = true;
                            }
                            if (!any)
                                return false;
                        }
                    } else if (op == "$ne") {
                        if (arg.type() == bsoncxx::type::k_null) {
                            if (!exists || sawMissingLeaf)
                                return false;
                        }
                        for (const auto &v : values) {
                            if (matchesScalarOrElem(v.view(), arg))
                                return false;
                        }
                    } else if (op == "$gt" || op == "$gte" || op == "$lt" || op == "$lte") {
                        bool any = false;
                        for (const auto &v : values) {
                            if (candidateSatisfiesComparison(v.view(), op, arg))
                                any = true;
                        }
                        if (!any)
                            return false;
                    } else if (op == "$in" || op == "$nin") {
                        if (arg.type() != bsoncxx::type::k_array)
                            return false;
                        bool matched = false;
                        for (const auto &target : arg.get_array().value) {
                            if (target.type() == bsoncxx::type::k_null && (!exists || sawMissingLeaf)) {
                                matched = true;
                                break;
                            }
                            for (const auto &v : values) {
                                if (matchesScalarOrElem(v.view(), target.get_value())) {
                                    matched = true;
                                    break;
                                }
                            }
                            if (matched)
                                break;
                        }
                        if (op == "$in" && !matched)
                            return false;
                        if (op == "$nin" && matched)
                            return false;
                    } else if (op == "$exists") {
                        const bool want = arg.type() == bsoncxx::type::k_bool
                            ? arg.get_bool().value : (toInt64Value(arg, 1) != 0);
                        if (exists != want)
                            return false;
                    } else if (op == "$type") {
                        bool any = false;
                        for (const auto &v : values) {
                            if (matchesTypeFilter(v.view(), arg))
                                any = true;
                            else if (v.view().type() == bsoncxx::type::k_array) {
                                for (const auto &it : v.view().get_array().value) {
                                    if (matchesTypeFilter(it.get_value(), arg))
                                        any = true;
                                }
                            }
                        }
                        if (!any)
                            return false;
                    } else if (op == "$not") {
                        if (matchesFieldCondition(values, exists, sawMissingLeaf, arg, depth + 1))
                            return false;
                    } else if (op == "$size") {
                        const std::int64_t want = toInt64Value(arg, -1);
                        bool any = false;
                        for (const auto &v : values) {
                            if (v.view().type() == bsoncxx::type::k_array) {
                                const std::int64_t len = std::distance(
                                    v.view().get_array().value.begin(),
                                    v.view().get_array().value.end());
                                if (len == want)
                                    any = true;
                            }
                        }
                        if (!any)
                            return false;
                    } else if (op == "$all") {
                        if (arg.type() != bsoncxx::type::k_array)
                            return false;
                        for (const auto &req : arg.get_array().value) {
                            bool foundReq = false;
                            for (const auto &v : values) {
                                if (matchesScalarOrElem(v.view(), req.get_value()))
                                    foundReq = true;
                            }
                            if (!foundReq)
                                return false;
                        }
                    } else if (op == "$elemMatch") {
                        if (arg.type() != bsoncxx::type::k_document)
                            return false;
                        const DocView elemFilter = arg.get_document().value;
                        const bool elemOps = isOperatorObject(elemFilter);
                        bool any = false;
                        for (const auto &v : values) {
                            if (v.view().type() != bsoncxx::type::k_array)
                                continue;
                            for (const auto &item : v.view().get_array().value) {
                                if (elemOps) {
                                    std::vector<BsonVal> single{BsonVal(item.get_value())};
                                    if (matchesFieldCondition(single, true, false, arg, depth + 1))
                                        any = true;
                                } else if (item.type() == bsoncxx::type::k_document) {
                                    if (matchesDocument(item.get_document().value, elemFilter, depth + 1))
                                        any = true;
                                }
                            }
                        }
                        if (!any)
                            return false;
                    } else {
                        return false;
                    }
                }
                return true;
            }

            if (cond.type() == bsoncxx::type::k_null) {
                if (!exists || sawMissingLeaf)
                    return true;
            }
            for (const auto &v : values) {
                if (matchesScalarOrElem(v.view(), cond))
                    return true;
            }
            return false;
        }

        bool matchesDocument(DocView doc, DocView filter, int depth)
        {
            if (depth > InMemoryDemoTransport::kMaxDepth)
                return false;
            for (const auto &el : filter) {
                const bsoncxx::stdx::string_view key = el.key();
                if (key == "$comment")
                    continue;
                if (key == "$and") {
                    if (el.type() != bsoncxx::type::k_array)
                        return false;
                    for (const auto &clause : el.get_array().value) {
                        if (clause.type() != bsoncxx::type::k_document
                            || !matchesDocument(doc, clause.get_document().value, depth + 1))
                            return false;
                    }
                    continue;
                }
                if (key == "$or") {
                    if (el.type() != bsoncxx::type::k_array)
                        return false;
                    bool any = false;
                    for (const auto &clause : el.get_array().value) {
                        if (clause.type() == bsoncxx::type::k_document
                            && matchesDocument(doc, clause.get_document().value, depth + 1)) {
                            any = true;
                            break;
                        }
                    }
                    if (!any)
                        return false;
                    continue;
                }
                if (key == "$nor") {
                    if (el.type() != bsoncxx::type::k_array)
                        return false;
                    for (const auto &clause : el.get_array().value) {
                        if (clause.type() == bsoncxx::type::k_document
                            && matchesDocument(doc, clause.get_document().value, depth + 1))
                            return false;
                    }
                    continue;
                }
                if (!key.empty() && key[0] == '$')
                    return false;

                const QStringList segments = strFromView(key).split(QLatin1Char('.'));
                std::vector<BsonVal> values;
                bool foundAny = false;
                bool sawMissingLeaf = false;
                collectPathValues(doc, segments, 0, values, &foundAny, &sawMissingLeaf, depth + 1);
                if (!matchesFieldCondition(values, foundAny, sawMissingLeaf, el.get_value(), depth + 1))
                    return false;
            }
            return true;
        }

        // Extracts sort key value for `path` following MongoDB's min/max rule on arrays.
        BsonVal extractSortValue(DocView doc, const QString &path, int dir)
        {
            const QStringList segments = path.split(QLatin1Char('.'));
            std::vector<BsonVal> values;
            bool foundAny = false;
            bool sawMissing = false;
            collectPathValues(doc, segments, 0, values, &foundAny, &sawMissing);
            if (!foundAny || values.empty())
                return BsonVal(bsoncxx::types::b_null{});

            std::vector<BsonVal> candidates;
            for (const auto &v : values) {
                if (v.view().type() == bsoncxx::type::k_array) {
                    const auto arr = v.view().get_array().value;
                    if (arr.empty()) {
                        candidates.emplace_back(bsoncxx::types::b_null{});
                    } else {
                        for (const auto &it : arr)
                            candidates.emplace_back(it.get_value());
                    }
                } else {
                    candidates.push_back(v);
                }
            }
            if (candidates.empty())
                return BsonVal(bsoncxx::types::b_null{});
            auto best = candidates.begin();
            for (auto it = candidates.begin() + 1; it != candidates.end(); ++it) {
                const int c = compareBsonValues(it->view(), best->view());
                if ((dir >= 0 && c < 0) || (dir < 0 && c > 0))
                    best = it;
            }
            return *best;
        }

        void sortDocuments(std::vector<DocVal> &docs, DocView sortSpec)
        {
            if (sortSpec.empty() || docs.size() <= 1)
                return;
            std::vector<std::pair<QString, int>> keys;
            for (const auto &el : sortSpec) {
                const std::int64_t dir = toInt64Option(el, 1);
                keys.emplace_back(strFromView(el.key()), dir < 0 ? -1 : 1);
            }
            std::stable_sort(docs.begin(), docs.end(), [&](const DocVal &a, const DocVal &b) {
                for (const auto &[path, dir] : keys) {
                    const BsonVal va = extractSortValue(a.view(), path, dir);
                    const BsonVal vb = extractSortValue(b.view(), path, dir);
                    const int c = compareBsonValues(va.view(), vb.view());
                    if (c != 0)
                        return dir >= 0 ? (c < 0) : (c > 0);
                }
                return false;
            });
        }

        // Evaluates simple aggregation expressions ("$field", "$a.b", literals, $ifNull, $concat,
        // $toLower, $toUpper, $add, $subtract, $multiply, $divide).
        BsonVal evalExpr(DocView doc, BsonView expr, bool *present = nullptr, int depth = 0);

        std::optional<BsonVal> getFirstPathValue(DocView doc, const QString &path)
        {
            const QStringList segments = path.split(QLatin1Char('.'));
            std::vector<BsonVal> values;
            bool found = false;
            bool missing = false;
            collectPathValues(doc, segments, 0, values, &found, &missing);
            if (!found || values.empty())
                return std::nullopt;
            if (values.size() == 1)
                return values.front();
            basic::array arr;
            for (const auto &v : values)
                arr.append(v.view());
            return BsonVal(arr.extract());
        }

        BsonVal makeAppropriateNumber(long double val, bool forceDouble = false)
        {
            if (!forceDouble && std::isfinite(val) && std::floor(val) == val
                && val >= static_cast<long double>(INT32_MIN)
                && val <= static_cast<long double>(INT32_MAX)) {
                return BsonVal(bsoncxx::types::b_int32{static_cast<std::int32_t>(val)});
            }
            if (!forceDouble && std::isfinite(val) && std::floor(val) == val
                && val >= -9007199254740991.0L && val <= 9007199254740991.0L) {
                return BsonVal(bsoncxx::types::b_int64{doubleToInt64(static_cast<double>(val), 0)});
            }
            return BsonVal(bsoncxx::types::b_double{static_cast<double>(val)});
        }

        BsonVal evalExpr(DocView doc, BsonView expr, bool *present, int depth)
        {
            if (present)
                *present = true;
            if (depth > InMemoryDemoTransport::kMaxDepth)
                return BsonVal(bsoncxx::types::b_null{});
            if (expr.type() == bsoncxx::type::k_string) {
                const bsoncxx::stdx::string_view sv = expr.get_string().value;
                if (sv.size() >= 2 && sv[0] == '$' && sv[1] != '$') {
                    const auto found = getFirstPathValue(doc, strFromView(sv.substr(1)));
                    if (!found) {
                        if (present)
                            *present = false;
                        return BsonVal(bsoncxx::types::b_null{});
                    }
                    return *found;
                }
                if (sv == "$$ROOT")
                    return BsonVal(DocVal(doc));
                return BsonVal(expr);
            }
            if (expr.type() == bsoncxx::type::k_document) {
                const DocView d = expr.get_document().value;
                if (d.empty())
                    return BsonVal(DocVal(d));
                const auto first = *d.begin();
                const bsoncxx::stdx::string_view k = first.key();
                if (!k.empty() && k[0] == '$') {
                    if (k == "$literal")
                        return BsonVal(first.get_value());
                    if (k == "$ifNull" && first.type() == bsoncxx::type::k_array) {
                        BsonVal last(bsoncxx::types::b_null{});
                        for (const auto &item : first.get_array().value) {
                            bool ok = false;
                            BsonVal v = evalExpr(doc, item.get_value(), &ok, depth + 1);
                            last = v;
                            if (ok && v.view().type() != bsoncxx::type::k_null
                                && v.view().type() != bsoncxx::type::k_undefined)
                                return v;
                        }
                        return last;
                    }
                    if (k == "$toLower" || k == "$toUpper") {
                        const BsonVal v = evalExpr(doc, first.get_value(), nullptr, depth + 1);
                        const QString s = v.view().type() == bsoncxx::type::k_string
                            ? strFromView(v.view().get_string().value) : QString();
                        const QByteArray res = (k == "$toLower" ? s.toLower() : s.toUpper()).toUtf8();
                        return BsonVal(std::string(res.constData(), static_cast<std::size_t>(res.size())));
                    }
                    if (k == "$concat" && first.type() == bsoncxx::type::k_array) {
                        QString out;
                        for (const auto &item : first.get_array().value) {
                            const BsonVal v = evalExpr(doc, item.get_value(), nullptr, depth + 1);
                            if (v.view().type() == bsoncxx::type::k_null)
                                return BsonVal(bsoncxx::types::b_null{});
                            if (v.view().type() == bsoncxx::type::k_string)
                                out += strFromView(v.view().get_string().value);
                        }
                        const QByteArray utf8 = out.toUtf8();
                        return BsonVal(std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
                    }
                    if ((k == "$add" || k == "$subtract" || k == "$multiply" || k == "$divide")
                        && first.type() == bsoncxx::type::k_array) {
                        std::vector<BsonVal> vals;
                        bool anyFloat = (k == "$divide");
                        for (const auto &item : first.get_array().value) {
                            BsonVal v = evalExpr(doc, item.get_value(), nullptr, depth + 1);
                            if (!isNumericType(v.view().type()))
                                return BsonVal(bsoncxx::types::b_null{});
                            if (v.view().type() == bsoncxx::type::k_double
                                || v.view().type() == bsoncxx::type::k_decimal128)
                                anyFloat = true;
                            vals.push_back(std::move(v));
                        }
                        if (vals.empty())
                            return BsonVal(bsoncxx::types::b_int32{0});
                        long double acc = asLongDouble(vals[0].view());
                        for (std::size_t i = 1; i < vals.size(); ++i) {
                            const long double rhs = asLongDouble(vals[i].view());
                            if (k == "$add")           acc += rhs;
                            else if (k == "$subtract") acc -= rhs;
                            else if (k == "$multiply") acc *= rhs;
                            else if (rhs == 0.0L)      return BsonVal(bsoncxx::types::b_null{});
                            else                       acc /= rhs;
                        }
                        return makeAppropriateNumber(acc, anyFloat);
                    }
                }
                // Composite object literal in $project / $group _id / $addFields.
                basic::document out;
                for (const auto &el : d) {
                    bool subPresent = false;
                    BsonVal v = evalExpr(doc, el.get_value(), &subPresent, depth + 1);
                    if (subPresent)
                        out.append(kvp(el.key(), v.view()));
                }
                return BsonVal(out.extract());
            }
            if (expr.type() == bsoncxx::type::k_array) {
                basic::array out;
                for (const auto &el : expr.get_array().value)
                    out.append(evalExpr(doc, el.get_value(), nullptr, depth + 1).view());
                return BsonVal(out.extract());
            }
            return BsonVal(expr);
        }

        bool isTruthyProjectionFlag(BsonView v)
        {
            if (v.type() == bsoncxx::type::k_bool)
                return v.get_bool().value;
            if (isNumericType(v.type()))
                return asLongDouble(v) != 0.0L;
            return true;
        }

        bool isPlainFlagValue(BsonView v)
        {
            return v.type() == bsoncxx::type::k_bool
                   || v.type() == bsoncxx::type::k_int32
                   || v.type() == bsoncxx::type::k_int64
                   || v.type() == bsoncxx::type::k_double;
        }

        DocVal applyProjection(DocView doc, DocView proj, bool allowExpressions)
        {
            if (proj.empty())
                return DocVal(doc);

            bool hasInclusion = false;
            bool excludeId = false;
            for (const auto &el : proj) {
                const bool flagPlain = isPlainFlagValue(el.get_value());
                const bool truthy = flagPlain ? isTruthyProjectionFlag(el.get_value()) : true;
                if (el.key() == "_id") {
                    if (flagPlain && !truthy)
                        excludeId = true;
                    else if (!flagPlain)
                        hasInclusion = true;
                } else if (truthy) {
                    hasInclusion = true;
                }
            }

            if (!hasInclusion) {
                // Exclusion mode: keep every field not excluded.
                std::set<std::string> excluded;
                for (const auto &el : proj) {
                    if (!isTruthyProjectionFlag(el.get_value()))
                        excluded.emplace(el.key());
                }
                basic::document out;
                for (const auto &el : doc) {
                    if (excluded.count(std::string(el.key())) == 0)
                        out.append(kvp(el.key(), el.get_value()));
                }
                return out.extract();
            }

            basic::document out;
            if (!excludeId && !proj["_id"]) {
                if (const auto id = doc["_id"])
                    out.append(kvp("_id", id.get_value()));
            }
            for (const auto &el : proj) {
                if (isPlainFlagValue(el.get_value())) {
                    if (!isTruthyProjectionFlag(el.get_value()))
                        continue;
                    const QString keyStr = strFromView(el.key());
                    if (!keyStr.contains(QLatin1Char('.'))) {
                        if (const auto src = doc[el.key()])
                            out.append(kvp(el.key(), src.get_value()));
                    } else if (const auto nested = getFirstPathValue(doc, keyStr)) {
                        out.append(kvp(el.key(), nested->view()));
                    }
                } else if (allowExpressions) {
                    bool present = false;
                    BsonVal v = evalExpr(doc, el.get_value(), &present);
                    if (present)
                        out.append(kvp(el.key(), v.view()));
                }
            }
            return out.extract();
        }

        struct WriteOpError
        {
            int code = 0;
            QString codeName;
            QString message;
        };

        bool validateWritePath(const QString &rawKey, QStringList *outParts, WriteOpError *err)
        {
            if (rawKey.isEmpty()) {
                if (err) {
                    err->code = 56;
                    err->codeName = QStringLiteral("EmptyFieldName");
                    err->message = QStringLiteral("An update path must not be empty");
                }
                return false;
            }
            const QStringList parts = rawKey.split(QLatin1Char('.'));
            if (parts.size() > InMemoryDemoTransport::kMaxDepth) {
                if (err) {
                    err->code = 2;
                    err->codeName = QStringLiteral("BadValue");
                    err->message = QStringLiteral("Field path exceeds maximum nesting depth");
                }
                return false;
            }
            for (const QString &seg : parts) {
                if (seg.isEmpty()) {
                    if (err) {
                        err->code = 56;
                        err->codeName = QStringLiteral("EmptyFieldName");
                        err->message = QStringLiteral("An update path must not contain empty field names");
                    }
                    return false;
                }
            }
            *outParts = parts;
            return true;
        }

        // Recursive dotted-path document mutation helpers for $set, $unset, $inc, $push, etc.
        DocVal setDocumentPath(DocView doc, const QStringList &parts, int idx, BsonView newVal)
        {
            if (idx >= parts.size() || idx > InMemoryDemoTransport::kMaxDepth)
                return DocVal(doc);
            const QByteArray keyUtf8 = parts[idx].toUtf8();
            const bsoncxx::stdx::string_view target(keyUtf8.constData(), static_cast<std::size_t>(keyUtf8.size()));
            basic::document out;
            bool replaced = false;
            for (const auto &el : doc) {
                if (el.key() == target) {
                    replaced = true;
                    if (idx + 1 == parts.size()) {
                        out.append(kvp(target, newVal));
                    } else if (el.type() == bsoncxx::type::k_document) {
                        DocVal sub = setDocumentPath(el.get_document().value, parts, idx + 1, newVal);
                        out.append(kvp(target, sub.view()));
                    } else {
                        DocVal sub = setDocumentPath(make_document().view(), parts, idx + 1, newVal);
                        out.append(kvp(target, sub.view()));
                    }
                } else {
                    out.append(kvp(el.key(), el.get_value()));
                }
            }
            if (!replaced) {
                if (idx + 1 == parts.size()) {
                    out.append(kvp(target, newVal));
                } else {
                    DocVal sub = setDocumentPath(make_document().view(), parts, idx + 1, newVal);
                    out.append(kvp(target, sub.view()));
                }
            }
            return out.extract();
        }

        DocVal unsetDocumentPath(DocView doc, const QStringList &parts, int idx)
        {
            if (idx >= parts.size() || idx > InMemoryDemoTransport::kMaxDepth)
                return DocVal(doc);
            const QByteArray keyUtf8 = parts[idx].toUtf8();
            const bsoncxx::stdx::string_view target(keyUtf8.constData(), static_cast<std::size_t>(keyUtf8.size()));
            basic::document out;
            for (const auto &el : doc) {
                if (el.key() == target) {
                    if (idx + 1 < parts.size() && el.type() == bsoncxx::type::k_document) {
                        DocVal sub = unsetDocumentPath(el.get_document().value, parts, idx + 1);
                        out.append(kvp(target, sub.view()));
                    }
                } else {
                    out.append(kvp(el.key(), el.get_value()));
                }
            }
            return out.extract();
        }

        std::optional<BsonVal> getExactPath(DocView doc, const QStringList &parts, int idx)
        {
            if (idx >= parts.size() || idx > InMemoryDemoTransport::kMaxDepth)
                return std::nullopt;
            const QByteArray keyUtf8 = parts[idx].toUtf8();
            const bsoncxx::stdx::string_view target(keyUtf8.constData(), static_cast<std::size_t>(keyUtf8.size()));
            const auto el = doc[target];
            if (!el)
                return std::nullopt;
            if (idx + 1 == parts.size())
                return BsonVal(el.get_value());
            if (el.type() == bsoncxx::type::k_document)
                return getExactPath(el.get_document().value, parts, idx + 1);
            return std::nullopt;
        }

        bool checkedAddInt64(std::int64_t a, std::int64_t b, std::int64_t *out)
        {
            if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
                return false;
            *out = a + b;
            return true;
        }

        bool checkedMulInt64(std::int64_t a, std::int64_t b, std::int64_t *out)
        {
            if (a == 0 || b == 0) {
                *out = 0;
                return true;
            }
            if ((a == INT64_MIN && b == -1) || (b == INT64_MIN && a == -1))
                return false;
            if (a > 0) {
                if (b > 0) {
                    if (a > INT64_MAX / b)
                        return false;
                } else {
                    if (b < INT64_MIN / a)
                        return false;
                }
            } else {
                if (b > 0) {
                    if (a < INT64_MIN / b)
                        return false;
                } else {
                    if (b < INT64_MAX / a)
                        return false;
                }
            }
            *out = a * b;
            return true;
        }

        BsonVal addNumericBson(BsonView cur, BsonView delta, bool multiply)
        {
            if ((cur.type() == bsoncxx::type::k_int32 || cur.type() == bsoncxx::type::k_int64)
                && (delta.type() == bsoncxx::type::k_int32 || delta.type() == bsoncxx::type::k_int64)) {
                const std::int64_t a = toInt64Value(cur);
                const std::int64_t b = toInt64Value(delta);
                std::int64_t r = 0;
                const bool exactOk = multiply ? checkedMulInt64(a, b, &r)
                                              : checkedAddInt64(a, b, &r);
                if (exactOk) {
                    if (cur.type() == bsoncxx::type::k_int32 && delta.type() == bsoncxx::type::k_int32
                        && r >= INT32_MIN && r <= INT32_MAX)
                        return BsonVal(bsoncxx::types::b_int32{static_cast<std::int32_t>(r)});
                    return BsonVal(bsoncxx::types::b_int64{r});
                }
            }
            const long double a = asLongDouble(cur);
            const long double b = asLongDouble(delta);
            const long double res = multiply ? (a * b) : (a + b);
            if (cur.type() == bsoncxx::type::k_decimal128 || delta.type() == bsoncxx::type::k_decimal128) {
                const QString text = QString::number(static_cast<double>(res), 'g', 15);
                return BsonVal(bsoncxx::types::b_decimal128{bsoncxx::decimal128(text.toStdString())});
            }
            return BsonVal(bsoncxx::types::b_double{static_cast<double>(res)});
        }

        bool failImmutableId(WriteOpError *err)
        {
            err->code = 66;
            err->codeName = QStringLiteral("ImmutableField");
            err->message = QStringLiteral(
                "Performing an update on the path '_id' would modify the immutable field '_id'");
            return false;
        }

        bool applyUpdateSpec(DocView input, DocView updateSpec, bool isInsert,
                             DocVal *outDoc, WriteOpError *err)
        {
            const BsonVal origId = input["_id"] ? BsonVal(input["_id"].get_value())
                                                : BsonVal(bsoncxx::oid{});
            if (!isOperatorObject(updateSpec)) {
                // Replacement document: keep original _id; forbid changing _id.
                if (const auto repId = updateSpec["_id"]) {
                    if (!isInsert && compareBsonValues(repId.get_value(), origId.view()) != 0)
                        return failImmutableId(err);
                }
                basic::document replaced;
                replaced.append(kvp("_id", origId.view()));
                for (const auto &el : updateSpec) {
                    if (el.key() == "_id")
                        continue;
                    replaced.append(kvp(el.key(), el.get_value()));
                }
                *outDoc = replaced.extract();
                return true;
            }

            DocVal cur(input);
            if (!cur.view()["_id"])
                cur = setDocumentPath(cur.view(), QStringList{QStringLiteral("_id")}, 0, origId.view());

            for (const auto &opEl : updateSpec) {
                const bsoncxx::stdx::string_view op = opEl.key();
                if (opEl.type() != bsoncxx::type::k_document) {
                    err->code = 9;
                    err->codeName = QStringLiteral("FailedToParse");
                    err->message = QStringLiteral("Modifiers operate on fields but found non-object");
                    return false;
                }
                const DocView fields = opEl.get_document().value;

                if (op == "$setOnInsert") {
                    for (const auto &f : fields) {
                        QStringList parts;
                        if (!validateWritePath(strFromView(f.key()), &parts, err))
                            return false;
                        if (parts.first() == QLatin1String("_id")) {
                            if (parts.size() > 1
                                || (isInsert && compareBsonValues(f.get_value(), origId.view()) != 0))
                                return failImmutableId(err);
                        }
                        if (isInsert)
                            cur = setDocumentPath(cur.view(), parts, 0, f.get_value());
                    }
                } else if (op == "$set") {
                    for (const auto &f : fields) {
                        QStringList parts;
                        if (!validateWritePath(strFromView(f.key()), &parts, err))
                            return false;
                        if (parts.first() == QLatin1String("_id")) {
                            if (parts.size() > 1 || compareBsonValues(f.get_value(), origId.view()) != 0)
                                return failImmutableId(err);
                        }
                        cur = setDocumentPath(cur.view(), parts, 0, f.get_value());
                    }
                } else if (op == "$unset") {
                    for (const auto &f : fields) {
                        QStringList parts;
                        if (!validateWritePath(strFromView(f.key()), &parts, err))
                            return false;
                        if (parts.first() == QLatin1String("_id"))
                            return failImmutableId(err);
                        cur = unsetDocumentPath(cur.view(), parts, 0);
                    }
                } else if (op == "$inc" || op == "$mul") {
                    const bool mul = (op == "$mul");
                    for (const auto &f : fields) {
                        QStringList parts;
                        if (!validateWritePath(strFromView(f.key()), &parts, err))
                            return false;
                        if (parts.first() == QLatin1String("_id"))
                            return failImmutableId(err);
                        if (!isNumericType(f.get_value().type())) {
                            err->code = 14;
                            err->codeName = QStringLiteral("TypeMismatch");
                            err->message = QStringLiteral("Cannot apply numeric modifier to non-numeric value");
                            return false;
                        }
                        const auto existing = getExactPath(cur.view(), parts, 0);
                        BsonVal next(bsoncxx::types::b_int32{0});
                        if (!existing) {
                            next = mul ? makeAppropriateNumber(0.0L) : BsonVal(f.get_value());
                        } else if (!isNumericType(existing->view().type())) {
                            err->code = 14;
                            err->codeName = QStringLiteral("TypeMismatch");
                            err->message = QStringLiteral("Cannot apply numeric modifier to non-numeric field");
                            return false;
                        } else {
                            next = addNumericBson(existing->view(), f.get_value(), mul);
                        }
                        cur = setDocumentPath(cur.view(), parts, 0, next.view());
                    }
                } else if (op == "$min" || op == "$max") {
                    const bool isMin = (op == "$min");
                    for (const auto &f : fields) {
                        QStringList parts;
                        if (!validateWritePath(strFromView(f.key()), &parts, err))
                            return false;
                        if (parts.first() == QLatin1String("_id"))
                            return failImmutableId(err);
                        const auto existing = getExactPath(cur.view(), parts, 0);
                        if (!existing) {
                            cur = setDocumentPath(cur.view(), parts, 0, f.get_value());
                        } else {
                            const int c = compareBsonValues(f.get_value(), existing->view());
                            if ((isMin && c < 0) || (!isMin && c > 0))
                                cur = setDocumentPath(cur.view(), parts, 0, f.get_value());
                        }
                    }
                } else if (op == "$rename") {
                    for (const auto &f : fields) {
                        if (f.type() != bsoncxx::type::k_string) {
                            err->code = 2;
                            err->codeName = QStringLiteral("BadValue");
                            err->message = QStringLiteral("Invalid $rename target");
                            return false;
                        }
                        QStringList srcParts;
                        QStringList dstParts;
                        if (!validateWritePath(strFromView(f.key()), &srcParts, err)
                            || !validateWritePath(strFromView(f.get_string().value), &dstParts, err))
                            return false;
                        if (srcParts.first() == QLatin1String("_id")
                            || dstParts.first() == QLatin1String("_id"))
                            return failImmutableId(err);
                        if (const auto existing = getExactPath(cur.view(), srcParts, 0)) {
                            cur = unsetDocumentPath(cur.view(), srcParts, 0);
                            cur = setDocumentPath(cur.view(), dstParts, 0, existing->view());
                        }
                    }
                } else if (op == "$currentDate") {
                    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch());
                    for (const auto &f : fields) {
                        QStringList parts;
                        if (!validateWritePath(strFromView(f.key()), &parts, err))
                            return false;
                        if (parts.first() == QLatin1String("_id"))
                            return failImmutableId(err);
                        cur = setDocumentPath(cur.view(), parts, 0,
                                              BsonVal(bsoncxx::types::b_date{nowMs}).view());
                    }
                } else if (op == "$push" || op == "$addToSet") {
                    const bool unique = (op == "$addToSet");
                    for (const auto &f : fields) {
                        QStringList parts;
                        if (!validateWritePath(strFromView(f.key()), &parts, err))
                            return false;
                        if (parts.first() == QLatin1String("_id"))
                            return failImmutableId(err);
                        const auto existing = getExactPath(cur.view(), parts, 0);
                        std::vector<BsonVal> items;
                        if (existing) {
                            if (existing->view().type() != bsoncxx::type::k_array) {
                                err->code = 2;
                                err->codeName = QStringLiteral("BadValue");
                                err->message = QStringLiteral("The field is not an array");
                                return false;
                            }
                            for (const auto &it : existing->view().get_array().value)
                                items.emplace_back(it.get_value());
                        }
                        std::vector<BsonView> toAppend;
                        if (f.type() == bsoncxx::type::k_document && f.get_document().value["$each"]
                            && f.get_document().value["$each"].type() == bsoncxx::type::k_array) {
                            for (const auto &e : f.get_document().value["$each"].get_array().value)
                                toAppend.push_back(e.get_value());
                        } else {
                            toAppend.push_back(f.get_value());
                        }
                        for (const auto &candidate : toAppend) {
                            if (unique) {
                                bool dup = false;
                                for (const auto &curItem : items) {
                                    if (compareBsonValues(curItem.view(), candidate) == 0) {
                                        dup = true;
                                        break;
                                    }
                                }
                                if (dup)
                                    continue;
                            }
                            items.emplace_back(candidate);
                        }
                        basic::array arr;
                        for (const auto &it : items)
                            arr.append(it.view());
                        cur = setDocumentPath(cur.view(), parts, 0, BsonVal(arr.extract()).view());
                    }
                } else if (op == "$pull" || op == "$pop") {
                    for (const auto &f : fields) {
                        QStringList parts;
                        if (!validateWritePath(strFromView(f.key()), &parts, err))
                            return false;
                        if (parts.first() == QLatin1String("_id"))
                            return failImmutableId(err);
                        const auto existing = getExactPath(cur.view(), parts, 0);
                        if (!existing || existing->view().type() != bsoncxx::type::k_array)
                            continue;
                        std::vector<BsonVal> items;
                        for (const auto &it : existing->view().get_array().value)
                            items.emplace_back(it.get_value());
                        if (op == "$pop") {
                            if (!items.empty()) {
                                if (toInt64Value(f.get_value(), 1) < 0)
                                    items.erase(items.begin());
                                else
                                    items.pop_back();
                            }
                        } else {
                            const BsonView cond = f.get_value();
                            const bool condDoc = cond.type() == bsoncxx::type::k_document;
                            const bool condOps = condDoc && isOperatorObject(cond.get_document().value);
                            items.erase(std::remove_if(items.begin(), items.end(), [&](const BsonVal &it) {
                                if (condOps) {
                                    std::vector<BsonVal> one{it};
                                    return matchesFieldCondition(one, true, false, cond);
                                }
                                if (condDoc && it.view().type() == bsoncxx::type::k_document)
                                    return matchesDocument(it.view().get_document().value,
                                                           cond.get_document().value);
                                return compareBsonValues(it.view(), cond) == 0;
                            }), items.end());
                        }
                        basic::array arr;
                        for (const auto &it : items)
                            arr.append(it.view());
                        cur = setDocumentPath(cur.view(), parts, 0, BsonVal(arr.extract()).view());
                    }
                } else {
                    err->code = 9;
                    err->codeName = QStringLiteral("FailedToParse");
                    err->message = QStringLiteral("Unsupported update operator '%1' in demo transport")
                                       .arg(strFromView(op));
                    return false;
                }
            }
            *outDoc = std::move(cur);
            return true;
        }

        // Seed base document for upsert from equality predicates in `query`.
        DocVal buildUpsertBase(DocView query)
        {
            DocVal base = make_document(kvp("_id", bsoncxx::oid{}));
            for (const auto &el : query) {
                if (el.key().empty() || el.key()[0] == '$')
                    continue;
                QStringList parts;
                if (!validateWritePath(strFromView(el.key()), &parts, nullptr))
                    continue;
                if (parts.first() == QLatin1String("_id") && parts.size() > 1)
                    continue;
                if (el.type() == bsoncxx::type::k_document) {
                    const DocView sub = el.get_document().value;
                    if (isOperatorObject(sub)) {
                        if (const auto eq = sub["$eq"]) {
                            base = setDocumentPath(base.view(), parts, 0, eq.get_value());
                        }
                        continue;
                    }
                }
                base = setDocumentPath(base.view(), parts, 0, el.get_value());
            }
            return base;
        }

        QString defaultIndexName(DocView keySpec)
        {
            QStringList parts;
            for (const auto &el : keySpec) {
                QString valStr;
                if (el.type() == bsoncxx::type::k_string)
                    valStr = strFromView(el.get_string().value);
                else
                    valStr = QString::number(toInt64Option(el, 1));
                parts << (strFromView(el.key()) + QLatin1Char('_') + valStr);
            }
            return parts.join(QLatin1Char('_'));
        }
    } // namespace

    // =========================================================================
    // InMemoryDemoStore
    // =========================================================================

    class InMemoryDemoStore
    {
    public:
        struct CollectionData
        {
            std::vector<DocVal> docs;
            std::vector<DocVal> indexes;
            DocVal options = make_document();
        };

        InMemoryDemoStore()
        {
            resetUnlocked();
        }

        void reset()
        {
            std::lock_guard<std::mutex> lock(_mutex);
            resetUnlocked();
        }

        IMongoTransport::Result listDatabases()
        {
            std::lock_guard<std::mutex> lock(_mutex);
            basic::array out;
            for (const auto &[dbName, colls] : _dbs) {
                std::int64_t bytes = 0;
                for (const auto &[_, c] : colls) {
                    for (const auto &d : c.docs)
                        bytes += static_cast<std::int64_t>(d.view().length());
                }
                out.append(make_document(
                    kvp("name", dbName.toStdString()),
                    kvp("sizeOnDisk", std::max<std::int64_t>(bytes, 4096)),
                    kvp("empty", colls.empty())));
            }
            return okArray(out.view());
        }

        IMongoTransport::Result listCollections(const QString &dbName, DocView filter = {},
                                                bool nameOnly = false)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            std::vector<DocVal> out;
            if (const auto dbIt = _dbs.find(dbName); dbIt != _dbs.end()) {
                for (const auto &[collName, c] : dbIt->second) {
                    DocVal desc = nameOnly
                        ? make_document(kvp("name", collName.toStdString()),
                                        kvp("type", "collection"))
                        : make_document(kvp("name", collName.toStdString()),
                                        kvp("type", "collection"),
                                        kvp("options", c.options.view()),
                                        kvp("info", make_document(kvp("readOnly", false))));
                    if (filter.empty() || matchesDocument(desc.view(), filter))
                        out.push_back(std::move(desc));
                }
            }
            return wrapCursor(dbName + QStringLiteral(".$cmd.listCollections"), out);
        }

        IMongoTransport::Result runCommand(const QString &database, DocView cmd)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            return runCommandUnlocked(database, cmd);
        }

        static std::map<QString, CollectionData> buildCatalogSeed()
        {
            std::map<QString, CollectionData> colls;

            auto makeIdIndex = [](const std::string &ns) {
                return make_document(
                    kvp("v", 2),
                    kvp("key", make_document(kvp("_id", 1))),
                    kvp("name", "_id_"),
                    kvp("ns", ns));
            };

            CollectionData products;
            products.docs.push_back(make_document(
                kvp("_id", 1),
                kvp("sku", "KB-01"),
                kvp("name", "Mechanical Keyboard"),
                kvp("category", "peripherals"),
                kvp("price", 89.90),
                kvp("stock", 34),
                kvp("active", true),
                kvp("tags", make_array("input", "usb"))));
            products.docs.push_back(make_document(
                kvp("_id", 2),
                kvp("sku", "MS-07"),
                kvp("name", "Wireless Mouse"),
                kvp("category", "peripherals"),
                kvp("price", 39.50),
                kvp("stock", 120),
                kvp("active", true),
                kvp("tags", make_array("input", "wireless"))));
            products.docs.push_back(make_document(
                kvp("_id", 3),
                kvp("sku", "MN-27"),
                kvp("name", "27\" Monitor"),
                kvp("category", "displays"),
                kvp("price", 249.00),
                kvp("stock", 12),
                kvp("active", true),
                kvp("tags", make_array("display"))));
            products.indexes.push_back(makeIdIndex("catalog.products"));
            products.indexes.push_back(make_document(
                kvp("v", 2),
                kvp("key", make_document(kvp("sku", 1))),
                kvp("name", "sku_1"),
                kvp("unique", true),
                kvp("ns", "catalog.products")));
            colls.emplace(QStringLiteral("products"), std::move(products));

            CollectionData customers;
            customers.docs.push_back(make_document(
                kvp("_id", "c1"),
                kvp("name", "Ada Lovelace"),
                kvp("tier", "gold"),
                kvp("country", "UK")));
            customers.docs.push_back(make_document(
                kvp("_id", "c2"),
                kvp("name", "Grace Hopper"),
                kvp("tier", "silver"),
                kvp("country", "US")));
            customers.docs.push_back(make_document(
                kvp("_id", "c3"),
                kvp("name", "Edsger Dijkstra"),
                kvp("tier", "gold"),
                kvp("country", "NL")));
            customers.indexes.push_back(makeIdIndex("catalog.customers"));
            colls.emplace(QStringLiteral("customers"), std::move(customers));

            CollectionData orders;
            orders.docs.push_back(make_document(
                kvp("_id", 101),
                kvp("customerId", "c1"),
                kvp("sku", "KB-01"),
                kvp("qty", 1),
                kvp("total", 89.90),
                kvp("status", "shipped"),
                kvp("placedAt", bsoncxx::types::b_date{std::chrono::milliseconds(1791796500000LL)})));
            orders.docs.push_back(make_document(
                kvp("_id", 102),
                kvp("customerId", "c2"),
                kvp("sku", "MS-07"),
                kvp("qty", 2),
                kvp("total", 79.00),
                kvp("status", "delivered"),
                kvp("placedAt", bsoncxx::types::b_date{std::chrono::milliseconds(1792333800000LL)})));
            orders.docs.push_back(make_document(
                kvp("_id", 103),
                kvp("customerId", "c1"),
                kvp("sku", "MN-27"),
                kvp("qty", 1),
                kvp("total", 249.00),
                kvp("status", "processing"),
                kvp("placedAt", bsoncxx::types::b_date{std::chrono::milliseconds(1793617500000LL)})));
            orders.indexes.push_back(makeIdIndex("catalog.orders"));
            orders.indexes.push_back(make_document(
                kvp("v", 2),
                kvp("key", make_document(kvp("customerId", 1), kvp("placedAt", -1))),
                kvp("name", "customerId_1_placedAt_-1"),
                kvp("ns", "catalog.orders")));
            colls.emplace(QStringLiteral("orders"), std::move(orders));

            return colls;
        }

    private:
        void resetUnlocked()
        {
            _dbs.clear();
            _dbs.emplace(QStringLiteral("catalog"), buildCatalogSeed());
        }

        CollectionData *findCollection(const QString &db, const QString &coll)
        {
            auto dbIt = _dbs.find(db);
            if (dbIt == _dbs.end())
                return nullptr;
            auto cIt = dbIt->second.find(coll);
            return cIt == dbIt->second.end() ? nullptr : &cIt->second;
        }

        CollectionData *ensureCollection(const QString &db, const QString &coll,
                                         IMongoTransport::Result *quotaErr)
        {
            if (db.isEmpty() || coll.isEmpty() || db == QLatin1String("admin")
                || db == QLatin1String("local") || db == QLatin1String("config")) {
                *quotaErr = errResult(QStringLiteral("Cannot write to reserved database '%1'").arg(db),
                                      73, QStringLiteral("InvalidNamespace"));
                return nullptr;
            }
            auto dbIt = _dbs.find(db);
            if (dbIt == _dbs.end()) {
                if (_dbs.size() >= InMemoryDemoTransport::kMaxDatabases) {
                    *quotaErr = errResult(
                        QStringLiteral("Demo playground limit reached (max %1 databases)")
                            .arg(InMemoryDemoTransport::kMaxDatabases),
                        8000, QStringLiteral("AtlasError"));
                    return nullptr;
                }
                dbIt = _dbs.emplace(db, std::map<QString, CollectionData>{}).first;
            }
            auto cIt = dbIt->second.find(coll);
            if (cIt == dbIt->second.end()) {
                if (dbIt->second.size() >= InMemoryDemoTransport::kMaxCollectionsPerDb) {
                    *quotaErr = errResult(
                        QStringLiteral("Demo playground limit reached (max %1 collections per database)")
                            .arg(InMemoryDemoTransport::kMaxCollectionsPerDb),
                        8000, QStringLiteral("AtlasError"));
                    return nullptr;
                }
                CollectionData created;
                created.indexes.push_back(make_document(
                    kvp("v", 2),
                    kvp("key", make_document(kvp("_id", 1))),
                    kvp("name", "_id_"),
                    kvp("ns", (db + QLatin1Char('.') + coll).toStdString())));
                cIt = dbIt->second.emplace(coll, std::move(created)).first;
            }
            return &cIt->second;
        }

        // Checks `_id` uniqueness and any `unique: true` secondary index on `collData`.
        bool violatesUniqueIndex(const CollectionData &collData, DocView candidate,
                                 std::optional<std::size_t> skipIndex, QString *indexName) const
        {
            for (const auto &idxDoc : collData.indexes) {
                const DocView iv = idxDoc.view();
                const bool isId = iv["name"] && iv["name"].type() == bsoncxx::type::k_string
                                  && iv["name"].get_string().value == "_id_";
                const bool unique = isId || toBoolOption(iv["unique"], false);
                if (!unique || !iv["key"] || iv["key"].type() != bsoncxx::type::k_document)
                    continue;
                const DocView keySpec = iv["key"].get_document().value;

                auto extractKeyTuple = [&](DocView d) {
                    basic::array tuple;
                    for (const auto &k : keySpec) {
                        const auto v = getFirstPathValue(d, strFromView(k.key()));
                        if (v)
                            tuple.append(v->view());
                        else
                            tuple.append(bsoncxx::types::b_null{});
                    }
                    return BsonVal(tuple.extract());
                };

                const BsonVal candKey = extractKeyTuple(candidate);
                for (std::size_t i = 0; i < collData.docs.size(); ++i) {
                    if (skipIndex && *skipIndex == i)
                        continue;
                    const BsonVal existingKey = extractKeyTuple(collData.docs[i].view());
                    if (compareBsonValues(candKey.view(), existingKey.view()) == 0) {
                        if (indexName && iv["name"] && iv["name"].type() == bsoncxx::type::k_string)
                            *indexName = strFromView(iv["name"].get_string().value);
                        return true;
                    }
                }
            }
            return false;
        }

        IMongoTransport::Result runFindUnlocked(const QString &db, const QString &coll, DocView cmd,
                                                std::size_t *outDocsExamined = nullptr)
        {
            const CollectionData *c = findCollection(db, coll);
            const DocView filter = cmd["filter"] && cmd["filter"].type() == bsoncxx::type::k_document
                ? cmd["filter"].get_document().value
                : DocView{};
            const DocView sortSpec = cmd["sort"] && cmd["sort"].type() == bsoncxx::type::k_document
                ? cmd["sort"].get_document().value
                : DocView{};
            const DocView proj = cmd["projection"] && cmd["projection"].type() == bsoncxx::type::k_document
                ? cmd["projection"].get_document().value
                : DocView{};
            const std::int64_t skip = std::max<std::int64_t>(0, toInt64Option(cmd["skip"], 0));
            const std::int64_t rawLimit = toInt64Option(cmd["limit"], 0);
            const std::int64_t limit = rawLimit < 0
                ? (rawLimit == INT64_MIN ? INT64_MAX : -rawLimit)
                : rawLimit;

            std::vector<DocVal> matched;
            if (c) {
                if (outDocsExamined)
                    *outDocsExamined = c->docs.size();
                for (const auto &d : c->docs) {
                    if (filter.empty() || matchesDocument(d.view(), filter))
                        matched.emplace_back(d.view());
                }
            } else if (outDocsExamined) {
                *outDocsExamined = 0;
            }

            if (!sortSpec.empty())
                sortDocuments(matched, sortSpec);

            std::vector<DocVal> sliced;
            const std::size_t start = static_cast<std::size_t>(
                std::min<std::int64_t>(skip, static_cast<std::int64_t>(matched.size())));
            for (std::size_t i = start; i < matched.size(); ++i) {
                if (limit > 0 && static_cast<std::int64_t>(sliced.size()) >= limit)
                    break;
                sliced.push_back(proj.empty() ? std::move(matched[i])
                                              : applyProjection(matched[i].view(), proj, false));
            }
            return wrapCursor(db + QLatin1Char('.') + coll, sliced);
        }

        IMongoTransport::Result runAggregateUnlocked(const QString &db, const QString &coll, DocView cmd)
        {
            if (!cmd["pipeline"] || cmd["pipeline"].type() != bsoncxx::type::k_array)
                return errResult(QStringLiteral("'pipeline' option must be specified as an array"),
                                 14, QStringLiteral("TypeMismatch"));
            const auto pipeArr = cmd["pipeline"].get_array().value;
            if (static_cast<std::size_t>(std::distance(pipeArr.begin(), pipeArr.end()))
                > InMemoryDemoTransport::kMaxPipelineStages) {
                return errResult(
                    QStringLiteral("Pipeline exceeds demo stage limit (max %1 stages)")
                        .arg(InMemoryDemoTransport::kMaxPipelineStages),
                    8000, QStringLiteral("AtlasError"));
            }

            const CollectionData *c = findCollection(db, coll);
            std::vector<DocVal> docs;
            if (c) {
                docs.reserve(c->docs.size());
                for (const auto &d : c->docs)
                    docs.emplace_back(d.view());
            }

            for (const auto &stageEl : pipeArr) {
                if (stageEl.type() != bsoncxx::type::k_document || stageEl.get_document().value.empty())
                    return errResult(QStringLiteral("Each pipeline stage must be a non-empty object"),
                                     14, QStringLiteral("TypeMismatch"));
                const DocView s = stageEl.get_document().value;
                const auto opEl = *s.begin();
                const bsoncxx::stdx::string_view stage = opEl.key();

                if (stage == "$match") {
                    if (opEl.type() != bsoncxx::type::k_document)
                        return errResult(QStringLiteral("$match requires an object"), 14,
                                         QStringLiteral("TypeMismatch"));
                    const DocView m = opEl.get_document().value;
                    std::vector<DocVal> next;
                    for (auto &d : docs) {
                        if (matchesDocument(d.view(), m))
                            next.push_back(std::move(d));
                    }
                    docs = std::move(next);
                } else if (stage == "$sort") {
                    if (opEl.type() != bsoncxx::type::k_document)
                        return errResult(QStringLiteral("$sort requires an object"), 14,
                                         QStringLiteral("TypeMismatch"));
                    sortDocuments(docs, opEl.get_document().value);
                } else if (stage == "$skip") {
                    const std::int64_t n = toInt64Option(opEl, -1);
                    if (n < 0)
                        return errResult(QStringLiteral("$skip requires a non-negative integer"),
                                         2, QStringLiteral("BadValue"));
                    if (static_cast<std::size_t>(n) >= docs.size())
                        docs.clear();
                    else if (n > 0)
                        docs.erase(docs.begin(), docs.begin() + static_cast<std::ptrdiff_t>(n));
                } else if (stage == "$limit") {
                    const std::int64_t n = toInt64Option(opEl, 0);
                    if (n <= 0)
                        return errResult(QStringLiteral("$limit requires a positive integer"),
                                         2, QStringLiteral("BadValue"));
                    if (static_cast<std::size_t>(n) < docs.size())
                        docs.erase(docs.begin() + static_cast<std::ptrdiff_t>(n), docs.end());
                } else if (stage == "$count") {
                    if (opEl.type() != bsoncxx::type::k_string || opEl.get_string().value.empty())
                        return errResult(QStringLiteral("$count requires a non-empty field name"),
                                         2, QStringLiteral("BadValue"));
                    const std::string field(opEl.get_string().value);
                    std::vector<DocVal> next;
                    if (!docs.empty()) {
                        next.push_back(make_document(
                            kvp(field, static_cast<std::int32_t>(docs.size()))));
                    }
                    docs = std::move(next);
                } else if (stage == "$project") {
                    if (opEl.type() != bsoncxx::type::k_document)
                        return errResult(QStringLiteral("$project requires an object"), 14,
                                         QStringLiteral("TypeMismatch"));
                    const DocView p = opEl.get_document().value;
                    for (auto &d : docs) {
                        d = applyProjection(d.view(), p, /*allowExpressions=*/true);
                        if (d.view().length() > InMemoryDemoTransport::kMaxDocumentBytes)
                            return errResult(QStringLiteral("Pipeline document exceeds demo 256 KiB limit"),
                                             10334, QStringLiteral("BSONObjectTooLarge"));
                    }
                } else if (stage == "$addFields" || stage == "$set") {
                    if (opEl.type() != bsoncxx::type::k_document)
                        return errResult(QStringLiteral("$addFields/$set requires an object"), 14,
                                         QStringLiteral("TypeMismatch"));
                    const DocView addSpec = opEl.get_document().value;
                    std::vector<std::pair<QStringList, BsonView>> parsedFields;
                    for (const auto &f : addSpec) {
                        QStringList parts;
                        WriteOpError pathErr;
                        if (!validateWritePath(strFromView(f.key()), &parts, &pathErr))
                            return errResult(pathErr.message, pathErr.code, pathErr.codeName);
                        parsedFields.emplace_back(std::move(parts), f.get_value());
                    }
                    for (auto &d : docs) {
                        DocVal cur(d.view());
                        for (const auto &[parts, exprView] : parsedFields) {
                            BsonVal v = evalExpr(d.view(), exprView);
                            cur = setDocumentPath(cur.view(), parts, 0, v.view());
                        }
                        if (cur.view().length() > InMemoryDemoTransport::kMaxDocumentBytes)
                            return errResult(QStringLiteral("Pipeline document exceeds demo 256 KiB limit"),
                                             10334, QStringLiteral("BSONObjectTooLarge"));
                        d = std::move(cur);
                    }
                } else if (stage == "$unset") {
                    std::vector<QStringList> paths;
                    auto pushUnsetPath = [&](bsoncxx::stdx::string_view sv) -> std::optional<IMongoTransport::Result> {
                        QStringList parts;
                        WriteOpError pathErr;
                        if (!validateWritePath(strFromView(sv), &parts, &pathErr))
                            return errResult(pathErr.message, pathErr.code, pathErr.codeName);
                        paths.push_back(std::move(parts));
                        return std::nullopt;
                    };
                    if (opEl.type() == bsoncxx::type::k_string) {
                        if (const auto err = pushUnsetPath(opEl.get_string().value))
                            return *err;
                    } else if (opEl.type() == bsoncxx::type::k_array) {
                        for (const auto &it : opEl.get_array().value) {
                            if (it.type() != bsoncxx::type::k_string)
                                return errResult(QStringLiteral("$unset requires a string or array of strings"),
                                                 2, QStringLiteral("BadValue"));
                            if (const auto err = pushUnsetPath(it.get_string().value))
                                return *err;
                        }
                    } else {
                        return errResult(QStringLiteral("$unset requires a string or array of strings"),
                                         2, QStringLiteral("BadValue"));
                    }
                    for (auto &d : docs) {
                        DocVal cur(d.view());
                        for (const auto &p : paths)
                            cur = unsetDocumentPath(cur.view(), p, 0);
                        d = std::move(cur);
                    }
                } else if (stage == "$sample") {
                    if (opEl.type() != bsoncxx::type::k_document)
                        return errResult(QStringLiteral("$sample requires an object"), 14,
                                         QStringLiteral("TypeMismatch"));
                    const std::int64_t size = toInt64Option(opEl.get_document().value["size"], 0);
                    if (size < 0)
                        return errResult(QStringLiteral("$sample size must be non-negative"),
                                         2, QStringLiteral("BadValue"));
                    std::mt19937 rng{std::random_device{}()};
                    std::shuffle(docs.begin(), docs.end(), rng);
                    if (static_cast<std::size_t>(size) < docs.size())
                        docs.erase(docs.begin() + static_cast<std::ptrdiff_t>(size), docs.end());
                } else if (stage == "$unwind") {
                    QString path;
                    bool preserveEmpty = false;
                    if (opEl.type() == bsoncxx::type::k_string) {
                        path = strFromView(opEl.get_string().value);
                    } else if (opEl.type() == bsoncxx::type::k_document) {
                        const DocView u = opEl.get_document().value;
                        if (u["path"] && u["path"].type() == bsoncxx::type::k_string)
                            path = strFromView(u["path"].get_string().value);
                        preserveEmpty = toBoolOption(u["preserveNullAndEmptyArrays"], false);
                    }
                    if (!path.startsWith(QLatin1Char('$')) || path.size() < 2)
                        return errResult(QStringLiteral("$unwind requires a '$field' path"),
                                         2, QStringLiteral("BadValue"));
                    QStringList parts;
                    WriteOpError pathErr;
                    if (!validateWritePath(path.mid(1), &parts, &pathErr))
                        return errResult(pathErr.message, pathErr.code, pathErr.codeName);
                    std::vector<DocVal> next;
                    for (const auto &d : docs) {
                        const auto val = getExactPath(d.view(), parts, 0);
                        if (!val || val->view().type() == bsoncxx::type::k_null) {
                            if (preserveEmpty)
                                next.emplace_back(d.view());
                        } else if (val->view().type() == bsoncxx::type::k_array) {
                            const auto arr = val->view().get_array().value;
                            if (arr.empty()) {
                                if (preserveEmpty)
                                    next.push_back(unsetDocumentPath(d.view(), parts, 0));
                            } else {
                                for (const auto &elem : arr) {
                                    if (next.size() >= InMemoryDemoTransport::kMaxPipelineDocs) {
                                        return errResult(
                                            QStringLiteral("$unwind exceeded demo pipeline document cap"),
                                            8000, QStringLiteral("AtlasError"));
                                    }
                                    DocVal expanded = setDocumentPath(d.view(), parts, 0, elem.get_value());
                                    if (expanded.view().length() > InMemoryDemoTransport::kMaxDocumentBytes)
                                        return errResult(
                                            QStringLiteral("Pipeline document exceeds demo 256 KiB limit"),
                                            10334, QStringLiteral("BSONObjectTooLarge"));
                                    next.push_back(std::move(expanded));
                                }
                            }
                        } else {
                            next.emplace_back(d.view());
                        }
                    }
                    docs = std::move(next);
                } else if (stage == "$group" || stage == "$sortByCount") {
                    DocVal synthGroup = make_document();
                    DocView g = stage == "$group" && opEl.type() == bsoncxx::type::k_document
                        ? opEl.get_document().value : DocView{};
                    if (stage == "$sortByCount") {
                        synthGroup = make_document(
                            kvp("_id", opEl.get_value()),
                            kvp("count", make_document(kvp("$sum", 1))));
                        g = synthGroup.view();
                    }
                    if (g.empty() || !g["_id"])
                        return errResult(QStringLiteral("$group specification must include an _id"),
                                         15955, QStringLiteral("Location15955"));

                    struct Bucket
                    {
                        BsonVal id{bsoncxx::types::b_null{}};
                        std::vector<DocView> members;
                    };
                    std::map<BsonVal, std::size_t, BsonValueLess> indexById;
                    std::vector<Bucket> buckets;
                    const BsonView idExpr = g["_id"].get_value();
                    for (const auto &d : docs) {
                        BsonVal key = evalExpr(d.view(), idExpr);
                        auto [it, inserted] = indexById.emplace(key, buckets.size());
                        if (inserted)
                            buckets.push_back(Bucket{std::move(key), {}});
                        buckets[it->second].members.push_back(d.view());
                    }

                    std::vector<DocVal> grouped;
                    grouped.reserve(buckets.size());
                    for (const auto &b : buckets) {
                        basic::document out;
                        out.append(kvp("_id", b.id.view()));
                        for (const auto &accEl : g) {
                            if (accEl.key() == "_id")
                                continue;
                            if (accEl.type() != bsoncxx::type::k_document
                                || accEl.get_document().value.empty()) {
                                return errResult(QStringLiteral("Invalid $group accumulator"),
                                                 15952, QStringLiteral("Location15952"));
                            }
                            const auto accOp = *accEl.get_document().value.begin();
                            const bsoncxx::stdx::string_view accName = accOp.key();
                            const BsonView accExpr = accOp.get_value();

                            if (accName == "$count") {
                                out.append(kvp(accEl.key(),
                                               static_cast<std::int32_t>(b.members.size())));
                            } else if (accName == "$sum" || accName == "$avg") {
                                long double sum = 0.0L;
                                std::size_t numCount = 0;
                                bool anyFloat = (accName == "$avg");
                                for (const DocView m : b.members) {
                                    const BsonVal v = evalExpr(m, accExpr);
                                    if (isNumericType(v.view().type())) {
                                        if (v.view().type() == bsoncxx::type::k_double
                                            || v.view().type() == bsoncxx::type::k_decimal128)
                                            anyFloat = true;
                                        sum += asLongDouble(v.view());
                                        ++numCount;
                                    }
                                }
                                if (accName == "$avg") {
                                    if (numCount == 0)
                                        out.append(kvp(accEl.key(), bsoncxx::types::b_null{}));
                                    else
                                        out.append(kvp(accEl.key(),
                                                       static_cast<double>(sum / numCount)));
                                } else {
                                    out.append(kvp(accEl.key(),
                                                   makeAppropriateNumber(sum, anyFloat).view()));
                                }
                            } else if (accName == "$min" || accName == "$max") {
                                const bool isMin = (accName == "$min");
                                std::optional<BsonVal> best;
                                for (const DocView m : b.members) {
                                    bool ok = false;
                                    BsonVal v = evalExpr(m, accExpr, &ok);
                                    if (!ok || v.view().type() == bsoncxx::type::k_null
                                        || v.view().type() == bsoncxx::type::k_undefined)
                                        continue;
                                    if (!best) {
                                        best = std::move(v);
                                    } else {
                                        const int cmp = compareBsonValues(v.view(), best->view());
                                        if ((isMin && cmp < 0) || (!isMin && cmp > 0))
                                            best = std::move(v);
                                    }
                                }
                                if (best)
                                    out.append(kvp(accEl.key(), best->view()));
                                else
                                    out.append(kvp(accEl.key(), bsoncxx::types::b_null{}));
                            } else if (accName == "$first" || accName == "$last") {
                                if (b.members.empty()) {
                                    out.append(kvp(accEl.key(), bsoncxx::types::b_null{}));
                                } else {
                                    const DocView target = accName == "$first"
                                        ? b.members.front() : b.members.back();
                                    out.append(kvp(accEl.key(), evalExpr(target, accExpr).view()));
                                }
                            } else if (accName == "$push" || accName == "$addToSet") {
                                const bool unique = (accName == "$addToSet");
                                std::set<BsonVal, BsonValueLess> seen;
                                basic::array arr;
                                for (const DocView m : b.members) {
                                    bool ok = false;
                                    BsonVal v = evalExpr(m, accExpr, &ok);
                                    if (unique) {
                                        if (!ok)
                                            continue;
                                        if (!seen.insert(v).second)
                                            continue;
                                    }
                                    arr.append(v.view());
                                }
                                out.append(kvp(accEl.key(), arr.extract()));
                            } else {
                                return errResult(
                                    QStringLiteral("Unsupported $group accumulator '%1' in demo transport")
                                        .arg(strFromView(accName)),
                                    15952, QStringLiteral("Location15952"));
                            }
                        }
                        DocVal builtGroup = out.extract();
                        if (builtGroup.view().length() > InMemoryDemoTransport::kMaxDocumentBytes)
                            return errResult(QStringLiteral("Pipeline document exceeds demo 256 KiB limit"),
                                             10334, QStringLiteral("BSONObjectTooLarge"));
                        grouped.push_back(std::move(builtGroup));
                    }
                    if (stage == "$sortByCount")
                        sortDocuments(grouped, make_document(kvp("count", -1), kvp("_id", 1)).view());
                    docs = std::move(grouped);
                } else {
                    return errResult(
                        QStringLiteral("Unsupported aggregation stage '%1' in demo transport")
                            .arg(strFromView(stage)),
                        40324, QStringLiteral("UnrecognizedPipelineStage"));
                }
            }
            return wrapCursor(db + QLatin1Char('.') + coll, docs);
        }

        IMongoTransport::Result runInsertUnlocked(const QString &db, const QString &coll, DocView cmd)
        {
            if (!cmd["documents"] || cmd["documents"].type() != bsoncxx::type::k_array)
                return errResult(QStringLiteral("'documents' must be an array"), 14,
                                 QStringLiteral("TypeMismatch"));

            IMongoTransport::Result quotaErr;
            CollectionData *c = ensureCollection(db, coll, &quotaErr);
            if (!c)
                return quotaErr;

            const bool ordered = toBoolOption(cmd["ordered"], true);
            std::int32_t inserted = 0;
            basic::array writeErrors;
            int idx = 0;
            for (const auto &item : cmd["documents"].get_array().value) {
                const int curIdx = idx++;
                if (item.type() != bsoncxx::type::k_document) {
                    writeErrors.append(make_document(
                        kvp("index", curIdx),
                        kvp("code", 14),
                        kvp("codeName", "TypeMismatch"),
                        kvp("errmsg", "Document to insert must be an object")));
                    if (ordered)
                        break;
                    continue;
                }
                DocView raw = item.get_document().value;
                if (raw.length() > InMemoryDemoTransport::kMaxDocumentBytes) {
                    writeErrors.append(make_document(
                        kvp("index", curIdx),
                        kvp("code", 10334),
                        kvp("codeName", "BSONObjectTooLarge"),
                        kvp("errmsg", "Document exceeds demo 256 KiB limit")));
                    if (ordered)
                        break;
                    continue;
                }
                if (c->docs.size() >= InMemoryDemoTransport::kMaxDocumentsPerCollection) {
                    writeErrors.append(make_document(
                        kvp("index", curIdx),
                        kvp("code", 8000),
                        kvp("codeName", "AtlasError"),
                        kvp("errmsg", "Demo playground limit reached (max 5000 documents per collection)")));
                    if (ordered)
                        break;
                    continue;
                }

                DocVal candidate(raw);
                if (!raw["_id"]) {
                    basic::document withId;
                    withId.append(kvp("_id", bsoncxx::oid{}));
                    for (const auto &el : raw)
                        withId.append(kvp(el.key(), el.get_value()));
                    candidate = withId.extract();
                }

                QString dupIdx;
                if (violatesUniqueIndex(*c, candidate.view(), std::nullopt, &dupIdx)) {
                    const QString msg = QStringLiteral(
                        "E11000 duplicate key error collection: %1.%2 index: %3")
                        .arg(db, coll, dupIdx.isEmpty() ? QStringLiteral("_id_") : dupIdx);
                    writeErrors.append(make_document(
                        kvp("index", curIdx),
                        kvp("code", 11000),
                        kvp("codeName", "DuplicateKey"),
                        kvp("errmsg", msg.toStdString())));
                    if (ordered)
                        break;
                    continue;
                }

                c->docs.push_back(std::move(candidate));
                ++inserted;
            }

            basic::document reply;
            reply.append(kvp("n", inserted), kvp("ok", 1.0));
            const DocVal errsHolder = make_document(kvp("e", writeErrors.extract()));
            const auto errArr = errsHolder.view()["e"].get_array().value;
            if (!errArr.empty())
                reply.append(kvp("writeErrors", errArr));
            return okDoc(reply.extract().view());
        }

        IMongoTransport::Result runUpdateUnlocked(const QString &db, const QString &coll, DocView cmd)
        {
            if (!cmd["updates"] || cmd["updates"].type() != bsoncxx::type::k_array)
                return errResult(QStringLiteral("'updates' must be an array"), 14,
                                 QStringLiteral("TypeMismatch"));

            const bool ordered = toBoolOption(cmd["ordered"], true);
            std::int32_t matched = 0;
            std::int32_t modified = 0;
            basic::array upserted;
            basic::array writeErrors;
            int specIndex = 0;

            for (const auto &specEl : cmd["updates"].get_array().value) {
                const int curSpecIdx = specIndex++;
                if (specEl.type() != bsoncxx::type::k_document)
                    continue;
                const DocView spec = specEl.get_document().value;
                const DocView q = spec["q"] && spec["q"].type() == bsoncxx::type::k_document
                    ? spec["q"].get_document().value : DocView{};
                if (!spec["u"] || spec["u"].type() != bsoncxx::type::k_document) {
                    writeErrors.append(make_document(
                        kvp("index", curSpecIdx),
                        kvp("code", 9),
                        kvp("codeName", "FailedToParse"),
                        kvp("errmsg", "Update specification 'u' must be an object")));
                    if (ordered)
                        break;
                    continue;
                }
                const DocView u = spec["u"].get_document().value;
                const bool multi = toBoolOption(spec["multi"], false);
                const bool upsert = toBoolOption(spec["upsert"], false);

                if (multi && !isOperatorObject(u)) {
                    writeErrors.append(make_document(
                        kvp("index", curSpecIdx),
                        kvp("code", 9),
                        kvp("codeName", "FailedToParse"),
                        kvp("errmsg", "multi update only works with $ operators")));
                    if (ordered)
                        break;
                    continue;
                }

                CollectionData *c = findCollection(db, coll);
                std::vector<std::size_t> hitIndices;
                if (c) {
                    for (std::size_t i = 0; i < c->docs.size(); ++i) {
                        if (q.empty() || matchesDocument(c->docs[i].view(), q)) {
                            hitIndices.push_back(i);
                            if (!multi)
                                break;
                        }
                    }
                }

                if (hitIndices.empty()) {
                    if (!upsert)
                        continue;
                    IMongoTransport::Result quotaErr;
                    c = ensureCollection(db, coll, &quotaErr);
                    if (!c) {
                        writeErrors.append(make_document(
                            kvp("index", curSpecIdx),
                            kvp("code", quotaErr.code),
                            kvp("codeName", quotaErr.codeName.toStdString()),
                            kvp("errmsg", quotaErr.error.toStdString())));
                        if (ordered)
                            break;
                        continue;
                    }
                    if (c->docs.size() >= InMemoryDemoTransport::kMaxDocumentsPerCollection) {
                        writeErrors.append(make_document(
                            kvp("index", curSpecIdx),
                            kvp("code", 8000),
                            kvp("codeName", "AtlasError"),
                            kvp("errmsg", "Demo playground limit reached (max 5000 documents per collection)")));
                        if (ordered)
                            break;
                        continue;
                    }
                    const DocVal seed = buildUpsertBase(q);
                    DocVal created = make_document();
                    WriteOpError opErr;
                    if (!applyUpdateSpec(seed.view(), u, /*isInsert=*/true, &created, &opErr)) {
                        writeErrors.append(make_document(
                            kvp("index", curSpecIdx),
                            kvp("code", opErr.code),
                            kvp("codeName", opErr.codeName.toStdString()),
                            kvp("errmsg", opErr.message.toStdString())));
                        if (ordered)
                            break;
                        continue;
                    }
                    if (created.view().length() > InMemoryDemoTransport::kMaxDocumentBytes) {
                        writeErrors.append(make_document(
                            kvp("index", curSpecIdx),
                            kvp("code", 10334),
                            kvp("codeName", "BSONObjectTooLarge"),
                            kvp("errmsg", "Document exceeds demo 256 KiB limit")));
                        if (ordered)
                            break;
                        continue;
                    }
                    QString dupIdx;
                    if (violatesUniqueIndex(*c, created.view(), std::nullopt, &dupIdx)) {
                        const QString msg = QStringLiteral(
                            "E11000 duplicate key error collection: %1.%2 index: %3")
                            .arg(db, coll, dupIdx.isEmpty() ? QStringLiteral("_id_") : dupIdx);
                        writeErrors.append(make_document(
                            kvp("index", curSpecIdx),
                            kvp("code", 11000),
                            kvp("codeName", "DuplicateKey"),
                            kvp("errmsg", msg.toStdString())));
                        if (ordered)
                            break;
                        continue;
                    }
                    upserted.append(make_document(
                        kvp("index", curSpecIdx),
                        kvp("_id", created.view()["_id"].get_value())));
                    c->docs.push_back(std::move(created));
                    ++matched;
                    continue;
                }

                bool specFailed = false;
                for (const std::size_t docIdx : hitIndices) {
                    DocVal next = make_document();
                    WriteOpError opErr;
                    if (!applyUpdateSpec(c->docs[docIdx].view(), u, /*isInsert=*/false, &next, &opErr)) {
                        writeErrors.append(make_document(
                            kvp("index", curSpecIdx),
                            kvp("code", opErr.code),
                            kvp("codeName", opErr.codeName.toStdString()),
                            kvp("errmsg", opErr.message.toStdString())));
                        specFailed = true;
                        break;
                    }
                    if (next.view().length() > InMemoryDemoTransport::kMaxDocumentBytes) {
                        writeErrors.append(make_document(
                            kvp("index", curSpecIdx),
                            kvp("code", 10334),
                            kvp("codeName", "BSONObjectTooLarge"),
                            kvp("errmsg", "Document exceeds demo 256 KiB limit")));
                        specFailed = true;
                        break;
                    }
                    QString dupIdx;
                    if (violatesUniqueIndex(*c, next.view(), docIdx, &dupIdx)) {
                        const QString msg = QStringLiteral(
                            "E11000 duplicate key error collection: %1.%2 index: %3")
                            .arg(db, coll, dupIdx.isEmpty() ? QStringLiteral("_id_") : dupIdx);
                        writeErrors.append(make_document(
                            kvp("index", curSpecIdx),
                            kvp("code", 11000),
                            kvp("codeName", "DuplicateKey"),
                            kvp("errmsg", msg.toStdString())));
                        specFailed = true;
                        break;
                    }
                    ++matched;
                    if (compareDocuments(c->docs[docIdx].view(), next.view()) != 0)
                        ++modified;
                    c->docs[docIdx] = std::move(next);
                }
                if (specFailed && ordered)
                    break;
            }

            basic::document reply;
            reply.append(kvp("n", matched), kvp("nModified", modified), kvp("ok", 1.0));
            const DocVal upsHolder = make_document(kvp("u", upserted.extract()));
            if (const auto uArr = upsHolder.view()["u"].get_array().value; !uArr.empty())
                reply.append(kvp("upserted", uArr));
            const DocVal errHolder = make_document(kvp("e", writeErrors.extract()));
            if (const auto eArr = errHolder.view()["e"].get_array().value; !eArr.empty())
                reply.append(kvp("writeErrors", eArr));
            return okDoc(reply.extract().view());
        }

        IMongoTransport::Result runDeleteUnlocked(const QString &db, const QString &coll, DocView cmd)
        {
            if (!cmd["deletes"] || cmd["deletes"].type() != bsoncxx::type::k_array)
                return errResult(QStringLiteral("'deletes' must be an array"), 14,
                                 QStringLiteral("TypeMismatch"));

            CollectionData *c = findCollection(db, coll);
            std::int32_t removed = 0;
            if (c) {
                for (const auto &specEl : cmd["deletes"].get_array().value) {
                    if (specEl.type() != bsoncxx::type::k_document)
                        continue;
                    const DocView spec = specEl.get_document().value;
                    const DocView q = spec["q"] && spec["q"].type() == bsoncxx::type::k_document
                        ? spec["q"].get_document().value : DocView{};
                    const std::int64_t limit = toInt64Option(spec["limit"], 0);
                    for (auto it = c->docs.begin(); it != c->docs.end();) {
                        if (q.empty() || matchesDocument(it->view(), q)) {
                            it = c->docs.erase(it);
                            ++removed;
                            if (limit == 1)
                                break;
                        } else {
                            ++it;
                        }
                    }
                }
            }
            return okDoc(make_document(kvp("n", removed), kvp("ok", 1.0)).view());
        }

        IMongoTransport::Result runFindAndModifyUnlocked(const QString &db, const QString &coll, DocView cmd)
        {
            const DocView q = cmd["query"] && cmd["query"].type() == bsoncxx::type::k_document
                ? cmd["query"].get_document().value : DocView{};
            const DocView sortSpec = cmd["sort"] && cmd["sort"].type() == bsoncxx::type::k_document
                ? cmd["sort"].get_document().value : DocView{};
            const DocView fields = cmd["fields"] && cmd["fields"].type() == bsoncxx::type::k_document
                ? cmd["fields"].get_document().value : DocView{};
            const bool isRemove = toBoolOption(cmd["remove"], false);
            const bool returnNew = toBoolOption(cmd["new"], false);
            const bool upsert = toBoolOption(cmd["upsert"], false);
            const bool hasUpdate = cmd["update"] && cmd["update"].type() == bsoncxx::type::k_document;

            if (!isRemove && !hasUpdate)
                return errResult(QStringLiteral("Either an update or remove=true must be specified"),
                                 9, QStringLiteral("FailedToParse"));

            CollectionData *c = findCollection(db, coll);
            std::optional<std::size_t> targetIdx;
            if (c) {
                std::vector<std::pair<DocVal, std::size_t>> candidates;
                for (std::size_t i = 0; i < c->docs.size(); ++i) {
                    if (q.empty() || matchesDocument(c->docs[i].view(), q))
                        candidates.emplace_back(DocVal(c->docs[i].view()), i);
                }
                if (!candidates.empty()) {
                    if (!sortSpec.empty() && candidates.size() > 1) {
                        std::vector<DocVal> sortedDocs;
                        sortedDocs.reserve(candidates.size());
                        for (const auto &p : candidates)
                            sortedDocs.emplace_back(p.first.view());
                        sortDocuments(sortedDocs, sortSpec);
                        const BsonView winningId = sortedDocs.front().view()["_id"].get_value();
                        for (const auto &p : candidates) {
                            if (compareBsonValues(p.first.view()["_id"].get_value(), winningId) == 0) {
                                targetIdx = p.second;
                                break;
                            }
                        }
                    } else {
                        targetIdx = candidates.front().second;
                    }
                }
            }

            if (!targetIdx) {
                if (isRemove || !upsert) {
                    return okDoc(make_document(
                        kvp("lastErrorObject", make_document(kvp("n", 0), kvp("updatedExisting", false))),
                        kvp("value", bsoncxx::types::b_null{}),
                        kvp("ok", 1.0)).view());
                }
                IMongoTransport::Result quotaErr;
                c = ensureCollection(db, coll, &quotaErr);
                if (!c)
                    return quotaErr;
                if (c->docs.size() >= InMemoryDemoTransport::kMaxDocumentsPerCollection)
                    return errResult(QStringLiteral("Demo playground limit reached"),
                                     8000, QStringLiteral("AtlasError"));
                const DocVal seed = buildUpsertBase(q);
                DocVal created = make_document();
                WriteOpError opErr;
                if (!applyUpdateSpec(seed.view(), cmd["update"].get_document().value, true, &created, &opErr))
                    return errResult(opErr.message, opErr.code, opErr.codeName);
                if (created.view().length() > InMemoryDemoTransport::kMaxDocumentBytes)
                    return errResult(QStringLiteral("Document exceeds demo 256 KiB limit"),
                                     10334, QStringLiteral("BSONObjectTooLarge"));
                QString dupIdx;
                if (violatesUniqueIndex(*c, created.view(), std::nullopt, &dupIdx))
                    return errResult(QStringLiteral("E11000 duplicate key error collection: %1.%2")
                                         .arg(db, coll),
                                     11000, QStringLiteral("DuplicateKey"));
                const BsonVal newId(created.view()["_id"].get_value());
                DocVal projected = fields.empty() ? DocVal(created.view())
                                                  : applyProjection(created.view(), fields, false);
                c->docs.push_back(std::move(created));
                return okDoc(make_document(
                    kvp("lastErrorObject", make_document(
                        kvp("n", 1),
                        kvp("updatedExisting", false),
                        kvp("upserted", newId.view()))),
                    kvp("value", returnNew ? BsonVal(projected.view()).view()
                                           : BsonVal(bsoncxx::types::b_null{}).view()),
                    kvp("ok", 1.0)).view());
            }

            const DocVal before(c->docs[*targetIdx].view());
            if (isRemove) {
                c->docs.erase(c->docs.begin() + static_cast<std::ptrdiff_t>(*targetIdx));
                const DocVal projected = fields.empty() ? before
                                                        : applyProjection(before.view(), fields, false);
                return okDoc(make_document(
                    kvp("lastErrorObject", make_document(kvp("n", 1))),
                    kvp("value", projected.view()),
                    kvp("ok", 1.0)).view());
            }

            DocVal after = make_document();
            WriteOpError opErr;
            if (!applyUpdateSpec(before.view(), cmd["update"].get_document().value, false, &after, &opErr))
                return errResult(opErr.message, opErr.code, opErr.codeName);
            if (after.view().length() > InMemoryDemoTransport::kMaxDocumentBytes)
                return errResult(QStringLiteral("Document exceeds demo 256 KiB limit"),
                                 10334, QStringLiteral("BSONObjectTooLarge"));
            QString dupIdx;
            if (violatesUniqueIndex(*c, after.view(), *targetIdx, &dupIdx))
                return errResult(QStringLiteral("E11000 duplicate key error collection: %1.%2")
                                     .arg(db, coll),
                                 11000, QStringLiteral("DuplicateKey"));
            c->docs[*targetIdx] = DocVal(after.view());
            const DocVal chosen = returnNew ? after : before;
            const DocVal projected = fields.empty() ? chosen
                                                    : applyProjection(chosen.view(), fields, false);
            return okDoc(make_document(
                kvp("lastErrorObject", make_document(kvp("n", 1), kvp("updatedExisting", true))),
                kvp("value", projected.view()),
                kvp("ok", 1.0)).view());
        }

        IMongoTransport::Result runDistinctUnlocked(const QString &db, const QString &coll, DocView cmd)
        {
            if (!cmd["key"] || cmd["key"].type() != bsoncxx::type::k_string)
                return errResult(QStringLiteral("'key' field must be a string"), 14,
                                 QStringLiteral("TypeMismatch"));
            const QString keyPath = strFromView(cmd["key"].get_string().value);
            const QStringList segments = keyPath.split(QLatin1Char('.'));
            const DocView q = cmd["query"] && cmd["query"].type() == bsoncxx::type::k_document
                ? cmd["query"].get_document().value : DocView{};

            std::set<BsonVal, BsonValueLess> uniqueVals;
            if (const CollectionData *c = findCollection(db, coll)) {
                for (const auto &d : c->docs) {
                    if (!q.empty() && !matchesDocument(d.view(), q))
                        continue;
                    std::vector<BsonVal> vals;
                    bool found = false, missing = false;
                    collectPathValues(d.view(), segments, 0, vals, &found, &missing);
                    for (const auto &v : vals) {
                        if (v.view().type() == bsoncxx::type::k_array) {
                            for (const auto &elem : v.view().get_array().value)
                                uniqueVals.emplace(elem.get_value());
                        } else {
                            uniqueVals.insert(v);
                        }
                    }
                }
            }
            basic::array out;
            for (const auto &v : uniqueVals)
                out.append(v.view());
            return okDoc(make_document(kvp("values", out.extract()), kvp("ok", 1.0)).view());
        }

        IMongoTransport::Result runExplainUnlocked(const QString &db, DocView cmd)
        {
            if (!cmd["explain"] || cmd["explain"].type() != bsoncxx::type::k_document)
                return errResult(QStringLiteral("explain requires a command document"),
                                 9, QStringLiteral("FailedToParse"));
            const DocView inner = cmd["explain"].get_document().value;
            QString coll;
            DocView filter{};
            std::int32_t nReturned = 0;
            std::int32_t docsExamined = 0;

            if (inner["find"] && inner["find"].type() == bsoncxx::type::k_string) {
                coll = strFromView(inner["find"].get_string().value);
                if (inner["filter"] && inner["filter"].type() == bsoncxx::type::k_document)
                    filter = inner["filter"].get_document().value;
                std::size_t examined = 0;
                const auto r = runFindUnlocked(db, coll, inner, &examined);
                if (!r.ok)
                    return r;
                docsExamined = static_cast<std::int32_t>(examined);
                const auto parsed = bsoncxx::from_json(r.ejson.toStdString());
                const auto batch = parsed.view()["cursor"]["firstBatch"].get_array().value;
                nReturned = static_cast<std::int32_t>(std::distance(batch.begin(), batch.end()));
            } else if (inner["aggregate"] && inner["aggregate"].type() == bsoncxx::type::k_string) {
                coll = strFromView(inner["aggregate"].get_string().value);
                if (const CollectionData *c = findCollection(db, coll))
                    docsExamined = static_cast<std::int32_t>(c->docs.size());
                const auto r = runAggregateUnlocked(db, coll, inner);
                if (!r.ok)
                    return r;
                const auto parsed = bsoncxx::from_json(r.ejson.toStdString());
                const auto batch = parsed.view()["cursor"]["firstBatch"].get_array().value;
                nReturned = static_cast<std::int32_t>(std::distance(batch.begin(), batch.end()));
            } else if (inner["count"] && inner["count"].type() == bsoncxx::type::k_string) {
                coll = strFromView(inner["count"].get_string().value);
                if (inner["query"] && inner["query"].type() == bsoncxx::type::k_document)
                    filter = inner["query"].get_document().value;
                if (const CollectionData *c = findCollection(db, coll)) {
                    docsExamined = static_cast<std::int32_t>(c->docs.size());
                    for (const auto &d : c->docs) {
                        if (filter.empty() || matchesDocument(d.view(), filter))
                            ++nReturned;
                    }
                }
            } else {
                for (const auto &el : inner) {
                    if (el.type() == bsoncxx::type::k_string) {
                        coll = strFromView(el.get_string().value);
                        break;
                    }
                }
            }

            // Use IXSCAN in the plan summary when an equality filter hits an indexed leading key.
            QString usedIndex;
            DocVal usedKeyPattern = make_document();
            if (const CollectionData *c = findCollection(db, coll); c && !filter.empty()) {
                for (const auto &idx : c->indexes) {
                    const DocView iv = idx.view();
                    if (!iv["key"] || iv["key"].type() != bsoncxx::type::k_document)
                        continue;
                    const DocView keySpec = iv["key"].get_document().value;
                    if (keySpec.empty())
                        continue;
                    const auto lead = *keySpec.begin();
                    if (filter[lead.key()]) {
                        usedIndex = iv["name"] && iv["name"].type() == bsoncxx::type::k_string
                            ? strFromView(iv["name"].get_string().value) : QStringLiteral("_id_");
                        usedKeyPattern = DocVal(keySpec);
                        break;
                    }
                }
            }

            const std::string ns = (db + QLatin1Char('.') + coll).toStdString();
            const DocVal winningPlan = usedIndex.isEmpty()
                ? make_document(
                    kvp("stage", "COLLSCAN"),
                    kvp("filter", filter),
                    kvp("direction", "forward"))
                : make_document(
                    kvp("stage", "FETCH"),
                    kvp("inputStage", make_document(
                        kvp("stage", "IXSCAN"),
                        kvp("indexName", usedIndex.toStdString()),
                        kvp("keyPattern", usedKeyPattern.view()),
                        kvp("direction", "forward"))));
            const DocVal execStages = usedIndex.isEmpty()
                ? make_document(
                    kvp("stage", "COLLSCAN"),
                    kvp("filter", filter),
                    kvp("nReturned", nReturned),
                    kvp("docsExamined", docsExamined),
                    kvp("executionTimeMillisEstimate", 0))
                : make_document(
                    kvp("stage", "FETCH"),
                    kvp("nReturned", nReturned),
                    kvp("docsExamined", nReturned),
                    kvp("executionTimeMillisEstimate", 0),
                    kvp("inputStage", make_document(
                        kvp("stage", "IXSCAN"),
                        kvp("indexName", usedIndex.toStdString()),
                        kvp("keyPattern", usedKeyPattern.view()),
                        kvp("keysExamined", nReturned),
                        kvp("nReturned", nReturned),
                        kvp("executionTimeMillisEstimate", 0))));

            return okDoc(make_document(
                kvp("queryPlanner", make_document(
                    kvp("plannerVersion", 1),
                    kvp("namespace", ns),
                    kvp("indexFilterSet", false),
                    kvp("parsedQuery", filter),
                    kvp("winningPlan", winningPlan.view()),
                    kvp("rejectedPlans", make_array()))),
                kvp("executionStats", make_document(
                    kvp("executionSuccess", true),
                    kvp("nReturned", nReturned),
                    kvp("executionTimeMillis", 0),
                    kvp("totalKeysExamined", usedIndex.isEmpty() ? 0 : nReturned),
                    kvp("totalDocsExamined", usedIndex.isEmpty() ? docsExamined : nReturned),
                    kvp("executionStages", execStages.view()))),
                kvp("serverInfo", make_document(
                    kvp("host", "demo"),
                    kvp("port", 27017),
                    kvp("version", kDemoServerVersion))),
                kvp("ok", 1.0)).view());
        }

        IMongoTransport::Result runCommandUnlocked(const QString &database, DocView cmd)
        {
            if (cmd.empty())
                return errResult(QStringLiteral("unreadable command"), 9,
                                 QStringLiteral("FailedToParse"));

            // Admin / diagnostic commands allowed on any database.
            if (cmd["ping"]) {
                IMongoTransport::Result r;
                r.ok = true;
                r.ejson = QStringLiteral(R"({"ok":1})");
                return r;
            }
            if (cmd["hello"] || cmd["isMaster"] || cmd["ismaster"]) {
                return okDoc(make_document(
                    kvp("ismaster", true),
                    kvp("isWritablePrimary", true),
                    kvp("maxBsonObjectSize", static_cast<std::int32_t>(16 * 1024 * 1024)),
                    kvp("maxMessageSizeBytes", static_cast<std::int32_t>(48 * 1024 * 1024)),
                    kvp("maxWriteBatchSize", static_cast<std::int32_t>(1000)),
                    kvp("minWireVersion", 0),
                    kvp("maxWireVersion", 21),
                    kvp("readOnly", false),
                    kvp("ok", 1.0)).view());
            }
            if (cmd["buildInfo"] || cmd["buildinfo"]) {
                return okDoc(make_document(
                    kvp("version", kDemoServerVersion),
                    kvp("versionArray", make_array(8, 0, 0, 0)),
                    kvp("bits", 64),
                    kvp("maxBsonObjectSize", static_cast<std::int32_t>(16 * 1024 * 1024)),
                    kvp("ok", 1.0)).view());
            }
            if (cmd["serverStatus"]) {
                return okDoc(make_document(
                    kvp("host", "demo"),
                    kvp("version", kDemoServerVersion),
                    kvp("process", "in-memory-playground"),
                    kvp("storageEngine", make_document(kvp("name", "inMemoryDemo"))),
                    kvp("ok", 1.0)).view());
            }
            if (cmd["hostInfo"]) {
                return okDoc(make_document(
                    kvp("system", make_document(kvp("hostname", "demo"))),
                    kvp("os", make_document(kvp("type", "WebAssembly"))),
                    kvp("ok", 1.0)).view());
            }
            if (cmd["connectionStatus"]) {
                return okDoc(make_document(
                    kvp("authInfo", make_document(
                        kvp("authenticatedUsers", make_array()),
                        kvp("authenticatedUserRoles", make_array()))),
                    kvp("ok", 1.0)).view());
            }
            if (cmd["whatsmyuri"])
                return okDoc(make_document(kvp("you", "127.0.0.1:0"), kvp("ok", 1.0)).view());
            if (cmd["getCmdLineOpts"]) {
                return okDoc(make_document(
                    kvp("argv", make_array("in-memory-demo")),
                    kvp("parsed", make_document()),
                    kvp("ok", 1.0)).view());
            }
            if (cmd["getLog"]) {
                return okDoc(make_document(
                    kvp("totalLinesWritten", 0),
                    kvp("log", make_array()),
                    kvp("ok", 1.0)).view());
            }
            if (cmd["currentOp"])
                return okDoc(make_document(kvp("inprog", make_array()), kvp("ok", 1.0)).view());
            if (cmd["killOp"])
                return okDoc(make_document(kvp("info", "demo: nothing running"), kvp("ok", 1.0)).view());
            if (cmd["killCursors"]) {
                return okDoc(make_document(
                    kvp("cursorsKilled", make_array()),
                    kvp("cursorsNotFound", make_array()),
                    kvp("cursorsAlive", make_array()),
                    kvp("cursorsUnknown", make_array()),
                    kvp("ok", 1.0)).view());
            }
            if (cmd["listDatabases"]) {
                const auto arrRes = [&]() {
                    basic::array dbs;
                    std::int64_t totalSize = 0;
                    for (const auto &[dbName, colls] : _dbs) {
                        std::int64_t bytes = 0;
                        for (const auto &[_, c] : colls) {
                            for (const auto &d : c.docs)
                                bytes += static_cast<std::int64_t>(d.view().length());
                        }
                        const std::int64_t sz = std::max<std::int64_t>(bytes, 4096);
                        totalSize += sz;
                        dbs.append(make_document(
                            kvp("name", dbName.toStdString()),
                            kvp("sizeOnDisk", sz),
                            kvp("empty", colls.empty())));
                    }
                    return make_document(
                        kvp("databases", dbs.extract()),
                        kvp("totalSize", totalSize),
                        kvp("ok", 1.0));
                }();
                return okDoc(arrRes.view());
            }

            if (cmd["explain"])
                return runExplainUnlocked(database, cmd);

            if (cmd["listCollections"]) {
                const DocView filter = cmd["filter"] && cmd["filter"].type() == bsoncxx::type::k_document
                    ? cmd["filter"].get_document().value : DocView{};
                const bool nameOnly = toBoolOption(cmd["nameOnly"], false);
                std::vector<DocVal> out;
                if (const auto dbIt = _dbs.find(database); dbIt != _dbs.end()) {
                    for (const auto &[collName, c] : dbIt->second) {
                        DocVal desc = nameOnly
                            ? make_document(kvp("name", collName.toStdString()),
                                            kvp("type", "collection"))
                            : make_document(kvp("name", collName.toStdString()),
                                            kvp("type", "collection"),
                                            kvp("options", c.options.view()),
                                            kvp("info", make_document(kvp("readOnly", false))));
                        if (filter.empty() || matchesDocument(desc.view(), filter))
                            out.push_back(std::move(desc));
                    }
                }
                return wrapCursor(database + QStringLiteral(".$cmd.listCollections"), out);
            }

            if (const auto f = cmd["find"]; f && f.type() == bsoncxx::type::k_string)
                return runFindUnlocked(database, strFromView(f.get_string().value), cmd);

            if (const auto a = cmd["aggregate"]; a && a.type() == bsoncxx::type::k_string)
                return runAggregateUnlocked(database, strFromView(a.get_string().value), cmd);

            if (const auto cnt = cmd["count"]; cnt && cnt.type() == bsoncxx::type::k_string) {
                const QString coll = strFromView(cnt.get_string().value);
                const DocView q = cmd["query"] && cmd["query"].type() == bsoncxx::type::k_document
                    ? cmd["query"].get_document().value
                    : (cmd["filter"] && cmd["filter"].type() == bsoncxx::type::k_document
                           ? cmd["filter"].get_document().value : DocView{});
                const std::int64_t skip = std::max<std::int64_t>(0, toInt64Option(cmd["skip"], 0));
                const std::int64_t limit = std::max<std::int64_t>(0, toInt64Option(cmd["limit"], 0));
                std::int64_t matched = 0;
                if (const CollectionData *c = findCollection(database, coll)) {
                    for (const auto &d : c->docs) {
                        if (q.empty() || matchesDocument(d.view(), q))
                            ++matched;
                    }
                }
                matched = std::max<std::int64_t>(0, matched - skip);
                if (limit > 0)
                    matched = std::min<std::int64_t>(matched, limit);
                return okDoc(make_document(
                    kvp("n", static_cast<std::int32_t>(matched)),
                    kvp("ok", 1.0)).view());
            }

            if (const auto dist = cmd["distinct"]; dist && dist.type() == bsoncxx::type::k_string)
                return runDistinctUnlocked(database, strFromView(dist.get_string().value), cmd);

            if (const auto ins = cmd["insert"]; ins && ins.type() == bsoncxx::type::k_string)
                return runInsertUnlocked(database, strFromView(ins.get_string().value), cmd);

            if (const auto upd = cmd["update"]; upd && upd.type() == bsoncxx::type::k_string)
                return runUpdateUnlocked(database, strFromView(upd.get_string().value), cmd);

            if (const auto del = cmd["delete"]; del && del.type() == bsoncxx::type::k_string)
                return runDeleteUnlocked(database, strFromView(del.get_string().value), cmd);

            if (const auto fam = cmd["findAndModify"] ? cmd["findAndModify"] : cmd["findandmodify"];
                fam && fam.type() == bsoncxx::type::k_string)
                return runFindAndModifyUnlocked(database, strFromView(fam.get_string().value), cmd);

            if (const auto cr = cmd["create"]; cr && cr.type() == bsoncxx::type::k_string) {
                const QString coll = strFromView(cr.get_string().value);
                if (findCollection(database, coll)) {
                    return errResult(
                        QStringLiteral("Collection '%1.%2' already exists").arg(database, coll),
                        48, QStringLiteral("NamespaceExists"));
                }
                IMongoTransport::Result quotaErr;
                CollectionData *created = ensureCollection(database, coll, &quotaErr);
                if (!created)
                    return quotaErr;
                basic::document opts;
                for (const auto &el : cmd) {
                    if (el.key() == "create" || el.key() == "maxTimeMS" || el.key() == "$db")
                        continue;
                    opts.append(kvp(el.key(), el.get_value()));
                }
                created->options = opts.extract();
                return okDoc(make_document(kvp("ok", 1.0)).view());
            }

            if (const auto dr = cmd["drop"]; dr && dr.type() == bsoncxx::type::k_string) {
                const QString coll = strFromView(dr.get_string().value);
                auto dbIt = _dbs.find(database);
                if (dbIt == _dbs.end() || dbIt->second.erase(coll) == 0)
                    return errResult(QStringLiteral("ns not found: %1.%2").arg(database, coll),
                                     26, QStringLiteral("NamespaceNotFound"));
                return okDoc(make_document(
                    kvp("nIndexesWas", 1),
                    kvp("ns", (database + QLatin1Char('.') + coll).toStdString()),
                    kvp("ok", 1.0)).view());
            }

            if (cmd["dropDatabase"]) {
                if (database == QLatin1String("admin") || database == QLatin1String("local")
                    || database == QLatin1String("config")) {
                    return errResult(QStringLiteral("Cannot drop reserved database"),
                                     20, QStringLiteral("IllegalOperation"));
                }
                _dbs.erase(database);
                return okDoc(make_document(
                    kvp("dropped", database.toStdString()),
                    kvp("ok", 1.0)).view());
            }

            if (const auto ren = cmd["renameCollection"]; ren && ren.type() == bsoncxx::type::k_string) {
                if (!cmd["to"] || cmd["to"].type() != bsoncxx::type::k_string)
                    return errResult(QStringLiteral("'to' namespace must be a string"),
                                     2, QStringLiteral("BadValue"));
                const QString fromNs = strFromView(ren.get_string().value);
                const QString toNs = strFromView(cmd["to"].get_string().value);
                const qsizetype d1 = fromNs.indexOf(QLatin1Char('.'));
                const qsizetype d2 = toNs.indexOf(QLatin1Char('.'));
                if (d1 <= 0 || d1 + 1 >= fromNs.size() || d2 <= 0 || d2 + 1 >= toNs.size())
                    return errResult(QStringLiteral("Invalid namespace"), 73,
                                     QStringLiteral("InvalidNamespace"));
                const QString srcDb = fromNs.left(d1), srcColl = fromNs.mid(d1 + 1);
                const QString dstDb = toNs.left(d2), dstColl = toNs.mid(d2 + 1);
                auto isReservedDb = [](const QString &db) {
                    return db == QLatin1String("admin") || db == QLatin1String("local")
                           || db == QLatin1String("config");
                };
                if (srcColl.isEmpty() || dstColl.isEmpty() || isReservedDb(srcDb) || isReservedDb(dstDb))
                    return errResult(QStringLiteral("Invalid namespace"), 73,
                                     QStringLiteral("InvalidNamespace"));
                if (fromNs == toNs)
                    return errResult(QStringLiteral("Can't rename a collection to itself"),
                                     48, QStringLiteral("NamespaceExists"));
                CollectionData *src = findCollection(srcDb, srcColl);
                if (!src)
                    return errResult(QStringLiteral("Source namespace does not exist"),
                                     26, QStringLiteral("NamespaceNotFound"));
                const bool dropTarget = toBoolOption(cmd["dropTarget"], false);
                const bool dstExists = (findCollection(dstDb, dstColl) != nullptr);
                if (dstExists && !dropTarget)
                    return errResult(QStringLiteral("Target namespace exists"),
                                     48, QStringLiteral("NamespaceExists"));
                // Pre-check database and collection quotas BEFORE mutating source or target.
                const auto dstDbIt = _dbs.find(dstDb);
                if (dstDbIt == _dbs.end()) {
                    if (_dbs.size() >= InMemoryDemoTransport::kMaxDatabases) {
                        return errResult(
                            QStringLiteral("Demo playground limit reached (max %1 databases)")
                                .arg(InMemoryDemoTransport::kMaxDatabases),
                            8000, QStringLiteral("AtlasError"));
                    }
                } else if (dstDb != srcDb && !dstExists
                           && dstDbIt->second.size() >= InMemoryDemoTransport::kMaxCollectionsPerDb) {
                    return errResult(
                        QStringLiteral("Demo playground limit reached (max %1 collections per database)")
                            .arg(InMemoryDemoTransport::kMaxCollectionsPerDb),
                        8000, QStringLiteral("AtlasError"));
                }

                CollectionData moved = std::move(*src);
                const std::string newNsStd = toNs.toStdString();
                for (auto &idxDoc : moved.indexes) {
                    basic::document rewritten;
                    bool hadNs = false;
                    for (const auto &el : idxDoc.view()) {
                        if (el.key() == "ns") {
                            rewritten.append(kvp("ns", newNsStd));
                            hadNs = true;
                        } else {
                            rewritten.append(kvp(el.key(), el.get_value()));
                        }
                    }
                    if (!hadNs)
                        rewritten.append(kvp("ns", newNsStd));
                    idxDoc = rewritten.extract();
                }
                _dbs[srcDb].erase(srcColl);
                if (dropTarget && _dbs.count(dstDb))
                    _dbs[dstDb].erase(dstColl);
                IMongoTransport::Result quotaErr;
                CollectionData *dst = ensureCollection(dstDb, dstColl, &quotaErr);
                if (!dst)
                    return quotaErr;
                *dst = std::move(moved);
                return okDoc(make_document(kvp("ok", 1.0)).view());
            }

            if (const auto li = cmd["listIndexes"]; li && li.type() == bsoncxx::type::k_string) {
                const QString coll = strFromView(li.get_string().value);
                const CollectionData *c = findCollection(database, coll);
                if (!c)
                    return errResult(QStringLiteral("ns does not exist: %1.%2").arg(database, coll),
                                     26, QStringLiteral("NamespaceNotFound"));
                return wrapCursor(database + QLatin1Char('.') + coll, c->indexes);
            }

            if (const auto ci = cmd["createIndexes"]; ci && ci.type() == bsoncxx::type::k_string) {
                if (!cmd["indexes"] || cmd["indexes"].type() != bsoncxx::type::k_array)
                    return errResult(QStringLiteral("'indexes' must be an array"),
                                     14, QStringLiteral("TypeMismatch"));
                const QString coll = strFromView(ci.get_string().value);
                const bool existedBefore = (findCollection(database, coll) != nullptr);
                IMongoTransport::Result quotaErr;
                CollectionData *c = ensureCollection(database, coll, &quotaErr);
                if (!c)
                    return quotaErr;
                const std::int32_t before = static_cast<std::int32_t>(c->indexes.size());
                for (const auto &idxEl : cmd["indexes"].get_array().value) {
                    if (idxEl.type() != bsoncxx::type::k_document)
                        continue;
                    const DocView spec = idxEl.get_document().value;
                    if (!spec["key"] || spec["key"].type() != bsoncxx::type::k_document
                        || spec["key"].get_document().value.empty()) {
                        return errResult(QStringLiteral("Index 'key' must be a non-empty document"),
                                         67, QStringLiteral("CannotCreateIndex"));
                    }
                    const QString name = spec["name"] && spec["name"].type() == bsoncxx::type::k_string
                        ? strFromView(spec["name"].get_string().value)
                        : defaultIndexName(spec["key"].get_document().value);
                    bool already = false;
                    for (const auto &existing : c->indexes) {
                        if (existing.view()["name"]
                            && existing.view()["name"].type() == bsoncxx::type::k_string
                            && strFromView(existing.view()["name"].get_string().value) == name) {
                            already = true;
                            break;
                        }
                    }
                    if (already)
                        continue;
                    if (c->indexes.size() >= InMemoryDemoTransport::kMaxIndexesPerCollection) {
                        return errResult(QStringLiteral("Demo playground index limit reached"),
                                         8000, QStringLiteral("AtlasError"));
                    }
                    basic::document built;
                    built.append(kvp("v", 2),
                                 kvp("key", spec["key"].get_document().value),
                                 kvp("name", name.toStdString()),
                                 kvp("ns", (database + QLatin1Char('.') + coll).toStdString()));
                    for (const auto &opt : spec) {
                        if (opt.key() == "key" || opt.key() == "name"
                            || opt.key() == "v" || opt.key() == "ns")
                            continue;
                        built.append(kvp(opt.key(), opt.get_value()));
                    }
                    const DocVal candidateIdx = built.extract();
                    if (toBoolOption(candidateIdx.view()["unique"], false)) {
                        c->indexes.push_back(DocVal(candidateIdx.view()));
                        bool conflict = false;
                        for (std::size_t i = 0; i < c->docs.size(); ++i) {
                            QString dup;
                            if (violatesUniqueIndex(*c, c->docs[i].view(), i, &dup)) {
                                conflict = true;
                                break;
                            }
                        }
                        if (conflict) {
                            c->indexes.pop_back();
                            return errResult(
                                QStringLiteral("E11000 duplicate key error building unique index '%1'").arg(name),
                                11000, QStringLiteral("DuplicateKey"));
                        }
                    } else {
                        c->indexes.push_back(candidateIdx);
                    }
                }
                return okDoc(make_document(
                    kvp("numIndexesBefore", before),
                    kvp("numIndexesAfter", static_cast<std::int32_t>(c->indexes.size())),
                    kvp("createdCollectionAutomatically", !existedBefore),
                    kvp("ok", 1.0)).view());
            }

            if (const auto di = cmd["dropIndexes"]; di && di.type() == bsoncxx::type::k_string) {
                const QString coll = strFromView(di.get_string().value);
                CollectionData *c = findCollection(database, coll);
                if (!c)
                    return errResult(QStringLiteral("ns not found: %1.%2").arg(database, coll),
                                     26, QStringLiteral("NamespaceNotFound"));
                const std::int32_t before = static_cast<std::int32_t>(c->indexes.size());
                const auto idxField = cmd["index"];
                if (!idxField)
                    return errResult(QStringLiteral("'index' parameter required"),
                                     2, QStringLiteral("BadValue"));
                if (idxField.type() == bsoncxx::type::k_string
                    && idxField.get_string().value == "*") {
                    c->indexes.erase(
                        std::remove_if(c->indexes.begin(), c->indexes.end(), [](const DocVal &d) {
                            return !(d.view()["name"]
                                     && d.view()["name"].type() == bsoncxx::type::k_string
                                     && d.view()["name"].get_string().value == "_id_");
                        }),
                        c->indexes.end());
                } else if (idxField.type() == bsoncxx::type::k_string) {
                    const bsoncxx::stdx::string_view target = idxField.get_string().value;
                    if (target == "_id_")
                        return errResult(QStringLiteral("cannot drop _id index"),
                                         72, QStringLiteral("InvalidOptions"));
                    const auto oldSize = c->indexes.size();
                    c->indexes.erase(
                        std::remove_if(c->indexes.begin(), c->indexes.end(), [&](const DocVal &d) {
                            return d.view()["name"]
                                   && d.view()["name"].type() == bsoncxx::type::k_string
                                   && d.view()["name"].get_string().value == target;
                        }),
                        c->indexes.end());
                    if (c->indexes.size() == oldSize)
                        return errResult(QStringLiteral("index not found with name [%1]")
                                             .arg(strFromView(target)),
                                         27, QStringLiteral("IndexNotFound"));
                } else if (idxField.type() == bsoncxx::type::k_document) {
                    const DocView targetKey = idxField.get_document().value;
                    c->indexes.erase(
                        std::remove_if(c->indexes.begin(), c->indexes.end(), [&](const DocVal &d) {
                            if (d.view()["name"]
                                && d.view()["name"].type() == bsoncxx::type::k_string
                                && d.view()["name"].get_string().value == "_id_")
                                return false;
                            return d.view()["key"]
                                   && d.view()["key"].type() == bsoncxx::type::k_document
                                   && compareDocuments(d.view()["key"].get_document().value, targetKey) == 0;
                        }),
                        c->indexes.end());
                }
                return okDoc(make_document(
                    kvp("nIndexesWas", before),
                    kvp("ok", 1.0)).view());
            }

            if (const auto cs = cmd["collStats"]; cs && cs.type() == bsoncxx::type::k_string) {
                const QString coll = strFromView(cs.get_string().value);
                const CollectionData *c = findCollection(database, coll);
                const std::int32_t count = c ? static_cast<std::int32_t>(c->docs.size()) : 0;
                std::int64_t sizeBytes = 0;
                if (c) {
                    for (const auto &d : c->docs)
                        sizeBytes += static_cast<std::int64_t>(d.view().length());
                }
                const std::int32_t nindexes = c ? static_cast<std::int32_t>(c->indexes.size()) : 0;
                basic::document indexSizes;
                if (c) {
                    for (const auto &idx : c->indexes) {
                        if (idx.view()["name"] && idx.view()["name"].type() == bsoncxx::type::k_string)
                            indexSizes.append(kvp(idx.view()["name"].get_string().value, 4096));
                    }
                }
                return okDoc(make_document(
                    kvp("ns", (database + QLatin1Char('.') + coll).toStdString()),
                    kvp("size", sizeBytes),
                    kvp("count", count),
                    kvp("avgObjSize", count > 0 ? (sizeBytes / count) : 0),
                    kvp("storageSize", std::max<std::int64_t>(sizeBytes, 4096)),
                    kvp("capped", c ? toBoolOption(c->options.view()["capped"], false) : false),
                    kvp("nindexes", nindexes),
                    kvp("totalIndexSize", static_cast<std::int64_t>(nindexes) * 4096),
                    kvp("totalSize", std::max<std::int64_t>(sizeBytes, 4096)
                                     + static_cast<std::int64_t>(nindexes) * 4096),
                    kvp("indexSizes", indexSizes.extract()),
                    kvp("ok", 1.0)).view());
            }

            if (cmd["dbStats"]) {
                std::int32_t collections = 0;
                std::int64_t objects = 0;
                std::int64_t dataSize = 0;
                std::int32_t indexes = 0;
                if (const auto dbIt = _dbs.find(database); dbIt != _dbs.end()) {
                    collections = static_cast<std::int32_t>(dbIt->second.size());
                    for (const auto &[_, c] : dbIt->second) {
                        objects += static_cast<std::int64_t>(c.docs.size());
                        indexes += static_cast<std::int32_t>(c.indexes.size());
                        for (const auto &d : c.docs)
                            dataSize += static_cast<std::int64_t>(d.view().length());
                    }
                }
                return okDoc(make_document(
                    kvp("db", database.toStdString()),
                    kvp("collections", collections),
                    kvp("views", 0),
                    kvp("objects", objects),
                    kvp("avgObjSize", objects > 0 ? static_cast<double>(dataSize) / objects : 0.0),
                    kvp("dataSize", dataSize),
                    kvp("storageSize", std::max<std::int64_t>(dataSize, 4096)),
                    kvp("indexes", indexes),
                    kvp("indexSize", static_cast<std::int64_t>(indexes) * 4096),
                    kvp("totalSize", std::max<std::int64_t>(dataSize, 4096)
                                     + static_cast<std::int64_t>(indexes) * 4096),
                    kvp("ok", 1.0)).view());
            }

            const QString firstKey = strFromView(cmd.begin()->key());
            return errResult(
                QStringLiteral("no such command: '%1' in demo transport").arg(firstKey),
                59, QStringLiteral("CommandNotFound"));
        }

        std::mutex _mutex;
        std::map<QString, std::map<QString, CollectionData>> _dbs;
    };

    // =========================================================================
    // InMemoryDemoTransport
    // =========================================================================

    std::shared_ptr<InMemoryDemoStore> InMemoryDemoTransport::sharedStore()
    {
        static auto s_shared = std::make_shared<InMemoryDemoStore>();
        return s_shared;
    }

    void InMemoryDemoTransport::resetSharedStore()
    {
        sharedStore()->reset();
    }

    InMemoryDemoTransport::InMemoryDemoTransport()
        : _store(std::make_shared<InMemoryDemoStore>())
    {
    }

    InMemoryDemoTransport::InMemoryDemoTransport(std::shared_ptr<InMemoryDemoStore> store)
        : _store(store ? std::move(store) : std::make_shared<InMemoryDemoStore>())
    {
    }

    InMemoryDemoTransport::~InMemoryDemoTransport() = default;

    void InMemoryDemoTransport::reset()
    {
        _store->reset();
    }

    QJsonObject InMemoryDemoTransport::dataset()
    {
        const auto seed = InMemoryDemoStore::buildCatalogSeed();
        QJsonObject root;
        for (const auto &[name, coll] : seed) {
            QJsonArray arr;
            for (const auto &doc : coll.docs) {
                const std::string relaxed = bsoncxx::to_json(
                    doc.view(), bsoncxx::ExtendedJsonMode::k_relaxed);
                arr.append(QJsonDocument::fromJson(QByteArray::fromStdString(relaxed)).object());
            }
            root.insert(name, arr);
        }
        return root;
    }

    IMongoTransport::Result InMemoryDemoTransport::connect(const QString &)
    {
        _connected = true;
        Result r;
        r.ok = true;
        r.ejson = QStringLiteral(R"({"ok":1})");
        return r;
    }

    IMongoTransport::Result InMemoryDemoTransport::runCommand(const QString &database,
                                                              const QString &ejsonCommand)
    {
        if (!_connected)
            return errResult(QStringLiteral("not connected"), 6, QStringLiteral("HostUnreachable"));
        if (database.trimmed().isEmpty())
            return errResult(QStringLiteral("database name is required"), 73,
                             QStringLiteral("InvalidNamespace"));
        try {
            const DocVal parsed = bsoncxx::from_json(ejsonCommand.toStdString());
            return _store->runCommand(database, parsed.view());
        } catch (const std::exception &) {
            return errResult(QStringLiteral("unreadable command"), 9,
                             QStringLiteral("FailedToParse"));
        }
    }

    IMongoTransport::Result InMemoryDemoTransport::listDatabases()
    {
        if (!_connected)
            return errResult(QStringLiteral("not connected"), 6, QStringLiteral("HostUnreachable"));
        return _store->listDatabases();
    }

    IMongoTransport::Result InMemoryDemoTransport::listCollections(const QString &database)
    {
        if (!_connected)
            return errResult(QStringLiteral("not connected"), 6, QStringLiteral("HostUnreachable"));
        return _store->listCollections(database);
    }

    IMongoTransport::Result InMemoryDemoTransport::ping()
    {
        if (!_connected)
            return errResult(QStringLiteral("not connected"), 6, QStringLiteral("HostUnreachable"));
        Result r;
        r.ok = true;
        r.ejson = QStringLiteral(R"({"ok":1})");
        return r;
    }

    IMongoTransport::Result InMemoryDemoTransport::killOp(const QString &)
    {
        if (!_connected)
            return errResult(QStringLiteral("not connected"), 6, QStringLiteral("HostUnreachable"));
        return okDoc(make_document(kvp("info", "demo: nothing running"), kvp("ok", 1.0)).view());
    }

    void InMemoryDemoTransport::disconnect()
    {
        _connected = false;
    }
}
