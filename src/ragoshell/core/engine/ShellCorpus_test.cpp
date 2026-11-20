// The shell scripts of docs/Shell.md checked against recorded results (shell-corpus/*.json: 107
// scripts and 64 completion prefixes), offline normalization/comparator unit tests, and the corpus
// seed/recorder driver (docs/ShellCorpusRecorder.md).
//
// Live tests connect to $RAGOSHELL_TEST_MONGO (default localhost:27017) with the "samples" database
// and skip cleanly when no server answers (unless seeding or recording was explicitly requested).

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <QCoreApplication>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell/core/engine/ScriptEngine.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/utils/AppPaths.h"

namespace RagoShell
{
namespace
{
    void ensureApp()
    {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char appName[] = "rago_unit_tests";
            static char *argv[] = { appName, nullptr };
            static QCoreApplication app(argc, argv);
        }
    }

    // Parses $RAGOSHELL_TEST_MONGO ("host", "host:port", "[ipv6]", "[ipv6]:port"); empty -> localhost:27017.
    // Rejects URI delimiters, credentials, whitespace, control characters, and out-of-range ports.
    bool parseTestMongoEndpoint(const QString &rawInput, std::string *hostOut, unsigned short *portOut, QString *errorOut = nullptr)
    {
        auto fail = [&](const QString &msg) {
            if (errorOut)
                *errorOut = msg;
            return false;
        };

        const QString s = rawInput.trimmed();
        if (s.isEmpty()) {
            *hostOut = "localhost";
            *portOut = 27017;
            return true;
        }

        for (QChar qc : s) {
            const ushort u = qc.unicode();
            if (u <= 0x20 || u >= 0x7f || qc == QLatin1Char('/') || qc == QLatin1Char('\\')
                || qc == QLatin1Char('?') || qc == QLatin1Char('@') || qc == QLatin1Char('#')) {
                return fail(QStringLiteral("invalid character in test MongoDB endpoint: ") + s);
            }
        }

        QString hostPart;
        QString portPart;
        if (s.startsWith(QLatin1Char('['))) {
            const int close = s.indexOf(QLatin1Char(']'));
            if (close <= 1)
                return fail(QStringLiteral("unterminated or empty IPv6 literal in endpoint: ") + s);
            const QString ipv6 = s.mid(1, close - 1);
            for (QChar qc : ipv6) {
                const char c = qc.toLatin1();
                const bool okChar = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
                                    || (c >= 'A' && c <= 'F') || c == ':';
                if (!okChar)
                    return fail(QStringLiteral("invalid IPv6 character in endpoint: ") + s);
            }
            hostPart = s.left(close + 1);
            if (close + 1 < s.size()) {
                if (s.at(close + 1) != QLatin1Char(':'))
                    return fail(QStringLiteral("expected ':' after IPv6 literal in endpoint: ") + s);
                portPart = s.mid(close + 2);
                if (portPart.isEmpty())
                    return fail(QStringLiteral("missing port after ':' in endpoint: ") + s);
            }
        } else {
            const int colon = s.indexOf(QLatin1Char(':'));
            if (colon == 0)
                return fail(QStringLiteral("missing host before ':' in endpoint: ") + s);
            if (colon > 0) {
                if (s.indexOf(QLatin1Char(':'), colon + 1) >= 0)
                    return fail(QStringLiteral("unbracketed IPv6 or extra ':' in endpoint: ") + s);
                hostPart = s.left(colon);
                portPart = s.mid(colon + 1);
                if (portPart.isEmpty())
                    return fail(QStringLiteral("missing port after ':' in endpoint: ") + s);
            } else {
                hostPart = s;
            }
            for (QChar qc : hostPart) {
                const char c = qc.toLatin1();
                const bool okChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                                    || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
                if (!okChar)
                    return fail(QStringLiteral("invalid hostname character in endpoint: ") + s);
            }
        }

        unsigned short port = 27017;
        if (!portPart.isEmpty()) {
            bool okPort = false;
            const uint parsed = portPart.toUInt(&okPort, 10);
            if (!okPort || parsed < 1 || parsed > 65535)
                return fail(QStringLiteral("port out of range (1..65535) in endpoint: ") + s);
            port = static_cast<unsigned short>(parsed);
        }

        *hostOut = hostPart.toStdString();
        *portOut = port;
        return true;
    }

    bool mongoReachableAt(const std::string &host, unsigned short port)
    {
        try {
            static mongocxx::instance inst{};
            (void)inst;
        } catch (const std::exception &) {
        }
        try {
            const std::string uri = "mongodb://" + host + ":" + std::to_string(port) + "/?serverSelectionTimeoutMS=2000";
            mongocxx::client client((mongocxx::uri(uri)));
            using bsoncxx::builder::basic::kvp;
            client.database("admin").run_command(bsoncxx::builder::basic::make_document(kvp("ping", 1)));
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

    QJsonValue relaxedJson(bsoncxx::document::view doc)
    {
        return QJsonDocument::fromJson(QByteArray::fromStdString(
                   bsoncxx::to_json(doc, bsoncxx::ExtendedJsonMode::k_relaxed))).object();
    }

    //Ordinal canonicalizer for dynamic ObjectId _id values within a single result item.
    // Maps the k-th distinct 24-hex ObjectId (case-folded) to 000000000000000000000001, ...02, etc.,
    // preserving intra-item equality/distinctness isomorphism without depending on import timestamps.
    class ObjectIdCanonicalizer
    {
    public:
        static bool isValidHex24(const std::string &s)
        {
            if (s.size() != 24)
                return false;
            for (unsigned char c : s) {
                const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                if (!hex)
                    return false;
            }
            return true;
        }

        std::string canonicalize(const std::string &hex24)
        {
            std::string lower = hex24;
            for (char &c : lower) {
                if (c >= 'A' && c <= 'F')
                    c = static_cast<char>(c - 'A' + 'a');
            }
            const auto it = _seen.find(lower);
            if (it != _seen.end())
                return it->second;
            const unsigned int next = static_cast<unsigned int>(_seen.size() + 1);
            char buf[25];
            std::snprintf(buf, sizeof(buf), "%024x", next);
            std::string mapped(buf, 24);
            _seen.emplace(std::move(lower), mapped);
            return mapped;
        }

    private:
        std::unordered_map<std::string, std::string> _seen;
    };

    bool hasValidNamespaceDot(const std::string &ns)
    {
        const size_t dot = ns.find('.');
        return dot != std::string::npos && dot > 0 && dot + 1 < ns.size();
    }

    // MongoDB <= 4.2 included "ns": "<db>.<coll>" in listIndexes specs; MongoDB 4.4+ omits it (SERVER-41696).
    bool isLegacyIndexDescriptorWithNs(const QJsonObject &obj)
    {
        if (!obj.contains(QLatin1String("v")) || !obj.contains(QLatin1String("key"))
            || !obj.contains(QLatin1String("name")) || !obj.contains(QLatin1String("ns")))
            return false;
        const QJsonValue v = obj.value(QLatin1String("v"));
        if (!v.isDouble())
            return false;
        const double vd = v.toDouble();
        if (vd < 1.0 || vd != static_cast<double>(static_cast<long long>(vd)))
            return false;
        const QJsonValue key = obj.value(QLatin1String("key"));
        if (!key.isObject() || key.toObject().isEmpty())
            return false;
        const QJsonValue name = obj.value(QLatin1String("name"));
        if (!name.isString() || name.toString().isEmpty())
            return false;
        const QJsonValue ns = obj.value(QLatin1String("ns"));
        return ns.isString() && hasValidNamespaceDot(ns.toString().toStdString());
    }

    QJsonValue normalizeJsonValue(const QJsonValue &val, ObjectIdCanonicalizer &canon);

    QJsonObject normalizeJsonObject(const QJsonObject &input, ObjectIdCanonicalizer &canon)
    {
        QJsonObject src = input;
        if (isLegacyIndexDescriptorWithNs(src))
            src.remove(QStringLiteral("ns"));

        QJsonObject out;
        for (auto it = src.constBegin(); it != src.constEnd(); ++it) {
            const QString &key = it.key();
            const QJsonValue &val = it.value();
            if (key == QLatin1String("_id") && val.isObject()) {
                const QJsonObject sub = val.toObject();
                if (sub.size() == 1 && sub.contains(QLatin1String("$oid"))
                    && sub.value(QLatin1String("$oid")).isString()) {
                    const std::string hex = sub.value(QLatin1String("$oid")).toString().toStdString();
                    if (ObjectIdCanonicalizer::isValidHex24(hex)) {
                        QJsonObject normalizedOid;
                        normalizedOid.insert(QStringLiteral("$oid"), QString::fromStdString(canon.canonicalize(hex)));
                        out.insert(key, normalizedOid);
                        continue;
                    }
                    // Malformed $oid string: leave untouched so strict comparison rejects it.
                    out.insert(key, val);
                    continue;
                }
                if (sub.contains(QLatin1String("$oid"))) {
                    // Extra keys alongside $oid or non-string $oid: leave untouched.
                    out.insert(key, val);
                    continue;
                }
                out.insert(key, normalizeJsonObject(sub, canon));
                continue;
            }
            out.insert(key, normalizeJsonValue(val, canon));
        }
        return out;
    }

    QJsonValue normalizeJsonValue(const QJsonValue &val, ObjectIdCanonicalizer &canon)
    {
        if (val.isObject())
            return normalizeJsonObject(val.toObject(), canon);
        if (val.isArray()) {
            QJsonArray out;
            for (const QJsonValue &elem : val.toArray())
                out.append(normalizeJsonValue(elem, canon));
            return out;
        }
        return val;
    }

    QJsonArray normalizeDocuments(const QJsonArray &docs)
    {
        ObjectIdCanonicalizer canon;
        QJsonArray out;
        for (const QJsonValue &v : docs)
            out.append(normalizeJsonValue(v, canon));
        return out;
    }

    // Non-destructive lexical/syntactic JSON normalizer for `item.output` when `item.type == "custom"`.
    // Preserves JavaScript/BSON field order, number formatting, and string escapes byte-for-byte while:
    //   1. Canonicalizing object member `"_id": {"$oid": "<24-hex>"}` payloads ordinally.
    //   2. Stripping legacy MongoDB <= 4.2 `"ns": "<db>.<coll>"` from valid listIndexes descriptors.
    class CustomOutputJsonNormalizer
    {
    public:
        static std::string normalize(const std::string &src)
        {
            CustomOutputJsonNormalizer scanner(src);
            NodeSummary root;
            scanner.skipWs();
            if (!scanner.parseValue(&root))
                return src;
            scanner.skipWs();
            if (scanner._pos != src.size())
                return src;
            return scanner.applyEdits();
        }

    private:
        enum class Kind { Object, Array, String, Integer, OtherNumber, Bool, Null };

        struct NodeSummary
        {
            Kind kind = Kind::Null;
            std::string strVal;
            long long intVal = 0;
            size_t rawStrContentPos = 0;
            size_t rawStrContentLen = 0;
            size_t memberCount = 0;
            bool singleOidValid = false;
            std::string singleOidHex;
            size_t singleOidPos = 0;
        };

        struct SpanEdit
        {
            size_t pos = 0;
            size_t len = 0;
            std::string replacement;
        };

        explicit CustomOutputJsonNormalizer(const std::string &src) : _src(src) {}

        void skipWs()
        {
            while (_pos < _src.size()) {
                const char c = _src[_pos];
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                    ++_pos;
                else
                    break;
            }
        }

        bool parseStringToken(std::string *decodedOut, size_t *rawContentPosOut, size_t *rawContentLenOut)
        {
            if (_pos >= _src.size() || _src[_pos] != '"')
                return false;
            ++_pos;
            const size_t contentStart = _pos;
            std::string decoded;
            while (_pos < _src.size()) {
                const unsigned char ch = static_cast<unsigned char>(_src[_pos]);
                if (ch < 0x20)
                    return false;
                if (ch == '"') {
                    const size_t contentEnd = _pos;
                    ++_pos;
                    *decodedOut = std::move(decoded);
                    *rawContentPosOut = contentStart;
                    *rawContentLenOut = contentEnd - contentStart;
                    return true;
                }
                if (ch == '\\') {
                    ++_pos;
                    if (_pos >= _src.size())
                        return false;
                    const char esc = _src[_pos++];
                    switch (esc) {
                    case '"': decoded.push_back('"'); break;
                    case '\\': decoded.push_back('\\'); break;
                    case '/': decoded.push_back('/'); break;
                    case 'b': decoded.push_back('\b'); break;
                    case 'f': decoded.push_back('\f'); break;
                    case 'n': decoded.push_back('\n'); break;
                    case 'r': decoded.push_back('\r'); break;
                    case 't': decoded.push_back('\t'); break;
                    case 'u': {
                        if (_pos + 4 > _src.size())
                            return false;
                        unsigned int code = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char hc = _src[_pos + static_cast<size_t>(i)];
                            code <<= 4;
                            if (hc >= '0' && hc <= '9') code |= static_cast<unsigned int>(hc - '0');
                            else if (hc >= 'a' && hc <= 'f') code |= static_cast<unsigned int>(hc - 'a' + 10);
                            else if (hc >= 'A' && hc <= 'F') code |= static_cast<unsigned int>(hc - 'A' + 10);
                            else return false;
                        }
                        _pos += 4;
                        if (code > 0 && code < 0x80)
                            decoded.push_back(static_cast<char>(code));
                        else
                            decoded.push_back('?');
                        break;
                    }
                    default:
                        return false;
                    }
                } else {
                    decoded.push_back(static_cast<char>(ch));
                    ++_pos;
                }
            }
            return false;
        }

        bool parseObject(NodeSummary *out)
        {
            if (_pos >= _src.size() || _src[_pos] != '{')
                return false;
            ++_pos;
            skipWs();

            struct MemberRecord
            {
                size_t keyStart = 0;
                size_t valEnd = 0;
                std::string key;
                NodeSummary val;
            };
            std::vector<MemberRecord> members;

            if (_pos < _src.size() && _src[_pos] == '}') {
                ++_pos;
                out->kind = Kind::Object;
                out->memberCount = 0;
                return true;
            }

            while (true) {
                skipWs();
                MemberRecord m;
                m.keyStart = _pos;
                size_t rawKeyPos = 0, rawKeyLen = 0;
                if (!parseStringToken(&m.key, &rawKeyPos, &rawKeyLen))
                    return false;
                skipWs();
                if (_pos >= _src.size() || _src[_pos] != ':')
                    return false;
                ++_pos;
                skipWs();
                if (!parseValue(&m.val))
                    return false;
                m.valEnd = _pos;

                if (m.key == "_id" && m.val.kind == Kind::Object && m.val.singleOidValid) {
                    _edits.push_back(SpanEdit{
                        m.val.singleOidPos,
                        24,
                        _canon.canonicalize(m.val.singleOidHex),
                    });
                }

                members.push_back(std::move(m));
                skipWs();
                if (_pos >= _src.size())
                    return false;
                if (_src[_pos] == ',') {
                    ++_pos;
                    continue;
                }
                if (_src[_pos] == '}') {
                    ++_pos;
                    break;
                }
                return false;
            }

            out->kind = Kind::Object;
            out->memberCount = members.size();
            if (members.size() == 1 && members[0].key == "$oid"
                && members[0].val.kind == Kind::String
                && members[0].val.rawStrContentLen == 24
                && ObjectIdCanonicalizer::isValidHex24(members[0].val.strVal)) {
                out->singleOidValid = true;
                out->singleOidHex = members[0].val.strVal;
                out->singleOidPos = members[0].val.rawStrContentPos;
            }

            // Check legacy MongoDB <= 4.2 listIndexes descriptor with "ns": "<db>.<coll>"
            bool hasV = false, hasKey = false, hasName = false;
            int nsIndex = -1;
            int nsCount = 0;
            for (size_t i = 0; i < members.size(); ++i) {
                const MemberRecord &m = members[i];
                if (m.key == "v" && m.val.kind == Kind::Integer && m.val.intVal >= 1)
                    hasV = true;
                else if (m.key == "key" && m.val.kind == Kind::Object && m.val.memberCount >= 1)
                    hasKey = true;
                else if (m.key == "name" && m.val.kind == Kind::String && !m.val.strVal.empty())
                    hasName = true;
                else if (m.key == "ns") {
                    ++nsCount;
                    if (m.val.kind == Kind::String && hasValidNamespaceDot(m.val.strVal))
                        nsIndex = static_cast<int>(i);
                }
            }
            if (hasV && hasKey && hasName && nsCount == 1 && nsIndex >= 0 && members.size() >= 4) {
                const size_t k = static_cast<size_t>(nsIndex);
                if (k > 0) {
                    const size_t start = members[k - 1].valEnd;
                    const size_t end = members[k].valEnd;
                    _edits.push_back(SpanEdit{ start, end - start, std::string() });
                } else {
                    const size_t start = members[0].keyStart;
                    const size_t end = members[1].keyStart;
                    _edits.push_back(SpanEdit{ start, end - start, std::string() });
                }
            }
            return true;
        }

        bool parseArray(NodeSummary *out)
        {
            if (_pos >= _src.size() || _src[_pos] != '[')
                return false;
            ++_pos;
            skipWs();
            if (_pos < _src.size() && _src[_pos] == ']') {
                ++_pos;
                out->kind = Kind::Array;
                return true;
            }
            while (true) {
                skipWs();
                NodeSummary elem;
                if (!parseValue(&elem))
                    return false;
                skipWs();
                if (_pos >= _src.size())
                    return false;
                if (_src[_pos] == ',') {
                    ++_pos;
                    continue;
                }
                if (_src[_pos] == ']') {
                    ++_pos;
                    break;
                }
                return false;
            }
            out->kind = Kind::Array;
            return true;
        }

        bool parseNumber(NodeSummary *out)
        {
            const size_t start = _pos;
            if (_pos < _src.size() && _src[_pos] == '-')
                ++_pos;
            if (_pos >= _src.size())
                return false;
            if (_src[_pos] == '0') {
                ++_pos;
            } else if (_src[_pos] >= '1' && _src[_pos] <= '9') {
                while (_pos < _src.size() && _src[_pos] >= '0' && _src[_pos] <= '9')
                    ++_pos;
            } else {
                return false;
            }
            bool isFloat = false;
            if (_pos < _src.size() && _src[_pos] == '.') {
                isFloat = true;
                ++_pos;
                if (_pos >= _src.size() || _src[_pos] < '0' || _src[_pos] > '9')
                    return false;
                while (_pos < _src.size() && _src[_pos] >= '0' && _src[_pos] <= '9')
                    ++_pos;
            }
            if (_pos < _src.size() && (_src[_pos] == 'e' || _src[_pos] == 'E')) {
                isFloat = true;
                ++_pos;
                if (_pos < _src.size() && (_src[_pos] == '+' || _src[_pos] == '-'))
                    ++_pos;
                if (_pos >= _src.size() || _src[_pos] < '0' || _src[_pos] > '9')
                    return false;
                while (_pos < _src.size() && _src[_pos] >= '0' && _src[_pos] <= '9')
                    ++_pos;
            }
            if (!isFloat) {
                const std::string token = _src.substr(start, _pos - start);
                char *endPtr = nullptr;
                const long long parsed = std::strtoll(token.c_str(), &endPtr, 10);
                if (endPtr && *endPtr == '\0') {
                    out->kind = Kind::Integer;
                    out->intVal = parsed;
                    return true;
                }
            }
            out->kind = Kind::OtherNumber;
            return true;
        }

        bool parseLiteral(const char *lit, size_t len, Kind kind, NodeSummary *out)
        {
            if (_pos + len > _src.size() || _src.compare(_pos, len, lit) != 0)
                return false;
            _pos += len;
            out->kind = kind;
            return true;
        }

        bool parseValue(NodeSummary *out)
        {
            if (_pos >= _src.size() || _depth >= 128)
                return false;
            ++_depth;
            bool ok = false;
            switch (_src[_pos]) {
            case '{': ok = parseObject(out); break;
            case '[': ok = parseArray(out); break;
            case '"':
                out->kind = Kind::String;
                ok = parseStringToken(&out->strVal, &out->rawStrContentPos, &out->rawStrContentLen);
                break;
            case 't': ok = parseLiteral("true", 4, Kind::Bool, out); break;
            case 'f': ok = parseLiteral("false", 5, Kind::Bool, out); break;
            case 'n': ok = parseLiteral("null", 4, Kind::Null, out); break;
            default:  ok = parseNumber(out); break;
            }
            --_depth;
            return ok;
        }

        std::string applyEdits() const
        {
            if (_edits.empty())
                return _src;
            std::vector<SpanEdit> sorted = _edits;
            std::sort(sorted.begin(), sorted.end(), [](const SpanEdit &a, const SpanEdit &b) {
                return a.pos < b.pos;
            });
            for (size_t i = 1; i < sorted.size(); ++i) {
                if (sorted[i - 1].pos + sorted[i - 1].len > sorted[i].pos)
                    return _src;
            }
            std::string out = _src;
            for (auto it = sorted.rbegin(); it != sorted.rend(); ++it)
                out.replace(it->pos, it->len, it->replacement);
            return out;
        }

        const std::string &_src;
        size_t _pos = 0;
        int _depth = 0;
        ObjectIdCanonicalizer _canon;
        std::vector<SpanEdit> _edits;
    };

    // What of a statement's result is compared with the recording.
    struct ItemSummary
    {
        std::string type;
        std::string output;
        QJsonArray documents;
        std::string db, collection;
        int limit = 0, skip = 0, batchSize = 0;
        QJsonValue query, fields, pipeline, options;
        std::string aggrCollection;
    };

    struct RunSummary
    {
        bool error = false;
        std::string errorMessage;
        std::vector<ItemSummary> items;
    };

    ItemSummary normalizeItem(ItemSummary item)
    {
        item.documents = normalizeDocuments(item.documents);
        if (item.type == "custom" && !item.output.empty())
            item.output = CustomOutputJsonNormalizer::normalize(item.output);
        if (item.type != "query") {
            item.limit = 0;
            item.skip = 0;
            item.batchSize = 0;
        }
        if (item.pipeline.isUndefined())
            item.pipeline = QJsonValue::Null;
        if (item.options.isUndefined())
            item.options = QJsonValue::Null;
        return item;
    }

    RunSummary normalizeRun(RunSummary run)
    {
        for (ItemSummary &item : run.items)
            item = normalizeItem(std::move(item));
        return run;
    }

    RunSummary summarize(const MongoShellExecResult &res)
    {
        RunSummary run;
        run.error = res.error();
        run.errorMessage = res.errorMessage();
        for (const MongoShellResult &r : res.results()) {
            ItemSummary item;
            item.type = r.type();
            item.output = r.response();
            for (const MongoDocumentPtr &d : r.documents())
                item.documents.append(relaxedJson(d->bsonObj()));
            const MongoQueryInfo q = r.queryInfo();
            item.db = q._info._ns.databaseName();
            item.collection = q._info._ns.collectionName();
            item.limit = q._limit;
            item.skip = q._skip;
            item.batchSize = q._batchSize;
            item.query = relaxedJson(q._query.view());
            item.fields = relaxedJson(q._fields.view());
            if (r.aggrInfo().isValid) {
                item.aggrCollection = r.aggrInfo().collectionName;
                item.pipeline = relaxedJson(r.aggrInfo().pipeline.view());
                item.options = relaxedJson(r.aggrInfo().options.view());
            } else {
                item.pipeline = QJsonValue::Null;
                item.options = QJsonValue::Null;
            }
            run.items.push_back(item);
        }
        return run;
    }

    // "SyntaxError: Unexpected token" -> "SyntaxError": the wording of an engine's messages is its own.
    std::string errorName(const std::string &output)
    {
        return output.substr(0, output.find(':'));
    }

    // ---- the recorded results (shell-corpus/*.json) -------------------------------------------------
    //
    // Golden baseline in RAGO_SHELL_CORPUS_DIR (or $RAGOSHELL_SHELL_CORPUS_DIR). Can be regenerated
    // deterministically into $RAGOSHELL_RECORD_SHELL_CORPUS via dev/shell-corpus/record-corpus.sh.

    QString activeCorpusDir()
    {
        const QString overrideDir = productEnv("SHELL_CORPUS_DIR").trimmed();
        return overrideDir.isEmpty() ? QStringLiteral(RAGO_SHELL_CORPUS_DIR) : overrideDir;
    }

    QString recordCorpusDir()
    {
        return productEnv("RECORD_SHELL_CORPUS").trimmed();
    }

    QString seedScriptPath()
    {
        return productEnv("SEED_SHELL_CORPUS").trimmed();
    }

    QString corpusFileIn(const QString &dir, const char *name)
    {
        return QDir(dir).filePath(QLatin1String(name) + QStringLiteral(".json"));
    }

    QString corpusFile(const char *name)
    {
        return corpusFileIn(activeCorpusDir(), name);
    }

    ItemSummary itemFromJson(const QJsonObject &o)
    {
        ItemSummary i;
        i.type = o.value("type").toString().toStdString();
        i.output = o.value("output").toString().toStdString();
        i.documents = o.value("documents").toArray();
        i.db = o.value("db").toString().toStdString();
        i.collection = o.value("collection").toString().toStdString();
        i.limit = o.value("limit").toInt();
        i.skip = o.value("skip").toInt();
        i.batchSize = o.value("batchSize").toInt();
        i.query = o.value("query");
        i.fields = o.value("fields");
        i.pipeline = o.value("pipeline");
        i.options = o.value("options");
        i.aggrCollection = o.value("aggrCollection").toString().toStdString();
        return i;
    }

    QJsonObject itemToJson(const ItemSummary &raw)
    {
        const ItemSummary i = normalizeItem(raw);
        QJsonObject o;
        o.insert(QStringLiteral("aggrCollection"), QString::fromStdString(i.aggrCollection));
        o.insert(QStringLiteral("batchSize"), i.batchSize);
        o.insert(QStringLiteral("collection"), QString::fromStdString(i.collection));
        o.insert(QStringLiteral("db"), QString::fromStdString(i.db));
        o.insert(QStringLiteral("documents"), i.documents);
        o.insert(QStringLiteral("fields"), i.fields.isUndefined() ? QJsonObject() : i.fields);
        o.insert(QStringLiteral("limit"), i.limit);
        o.insert(QStringLiteral("options"), i.options.isUndefined() ? QJsonValue::Null : i.options);
        o.insert(QStringLiteral("output"), QString::fromStdString(i.output));
        o.insert(QStringLiteral("pipeline"), i.pipeline.isUndefined() ? QJsonValue::Null : i.pipeline);
        o.insert(QStringLiteral("query"), i.query.isUndefined() ? QJsonObject() : i.query);
        o.insert(QStringLiteral("skip"), i.skip);
        o.insert(QStringLiteral("type"), QString::fromStdString(i.type));
        return o;
    }

    RunSummary runFromJson(const QJsonObject &o)
    {
        RunSummary r;
        r.error = o.value("error").toBool();
        r.errorMessage = o.value("errorMessage").toString().toStdString();
        for (const QJsonValue &v : o.value("items").toArray())
            r.items.push_back(itemFromJson(v.toObject()));
        return r;
    }

    QJsonObject runToJson(const RunSummary &r)
    {
        QJsonArray items;
        for (const ItemSummary &item : r.items)
            items.append(itemToJson(item));
        QJsonObject o;
        o.insert(QStringLiteral("error"), r.error);
        o.insert(QStringLiteral("errorMessage"), QString::fromStdString(r.errorMessage));
        o.insert(QStringLiteral("items"), items);
        return o;
    }

    QJsonObject readCorpusAt(const QString &path, bool *ok)
    {
        QFile file(path);
        *ok = file.open(QIODevice::ReadOnly);
        if (!*ok)
            return QJsonObject();
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            *ok = false;
            return QJsonObject();
        }
        return doc.object();
    }

    bool writeCorpusJson(const QString &dir, const char *name, const QJsonObject &root, QString *errorOut)
    {
        if (!QDir().mkpath(dir)) {
            if (errorOut)
                *errorOut = QStringLiteral("failed to create output directory: ") + dir;
            return false;
        }
        const QString path = corpusFileIn(dir, name);
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly)) {
            if (errorOut)
                *errorOut = QStringLiteral("failed to open output file: ") + path + QStringLiteral(" (") + out.errorString() + QStringLiteral(")");
            return false;
        }
        QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
        bytes.replace("\r\n", "\n");
        if (!bytes.endsWith('\n'))
            bytes.append('\n');
        if (out.write(bytes) != bytes.size() || !out.commit()) {
            if (errorOut)
                *errorOut = QStringLiteral("failed to commit output file: ") + path + QStringLiteral(" (") + out.errorString() + QStringLiteral(")");
            return false;
        }
        return true;
    }

    // Empty when the runs agree (modulo ordinal dynamic _id ObjectIds and legacy <= 4.2 index ns), else what differs.
    std::string difference(const RunSummary &recordedRaw, const RunSummary &jsRaw)
    {
        const RunSummary recorded = normalizeRun(recordedRaw);
        const RunSummary js = normalizeRun(jsRaw);
        if (recorded.error != js.error)
            return std::string("error flag: recorded ") + (recorded.error ? "yes (" + recorded.errorMessage + ")" : "no")
                   + ", js " + (js.error ? "yes (" + js.errorMessage + ")" : "no");
        if (recorded.items.size() != js.items.size())
            return "number of results: recorded " + std::to_string(recorded.items.size()) + ", js " + std::to_string(js.items.size());
        for (size_t i = 0; i < recorded.items.size(); ++i) {
            const ItemSummary &a = recorded.items[i];
            const ItemSummary &b = js.items[i];
            const std::string at = "result " + std::to_string(i) + ": ";
            if (a.type != b.type)
                return at + "type recorded '" + a.type + "', js '" + b.type + "'";
            if (a.type == "error") {
                if (errorName(a.output) != errorName(b.output))
                    return at + "error recorded '" + a.output + "', js '" + b.output + "'";
                continue;
            }
            if (a.output != b.output)
                return at + "output\n--- recorded:\n" + a.output + "\n--- js:\n" + b.output;
            if (a.documents != b.documents)
                return at + "documents\n--- recorded:\n" + QJsonDocument(a.documents).toJson().toStdString()
                       + "--- js:\n" + QJsonDocument(b.documents).toJson().toStdString();
            // limit, skip and batch size mean something only for a query: the recording of any other result holds the
            // uninitialized ints of a MongoQueryInfo (0xCDCDCDCD in a Debug build), which differ from build to build.
            const bool paging = a.type == "query";
            if (a.db != b.db || a.collection != b.collection
                || (paging && (a.limit != b.limit || a.skip != b.skip || a.batchSize != b.batchSize)))
                return at + "query info (" + a.db + "." + a.collection + " limit " + std::to_string(a.limit)
                       + " skip " + std::to_string(a.skip) + " batch " + std::to_string(a.batchSize) + ") vs ("
                       + b.db + "." + b.collection + " limit " + std::to_string(b.limit)
                       + " skip " + std::to_string(b.skip) + " batch " + std::to_string(b.batchSize) + ")";
            if (a.query != b.query || a.fields != b.fields)
                return at + "query or fields";
            if (a.aggrCollection != b.aggrCollection || a.pipeline != b.pipeline || a.options != b.options)
                return at + "aggregate info";
        }
        return std::string();
    }

    ItemSummary makeCustomJsonItem(const std::string &compactJson)
    {
        ItemSummary item;
        item.type = "custom";
        item.output = compactJson;
        const QJsonDocument parsed = QJsonDocument::fromJson(QByteArray::fromStdString(compactJson));
        if (parsed.isArray())
            item.documents = parsed.array();
        else if (parsed.isObject())
            item.documents.append(parsed.object());
        item.query = QJsonObject();
        item.fields = QJsonObject();
        item.pipeline = QJsonValue::Null;
        item.options = QJsonValue::Null;
        return item;
    }

    RunSummary singleItemRun(const ItemSummary &item)
    {
        RunSummary r;
        r.items.push_back(item);
        return r;
    }
}

// ---- Always-on offline unit tests for normalization, strictness, and endpoint parsing -----------

TEST(ShellCorpusCompareTest, dynamic_objectid_ids_match_across_documents_and_custom_output)
{
    const RunSummary recorded = singleItemRun(makeCustomJsonItem(
        "[{\"_id\":{\"$oid\":\"6ada7d441bcace55d9a1e147\"},\"name\":\"Marchis Restaurant\",\"score\":13},"
        "{\"_id\":{\"$oid\":\"6ada7d441bcace55d9a1e158\"},\"name\":\"Crystal Room\",\"score\":2}]"));
    const RunSummary fresh = singleItemRun(makeCustomJsonItem(
        "[{\"_id\":{\"$oid\":\"6726DC90A1B2C3D4E5F60718\"},\"name\":\"Marchis Restaurant\",\"score\":13},"
        "{\"_id\":{\"$oid\":\"6726dc90a1b2c3d4e5f60719\"},\"name\":\"Crystal Room\",\"score\":2}]"));

    EXPECT_EQ(difference(recorded, fresh), "");

    // Repeated appearance of the first ObjectId must match repeated appearance on the other side.
    const RunSummary recRepeat = singleItemRun(makeCustomJsonItem(
        "[{\"_id\":{\"$oid\":\"6ada7d441bcace55d9a1e147\"},\"k\":1},"
        "{\"_id\":{\"$oid\":\"6ada7d441bcace55d9a1e147\"},\"k\":2}]"));
    const RunSummary freshRepeat = singleItemRun(makeCustomJsonItem(
        "[{\"_id\":{\"$oid\":\"507f1f77bcf86cd799439011\"},\"k\":1},"
        "{\"_id\":{\"$oid\":\"507f1f77bcf86cd799439011\"},\"k\":2}]"));
    EXPECT_EQ(difference(recRepeat, freshRepeat), "");

    // Normalization is idempotent for deterministic corpus recording.
    const ItemSummary once = normalizeItem(fresh.items[0]);
    const ItemSummary twice = normalizeItem(once);
    EXPECT_EQ(once.output, twice.output);
    EXPECT_EQ(once.documents, twice.documents);
}

TEST(ShellCorpusCompareTest, duplicate_vs_distinct_objectids_are_rejected)
{
    const RunSummary distinctDocs = singleItemRun(makeCustomJsonItem(
        "[{\"_id\":{\"$oid\":\"6ada7d441bcace55d9a1e147\"},\"k\":1},"
        "{\"_id\":{\"$oid\":\"6ada7d441bcace55d9a1e158\"},\"k\":1}]"));
    const RunSummary duplicateDocs = singleItemRun(makeCustomJsonItem(
        "[{\"_id\":{\"$oid\":\"507f1f77bcf86cd799439011\"},\"k\":1},"
        "{\"_id\":{\"$oid\":\"507f1f77bcf86cd799439011\"},\"k\":1}]"));

    EXPECT_NE(difference(distinctDocs, duplicateDocs), "");
}

TEST(ShellCorpusCompareTest, malformed_objectids_are_rejected)
{
    const RunSummary valid = singleItemRun(makeCustomJsonItem(
        "{\"_id\":{\"$oid\":\"6ada7d441bcace55d9a1e147\"},\"name\":\"Marchis\"}"));

    const char *badCases[] = {
        "{\"_id\":{\"$oid\":\"6ada7d441bcace55d9a1e14\"},\"name\":\"Marchis\"}",            // 23 chars
        "{\"_id\":{\"$oid\":\"6ada7d441bcace55d9a1e1470\"},\"name\":\"Marchis\"}",          // 25 chars
        "{\"_id\":{\"$oid\":\"6ada7d441bcace55d9a1e14g\"},\"name\":\"Marchis\"}",           // non-hex 'g'
        "{\"_id\":{\"$oid\":\"6ada7d441bcace55d9a1e147\",\"extra\":1},\"name\":\"Marchis\"}",// extra sibling key
        "{\"_id\":{\"$oid\":123456789012345678901234},\"name\":\"Marchis\"}",               // non-string $oid
        "{\"_id\":\"6ada7d441bcace55d9a1e147\",\"name\":\"Marchis\"}",                      // plain string _id
        "{\"_id\":{\"$oid\":\"\\u0036ada7d441bcace55d9a1e147\"},\"name\":\"Marchis\"}",     // escaped hex in output
    };
    for (const char *bad : badCases) {
        const RunSummary candidate = singleItemRun(makeCustomJsonItem(bad));
        EXPECT_NE(difference(valid, candidate), "") << "should reject: " << bad;
    }
}

TEST(ShellCorpusCompareTest, deterministic_scalar_and_composite_ids_remain_strict)
{
    const RunSummary groupA = singleItemRun(makeCustomJsonItem("[{\"_id\":\"American\",\"n\":6183}]"));
    const RunSummary groupB = singleItemRun(makeCustomJsonItem("[{\"_id\":\"Chinese\",\"n\":6183}]"));
    EXPECT_NE(difference(groupA, groupB), "");
    EXPECT_EQ(difference(groupA, groupA), "");

    const RunSummary intA = singleItemRun(makeCustomJsonItem("{\"_id\":1,\"a\":\"again\"}"));
    const RunSummary intB = singleItemRun(makeCustomJsonItem("{\"_id\":2,\"a\":\"again\"}"));
    EXPECT_NE(difference(intA, intB), "");

    const RunSummary compA = singleItemRun(makeCustomJsonItem("{\"_id\":{\"state\":\"NY\"},\"n\":10}"));
    const RunSummary compB = singleItemRun(makeCustomJsonItem("{\"_id\":{\"state\":\"CA\"},\"n\":10}"));
    EXPECT_NE(difference(compA, compB), "");
}

TEST(ShellCorpusCompareTest, non_id_objectids_and_string_literals_remain_strict)
{
    const RunSummary refA = singleItemRun(makeCustomJsonItem(
        "{\"_id\":1,\"authorId\":{\"$oid\":\"507f1f77bcf86cd799439011\"}}"));
    const RunSummary refB = singleItemRun(makeCustomJsonItem(
        "{\"_id\":1,\"authorId\":{\"$oid\":\"507f1f77bcf86cd799439012\"}}"));
    EXPECT_NE(difference(refA, refB), "");

    const RunSummary strLitA = singleItemRun(makeCustomJsonItem(
        "{\"_id\":{\"$oid\":\"6ada7d441bcace55d9a1e147\"},\"note\":\"{\\\"_id\\\":{\\\"$oid\\\":\\\"507f1f77bcf86cd799439011\\\"}}\"}"));
    const RunSummary strLitB = singleItemRun(makeCustomJsonItem(
        "{\"_id\":{\"$oid\":\"6726dc90a1b2c3d4e5f60718\"},\"note\":\"{\\\"_id\\\":{\\\"$oid\\\":\\\"507f1f77bcf86cd799439099\\\"}}\"}"));
    EXPECT_NE(difference(strLitA, strLitB), "");
}

TEST(ShellCorpusCompareTest, field_values_missing_keys_array_lengths_and_output_key_order_remain_strict)
{
    const RunSummary base = singleItemRun(makeCustomJsonItem(
        "[{\"_id\":{\"$oid\":\"6ada7d441bcace55d9a1e147\"},\"a\":1,\"b\":[10,20]}]"));

    // Value mismatch
    EXPECT_NE(difference(base, singleItemRun(makeCustomJsonItem(
        "[{\"_id\":{\"$oid\":\"507f1f77bcf86cd799439011\"},\"a\":2,\"b\":[10,20]}]"))), "");
    // Missing key
    EXPECT_NE(difference(base, singleItemRun(makeCustomJsonItem(
        "[{\"_id\":{\"$oid\":\"507f1f77bcf86cd799439011\"},\"a\":1}]"))), "");
    // Extra key
    EXPECT_NE(difference(base, singleItemRun(makeCustomJsonItem(
        "[{\"_id\":{\"$oid\":\"507f1f77bcf86cd799439011\"},\"a\":1,\"b\":[10,20],\"c\":3}]"))), "");
    // Array length mismatch
    EXPECT_NE(difference(base, singleItemRun(makeCustomJsonItem(
        "[{\"_id\":{\"$oid\":\"507f1f77bcf86cd799439011\"},\"a\":1,\"b\":[10]}]"))), "");
    // Reordered keys in JS output text must fail even when QJsonObject sorts documents keys
    EXPECT_NE(difference(base, singleItemRun(makeCustomJsonItem(
        "[{\"_id\":{\"$oid\":\"507f1f77bcf86cd799439011\"},\"b\":[10,20],\"a\":1}]"))), "");
}

TEST(ShellCorpusCompareTest, legacy_list_indexes_ns_is_normalized_across_mongo_versions)
{
    const RunSummary mongo40 = singleItemRun(makeCustomJsonItem(
        "[{\"v\":2,\"key\":{\"_id\":1},\"name\":\"_id_\",\"ns\":\"samples.restaurants\"}]"));
    const RunSummary mongo40LeadingNs = singleItemRun(makeCustomJsonItem(
        "[{\"ns\":\"samples.restaurants\",\"v\":2,\"key\":{\"_id\":1},\"name\":\"_id_\"}]"));
    const RunSummary mongo70 = singleItemRun(makeCustomJsonItem(
        "[{\"v\":2,\"key\":{\"_id\":1},\"name\":\"_id_\"}]"));

    EXPECT_EQ(difference(mongo40, mongo70), "");
    EXPECT_EQ(difference(mongo40LeadingNs, mongo70), "");

    // Different index key, name, version, or malformed ns without '.' must still fail.
    EXPECT_NE(difference(mongo40, singleItemRun(makeCustomJsonItem(
        "[{\"v\":2,\"key\":{\"name\":1},\"name\":\"_id_\"}]"))), "");
    EXPECT_NE(difference(mongo40, singleItemRun(makeCustomJsonItem(
        "[{\"v\":2,\"key\":{\"_id\":1},\"name\":\"other_\"}]"))), "");
    EXPECT_NE(difference(mongo70, singleItemRun(makeCustomJsonItem(
        "[{\"v\":2,\"key\":{\"_id\":1},\"name\":\"_id_\",\"ns\":\"nodot\"}]"))), "");
}

TEST(ShellCorpusCompareTest, endpoint_parser_validates_host_and_port)
{
    std::string host;
    unsigned short port = 0;
    QString err;

    ASSERT_TRUE(parseTestMongoEndpoint(QString(), &host, &port, &err)) << err.toStdString();
    EXPECT_EQ(host, "localhost");
    EXPECT_EQ(port, 27017);

    ASSERT_TRUE(parseTestMongoEndpoint(QStringLiteral("127.0.0.1:49152"), &host, &port, &err)) << err.toStdString();
    EXPECT_EQ(host, "127.0.0.1");
    EXPECT_EQ(port, 49152);

    ASSERT_TRUE(parseTestMongoEndpoint(QStringLiteral("[::1]:27018"), &host, &port, &err)) << err.toStdString();
    EXPECT_EQ(host, "[::1]");
    EXPECT_EQ(port, 27018);

    for (const char *bad : {
             ":27017", "127.0.0.1:", "127.0.0.1:0", "127.0.0.1:70000", "127.0.0.1:abc",
             "user:pass@127.0.0.1:27017", "127.0.0.1:27017/samples", "127.0.0.1:27017?tls=true",
             "127.0.0.1 :27017", "[::1]extra", "[]" }) {
        EXPECT_FALSE(parseTestMongoEndpoint(QString::fromUtf8(bad), &host, &port, &err)) << "should reject: " << bad;
    }
}

// ---- Live shell corpus tests and deterministic recorder -----------------------------------------

class ShellCorpusTest : public ::testing::Test
{
protected:
    static void SetUpTestCase()
    {
        ensureApp();
        _endpointValid = parseTestMongoEndpoint(productEnv("TEST_MONGO"), &_host, &_port, &_endpointError);
        _settings.reset(new ConnectionSettings(/*isClone=*/false));
        if (_endpointValid) {
            _settings->setServerHost(_host);
            _settings->setServerPort(_port);
        }
        _settings->setDefaultDatabase("samples");

        const QString seedPath = seedScriptPath();
        if (_endpointValid && !seedPath.isEmpty()) {
            if (!mongoReachableAt(_host, _port)) {
                _seedError = QStringLiteral("cannot seed shell corpus: MongoDB not reachable on %1:%2")
                                 .arg(QString::fromStdString(_host)).arg(_port);
                return;
            }
            QFile seedFile(seedPath);
            if (!seedFile.open(QIODevice::ReadOnly)) {
                _seedError = QStringLiteral("cannot open seed script: ") + seedPath;
                return;
            }
            const std::string seedJs = QString::fromUtf8(seedFile.readAll()).toStdString();
            ScriptEngine seeder(_settings.get(), 120);
            seeder.init("", "samples");
            if (seeder.failedScope()) {
                _seedError = QStringLiteral("seed engine init failed: ") + QString::fromStdString(seeder.lastInitError());
                return;
            }
            const MongoShellExecResult res = seeder.exec(seedJs, "samples");
            if (res.error()) {
                _seedError = QStringLiteral("seed script execution failed: ") + QString::fromStdString(res.errorMessage());
                return;
            }
            for (const MongoShellResult &item : res.results()) {
                if (item.type() == "error") {
                    _seedError = QStringLiteral("seed script returned error item: ") + QString::fromStdString(item.response());
                    return;
                }
            }
        }
    }

    void SetUp() override
    {
        ASSERT_TRUE(_endpointValid) << _endpointError.toStdString();
        ASSERT_TRUE(_seedError.isEmpty()) << _seedError.toStdString();

        if (!mongoReachableAt(_host, _port)) {
            if (!recordCorpusDir().isEmpty() || !seedScriptPath().isEmpty()) {
                FAIL() << "MongoDB not reachable on " << _host << ":" << _port;
            }
            std::cerr << "[ SKIP ] MongoDB not reachable on " << _host << ":" << _port << "\n";
            _skip = true;
        }
    }

    // Runs the scripts in order on one engine in `db` (after `prepare`) and checks every result against the
    // recorded one of `group` (or writes canonicalized JSON to $RAGOSHELL_RECORD_SHELL_CORPUS and round-trip checks it).
    void compare(const char *group, const std::vector<std::string> &scripts, const std::string &db = "samples",
                 const std::string &prepare = std::string())
    {
        ScriptEngine engine(_settings.get(), 15);
        engine.init("", db);
        ASSERT_FALSE(engine.failedScope()) << engine.lastInitError();
        if (!prepare.empty())
            engine.exec(prepare, db);
        std::vector<RunSummary> runs;
        for (const std::string &script : scripts)
            runs.push_back(summarize(engine.exec(script, db)));

        const QString outDir = recordCorpusDir();
        QString targetFile = corpusFile(group);
        if (!outDir.isEmpty()) {
            QJsonArray recordedEntries;
            for (size_t i = 0; i < scripts.size(); ++i) {
                QJsonObject entry;
                entry.insert(QStringLiteral("run"), runToJson(runs[i]));
                entry.insert(QStringLiteral("script"), QString::fromStdString(scripts[i]));
                recordedEntries.append(entry);
            }
            QJsonObject root;
            root.insert(QStringLiteral("database"), QString::fromStdString(db));
            root.insert(QStringLiteral("entries"), recordedEntries);
            root.insert(QStringLiteral("group"), QLatin1String(group));
            QString writeErr;
            ASSERT_TRUE(writeCorpusJson(outDir, group, root, &writeErr)) << writeErr.toStdString();
            targetFile = corpusFileIn(outDir, group);
        }

        bool ok = false;
        const QJsonObject corpus = readCorpusAt(targetFile, &ok);
        ASSERT_TRUE(ok) << "no valid recorded results at " << targetFile.toStdString();
        const QJsonArray entries = corpus.value("entries").toArray();
        ASSERT_EQ(static_cast<size_t>(entries.size()), scripts.size()) << "the script list and " << group << ".json differ";
        for (size_t i = 0; i < scripts.size(); ++i) {
            const QJsonObject entry = entries.at(static_cast<int>(i)).toObject();
            ASSERT_EQ(entry.value("script").toString().toStdString(), scripts[i]) << "entry " << i << " of " << group << ".json";
            const std::string diff = difference(runFromJson(entry.value("run").toObject()), runs[i]);
            EXPECT_TRUE(diff.empty()) << "script: " << scripts[i] << "\n" << diff;
        }
    }

    bool _skip = false;
    static bool _endpointValid;
    static QString _endpointError;
    static QString _seedError;
    static std::string _host;
    static unsigned short _port;
    static std::unique_ptr<ConnectionSettings> _settings;
};

bool ShellCorpusTest::_endpointValid = true;
QString ShellCorpusTest::_endpointError;
QString ShellCorpusTest::_seedError;
std::string ShellCorpusTest::_host = "localhost";
unsigned short ShellCorpusTest::_port = 27017;
std::unique_ptr<ConnectionSettings> ShellCorpusTest::_settings;

TEST_F(ShellCorpusTest, reads_give_the_recorded_results)
{
    if (_skip) return;
    compare("reads", {
        "db.restaurants.find({cuisine:'Italian'}).limit(3).sort({_id:1})",
        "db.restaurants.find({cuisine:'Italian'}, {name:1, _id:0}).sort({name:1}).limit(5)",
        "db.restaurants.find().skip(2).limit(2).sort({_id:1})",
        "db.restaurants.find({}).sort({_id:1}).batchSize(7).limit(20)",
        "db.restaurants.findOne({cuisine:'Italian'}, {name:1, _id:0})",
        "db.restaurants.findOne({_id: ObjectId('000000000000000000000000')})",
        "db.restaurants.count({cuisine:'Italian'})",
        "db.restaurants.countDocuments({})",
        "db.restaurants.find({cuisine:'Italian'}).count()",
        "db.restaurants.find({cuisine:'Italian'}).limit(3).size()",
        "db.restaurants.find({cuisine:'Italian'}).limit(3).count(true)",
        "db.restaurants.find({}).limit(3).sort({_id:1}).toArray()",
        "db.restaurants.find({cuisine:'Italian'}).limit(4).sort({_id:1}).itcount()",
        "db.restaurants.distinct('cuisine', {cuisine: 'Italian'})",
        "db.restaurants.aggregate([{$group:{_id:'$cuisine', n:{$sum:1}}},{$sort:{n:-1,_id:1}},{$limit:3}])",
        "db.zips.aggregate([{$match:{state:'NY'}},{$count:'n'}])",
        "db.zips.aggregate([{$group:{_id:'$state', n:{$sum:1}}},{$sort:{_id:1}}], {allowDiskUse: true})",
        "db.restaurants.getIndexes()",
        "db.getCollectionNames()",
        "db.nosuchcollection.find().toArray()",
        "db.nosuchcollection.count()",
        "db.restaurants.find({cuisine:'Italian'}).limit(3).sort({_id:1}).forEach(function (d) { print(d.cuisine) })",
        "db.restaurants.find({}).sort({_id:1}).limit(2).map(function (d) { return d.cuisine })",
        "db.runCommand({ping: 1})",
        "db.getCollection('restaurants').find({}).sort({_id:1}).limit(1)",
        "db['restaurants'].find({}).sort({_id:1}).limit(1)",
        "db.getSiblingDB('admin').getName()",
        "db.restaurants.getName() + ' ' + db.restaurants.getFullName()",
    });
}

TEST_F(ShellCorpusTest, scripting_and_output_give_the_recorded_results)
{
    if (_skip) return;
    compare("scripting", {
        "var a = 5; var b = a * 2; b",
        "var a = 5;\nvar b = a * 2;\nb",
        "function f(n) { return n * 2 }\nf(21)",
        "class A { m() { return 7 } }\nnew A().m()",
        "[1,2,3].map(x => x * 2)",
        "({a: 1})",
        "'text'",
        "42",
        "null",
        "undefined",
        "true",
        "[]",
        "[{a: 1}, {a: 2}]",
        "[1, {a: 1}]",
        "db.restaurants.find({cuisine:'Italian'}).limit(1).sort({_id:1})\ndb.restaurants.count()",
        "use samples",
        "show users",
        "var d = ISODate('2021-05-06T07:08:09.123Z'); [d.getUTCFullYear(), d.toISOString()]",
        "JSON.stringify({a: [1, 2]})",
        "Math.max(1, 5, 3)",
        "EJSON.stringify({d: new Date(0), n: NumberLong(5)})",
        "x = 5",
        "typeof db",
        "typeof ObjectId",
        "ObjectId('507f1f77bcf86cd799439011').toString()",
        "ObjectId('507f1f77bcf86cd799439011').getTimestamp().toISOString()",
        "var s = 0; for (var i = 0; i < 1000; i++) { s += i } s",
        "// only a comment",
        "",
        "   ",
        ";",
    });
}

TEST_F(ShellCorpusTest, errors_give_the_recorded_results)
{
    if (_skip) return;
    compare("errors", {
        "syntax error here (",
        "undefinedFunction()",
        "throw new Error('boom')",
        "null.x",
        "db.restaurants.find({$badop: 1}).toArray()",
        "db.restaurants.find({$badop: 1})",
        "db.runCommand({nosuchcommand: 1})",
        "db.restaurants.aggregate([{$nosuchstage: {}}])",
        "rs.status()",
        "db.restaurants.updateOne({}, {a: 1})",
        "db.restaurants.replaceOne({}, {$set: {a: 1}})",
        "var a = 1;\nvar b = a.nope.deeper;",
    });
}

TEST_F(ShellCorpusTest, writes_give_the_recorded_results)
{
    if (_skip) return;
    compare("writes", {
        "db.t.insertOne({_id: 1, a: 1})",
        "db.t.insertMany([{_id: 2, a: 2}, {_id: 3, a: 3}, {_id: 4, a: 4}])",
        "db.t.updateOne({_id: 1}, {$set: {b: 2}})",
        "db.t.updateMany({a: {$gte: 2}}, {$inc: {a: 10}})",
        "db.t.update({_id: 4}, {$set: {z: 1}}, {upsert: true})",
        "db.t.update({a: {$gte: 12}}, {$set: {m: 1}}, {multi: true})",
        "db.t.updateOne({_id: 99}, {$set: {a: 1}}, {upsert: true})",
        "db.t.replaceOne({_id: 2}, {a: 'replaced'})",
        "db.t.findOneAndUpdate({_id: 1}, {$set: {c: 1}}, {returnDocument: 'after'})",
        "db.t.findOneAndUpdate({_id: 1}, {$set: {c: 2}}, {returnNewDocument: true})",
        "db.t.findOneAndReplace({_id: 1}, {a: 'again'})",
        "db.t.findAndModify({query: {_id: 1}, update: {$set: {fm: 1}}, new: true})",
        "db.t.findOneAndDelete({_id: 3})",
        "db.t.deleteOne({_id: 4})",
        "db.t.remove({_id: 99}, true)",
        "db.t.deleteMany({a: 'replaced'})",
        "db.t.createIndex({a: 1})",
        "db.t.createIndex({a: 1, b: -1}, {name: 'custom', sparse: true})",
        "db.t.getIndexKeys()",
        "db.t.dropIndex('a_1')",
        "db.t.createIndexes([{k1: 1}, {k2: -1}])",
        "db.t.dropIndexes()",
        "db.t.bulkWrite([{insertOne: {document: {_id: 50}}}, {updateOne: {filter: {_id: 50}, update: {$set: {q: 1}}}}, {deleteOne: {filter: {_id: 50}}}])",
        "db.t.bulkWrite([{insertOne: {document: {_id: 60}}}, {insertOne: {document: {_id: 60}}}])",
        "db.t.insertOne({_id: 1})",
        "db.t.save({_id: 7, s: 1}); db.t.save({_id: 7, s: 2}); db.t.findOne({_id: 7})",
        "db.t.find().sort({_id:1}).toArray()",
        "db.t.renameCollection('t2')",
        "db.t2.countDocuments({})",
        "db.t2.drop()",
        "db.t2.drop()",
        "db.createCollection('c2')",
        "db.c2.drop()",
        "db.createView('v1', 'c3', [{$match: {}}])",
        "db.v1.drop()",
        "db.dropDatabase()",
    }, "rago_parity", "db.dropDatabase()");
}

TEST_F(ShellCorpusTest, completions_give_the_recorded_lists)
{
    if (_skip) return;
    ScriptEngine engine(_settings.get(), 15);
    const std::string db = "rago_corpus_c";
    engine.init("", db);
    ASSERT_FALSE(engine.failedScope()) << engine.lastInitError();

    // A database with a plain name, one that needs db.getCollection(), and a quote in a name.
    engine.exec("db.dropDatabase()", db);
    engine.exec("db.plain.insertOne({}); db.getCollection('my-coll').insertOne({}); db.getCollection(\"it's\").insertOne({})", db);

    const std::vector<std::string> prefixes = {
        "", "d", "db", "db.", "db.p", "db.my", "db.it", "db.get", "db.getCo", "db.stat", "db.s", "db.plain", "db.plain.",
        "db.plain.fi", "db.plain.zzz", "db.getCollection('my-coll').", "db.getCollection(\"plain\").ag",
        "db.getCollection( 'plain' ).", "db.nosuch.", "$", "$ma", "$gt", "$zzz", "$dateT", "rs.", "rs.st", "rs.x", "sh",
        "s", "show ", "show d", "use ", "u", "O", "ObjectI", "Number", "NumberL", "ISO", "Timest", "UUID", "BinD", "pri",
        "printj", "fin", "find", "agg", "drop", "e", "x", "ISODate(", "ObjectId(", "db.plain.find({})",
        "db.plain.find({}).", "db.plain.find({}).so", "db.getCollection('my-coll').find({a: 1}).limit(5).",
        "db.plain.aggregate([]).", "db.plain.explain().", "db.system.", "db.getMongo().", "sh.", "EJSON.",
        "db.plain.aggregate([{ $", "db.plain.updateOne({}, { $", "db.plain.find({ a: { $", "12.", "// db.",
    };

    const QString outDir = recordCorpusDir();
    QString targetFile = corpusFile("completions");
    if (!outDir.isEmpty()) {
        QJsonArray recordedEntries;
        for (const std::string &prefix : prefixes) {
            const QStringList got = engine.complete(prefix, AutocompleteAll);
            QJsonArray itemsJson;
            for (const QString &s : got)
                itemsJson.append(s);
            QJsonObject entry;
            entry.insert(QStringLiteral("items"), itemsJson);
            entry.insert(QStringLiteral("prefix"), QString::fromStdString(prefix));
            recordedEntries.append(entry);
        }
        QJsonObject root;
        root.insert(QStringLiteral("database"), QString::fromStdString(db));
        root.insert(QStringLiteral("entries"), recordedEntries);
        root.insert(QStringLiteral("group"), QStringLiteral("completions"));
        QString writeErr;
        ASSERT_TRUE(writeCorpusJson(outDir, "completions", root, &writeErr)) << writeErr.toStdString();
        targetFile = corpusFileIn(outDir, "completions");
    }

    bool ok = false;
    const QJsonObject corpus = readCorpusAt(targetFile, &ok);
    ASSERT_TRUE(ok) << "no valid recorded results at " << targetFile.toStdString();
    const QJsonArray entries = corpus.value("entries").toArray();
    ASSERT_EQ(static_cast<size_t>(entries.size()), prefixes.size());
    for (size_t i = 0; i < prefixes.size(); ++i) {
        const QJsonObject entry = entries.at(static_cast<int>(i)).toObject();
        ASSERT_EQ(entry.value("prefix").toString().toStdString(), prefixes[i]);
        QStringList expected;
        for (const QJsonValue &v : entry.value("items").toArray())
            expected << v.toString();
        // The mode of ScriptEngine::complete() is not used: one list per prefix.
        const QStringList got = engine.complete(prefixes[i], AutocompleteAll);
        EXPECT_EQ(expected, got) << "prefix '" << prefixes[i] << "'\nrecorded: " << expected.join(" | ").toStdString()
                                 << "\njs:       " << got.join(" | ").toStdString();
    }
    // The lists are not empty where they must not be, and modes are respected.
    EXPECT_TRUE(engine.complete("db.", AutocompleteAll).contains("db.getCollection('my-coll')"));
    EXPECT_FALSE(engine.complete("db.", AutocompleteNoCollectionNames).contains("db.plain"));
    EXPECT_TRUE(engine.complete("db.", AutocompleteNoCollectionNames).contains("db.stats()"));
    EXPECT_TRUE(engine.complete("db.", AutocompleteNone).isEmpty());
    EXPECT_TRUE(engine.complete("db.plain.find({}).", AutocompleteAll).contains("db.plain.find({}).sort({})"));
    EXPECT_TRUE(engine.complete("$ma", AutocompleteAll).contains("$match"));

    engine.exec("db.dropDatabase()", db);
}

// Measurements for docs/Shell.md, not a check: run it by hand with
//   RAGO_MEASURE_TABS=3 rago_unit_tests --gtest_also_run_disabled_tests
//       --gtest_filter=ShellCorpusTest.DISABLED_measure_first_result_and_memory
// It prints the time to connect and to the first results of every "shell tab" (one ScriptEngine each) and
// the memory of this process.
TEST_F(ShellCorpusTest, DISABLED_measure_first_result_and_memory)
{
    if (_skip) return;
    const int tabs = qMax(1, qEnvironmentVariableIntValue("RAGO_MEASURE_TABS"));

    auto workingSetMb = []() {
#ifdef Q_OS_WIN
        PROCESS_MEMORY_COUNTERS_EX counters{};
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters));
        return static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
#else
        return 0.0;
#endif
    };

    std::cout << "MEASURE tabs=" << tabs
              << " pid=" << QCoreApplication::applicationPid() << " private_mb_before=" << workingSetMb() << std::endl;
    std::vector<std::unique_ptr<ScriptEngine>> engines;
    for (int i = 0; i < tabs; ++i) {
        auto engine = std::make_unique<ScriptEngine>(_settings.get(), 15);
        QElapsedTimer timer;
        timer.start();
        engine->init("", "samples");
        const qint64 initMs = timer.elapsed();
        timer.restart();
        const MongoShellExecResult first = engine->exec("db.restaurants.find({cuisine: 'Italian'}).limit(20)", "samples");
        const qint64 firstMs = timer.elapsed();
        timer.restart();
        engine->exec("db.restaurants.find({cuisine: 'Italian'}).limit(20)", "samples");
        const qint64 secondMs = timer.elapsed();
        std::cout << "MEASURE tab " << i << ": connect " << initMs << " ms, first script " << firstMs
                  << " ms, second script " << secondMs << " ms (" << (first.error() ? "error" : "ok") << ")" << std::endl;
        engines.push_back(std::move(engine));
    }
    std::cout << "MEASURE private_mb_after=" << workingSetMb() << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(6));
}

}
