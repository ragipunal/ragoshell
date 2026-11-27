#include "ragoshell/core/utils/SchemaAnalysis.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <set>
#include <string>

#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/options/aggregate.hpp>
#include <mongocxx/options/estimated_document_count.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell/core/mongodb/MongoUriUtils.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/transport/IMongoTransport.h"
#include "ragoshell/core/transport/InMemoryDemoTransport.h"
#include "ragoshell/core/utils/BsonUtils.h"
#include "ragoshell/core/utils/QtUtils.h"

namespace RagoShell
{
    namespace
    {
        constexpr int kMaxValueChars = 60;

        enum RangeKind { NoRange = 0, NumberRange, DateRange };

        // document::element and array::element (privately derived) alike.
        template <typename Element>
        bsoncxx::binary_sub_type binarySubType(const Element &el)
        {
            return el.type() == bsoncxx::type::k_binary ? el.get_binary().sub_type
                                                         : bsoncxx::binary_sub_type::k_binary;
        }

        std::vector<std::pair<QString, qint64>> byCountDesc(const std::map<QString, qint64> &counts,
                                                           size_t limit = SIZE_MAX)
        {
            std::vector<std::pair<QString, qint64>> v(counts.begin(), counts.end());
            std::stable_sort(v.begin(), v.end(),
                             [](const auto &a, const auto &b) { return a.second > b.second; });
            if (v.size() > limit)
                v.resize(limit);
            return v;
        }

        // Parents before children; "a.b" sorts right after "a" (not after "a b").
        bool pathLess(const QString &a, const QString &b)
        {
            const QStringList pa = a.split(QLatin1Char('.'));
            const QStringList pb = b.split(QLatin1Char('.'));
            return std::lexicographical_compare(pa.begin(), pa.end(), pb.begin(), pb.end());
        }

        // Tracked values are shell text: strings are quoted ("\"Bronx\"").
        bool isStringValue(const QString &text)
        {
            return text.startsWith(QLatin1Char('"'));
        }

        // A string field whose few values each repeat: worth listing completely.
        bool isCategorical(const SchemaField &f, const std::map<QString, qint64> &values)
        {
            if (f.types.empty() || f.types.front().first != QLatin1String("String") || f.valuesTruncated)
                return false;
            const qint64 distinct = static_cast<qint64>(values.size());
            if (distinct == 0 || distinct > SchemaAccumulator::kMaxCategoryValues)
                return false;
            qint64 strings = 0;
            for (const auto &[text, n] : values)
                if (isStringValue(text))
                    strings += n;
            return strings >= 2 * distinct;   // values repeat on average
        }

        // "upper" / "lower" when every string value with letters is so (at least three of them).
        QString letterCase(const std::map<QString, qint64> &values)
        {
            int withLetters = 0;
            bool upper = true, lower = true;
            for (const auto &[text, n] : values) {
                if (!isStringValue(text))
                    continue;
                bool letters = false;
                for (const QChar c : text) {
                    if (!c.isLetter())
                        continue;
                    letters = true;
                    upper &= !c.isLower();
                    lower &= !c.isUpper();
                }
                withLetters += letters;
            }
            if (withLetters < 3)
                return {};
            return upper ? QStringLiteral("upper") : lower ? QStringLiteral("lower") : QString();
        }

        std::int64_t cursorIdFromDoc(bsoncxx::document::view cursorObj)
        {
            const auto idEl = cursorObj["id"];
            if (!idEl)
                return 0;
            if (idEl.type() == bsoncxx::type::k_int64)
                return idEl.get_int64().value;
            if (idEl.type() == bsoncxx::type::k_int32)
                return idEl.get_int32().value;
            if (idEl.type() == bsoncxx::type::k_double) {
                const double d = idEl.get_double().value;
                if (std::isfinite(d) && d > -9223372036854775808.0 && d < 9223372036854774784.0)
                    return static_cast<std::int64_t>(std::trunc(d));
            }
            return 0;
        }

        std::vector<bsoncxx::document::value> drainTransportCursor(
            IMongoTransport &transport, const QString &database, const QString &defaultColl,
            bsoncxx::document::view firstReply, std::size_t maxDocs, std::int32_t maxTimeMs)
        {
            namespace basic = bsoncxx::builder::basic;
            using basic::kvp;
            using basic::make_array;
            using basic::make_document;

            std::vector<bsoncxx::document::value> out;
            const auto cursorEl = firstReply["cursor"];
            if (!cursorEl || cursorEl.type() != bsoncxx::type::k_document)
                return out;
            auto cv = cursorEl.get_document().value;
            QString collName = defaultColl;
            if (const auto nsEl = cv["ns"]; nsEl && nsEl.type() == bsoncxx::type::k_string) {
                const QString ns = QString::fromUtf8(
                    nsEl.get_string().value.data(),
                    static_cast<qsizetype>(nsEl.get_string().value.size()));
                const qsizetype dot = ns.indexOf(QLatin1Char('.'));
                if (dot >= 0 && dot + 1 < ns.size())
                    collName = ns.mid(dot + 1);
            }
            auto appendBatch = [&](const char *key) {
                if (const auto b = cv[key]; b && b.type() == bsoncxx::type::k_array) {
                    for (const auto &el : b.get_array().value) {
                        if (out.size() >= maxDocs)
                            break;
                        if (el.type() == bsoncxx::type::k_document)
                            out.emplace_back(el.get_document().value);
                    }
                }
            };
            appendBatch("firstBatch");
            std::int64_t cursorId = cursorIdFromDoc(cv);

            auto killRemote = [&]() noexcept {
                if (cursorId == 0 || collName.isEmpty())
                    return;
                const std::int64_t toKill = cursorId;
                cursorId = 0;
                try {
                    const auto killCmd = make_document(
                        kvp("killCursors", collName.toStdString()),
                        kvp("cursors", make_array(toKill)));
                    transport.runCommand(
                        database,
                        QString::fromStdString(
                            bsoncxx::to_json(killCmd.view(), bsoncxx::ExtendedJsonMode::k_canonical)));
                } catch (...) {
                }
            };

            try {
                while (cursorId != 0 && out.size() < maxDocs && !collName.isEmpty()) {
                    basic::document gm;
                    gm.append(kvp("getMore", cursorId),
                              kvp("collection", collName.toStdString()));
                    if (maxTimeMs > 0)
                        gm.append(kvp("maxTimeMS", maxTimeMs));
                    const auto res = transport.runCommand(
                        database,
                        QString::fromStdString(
                            bsoncxx::to_json(gm.view(), bsoncxx::ExtendedJsonMode::k_canonical)));
                    if (!res.ok) {
                        cursorId = 0;
                        throw std::runtime_error(res.error.toStdString());
                    }
                    const auto gmDoc = bsoncxx::from_json(res.ejson.toStdString());
                    const auto nextCur = gmDoc.view()["cursor"];
                    if (!nextCur || nextCur.type() != bsoncxx::type::k_document) {
                        cursorId = 0;
                        break;
                    }
                    cv = nextCur.get_document().value;
                    cursorId = cursorIdFromDoc(cv);
                    const std::size_t before = out.size();
                    appendBatch("nextBatch");
                    if (out.size() == before && cursorId != 0) {
                        killRemote();
                        break;
                    }
                }
            } catch (...) {
                killRemote();
                throw;
            }
            if (cursorId != 0)
                killRemote();
            return out;
        }
    }

    SchemaAccumulator::SchemaAccumulator(UUIDEncoding uuidEncoding, SupportedTimes timeZone)
        : _uuidEncoding(uuidEncoding), _timeZone(timeZone)
    {
    }

    void SchemaAccumulator::add(bsoncxx::document::view doc, bool recent)
    {
        // The newest documents are not representative (often missing fields that
        // older ones have): they are kept apart and only add fields of their own.
        if (recent)
            walk(doc, QString(), 0, _recentFields, _recentDocuments++);
        else
            walk(doc, QString(), 0, _fields, _documents++);
    }

    void SchemaAccumulator::walk(bsoncxx::document::view doc, const QString &prefix, int depth,
                                 std::map<QString, FieldStats> &fields, qint64 document)
    {
        for (const auto &el : doc) {
            const auto key = el.key();
            const QString name = QString::fromUtf8(key.data(), static_cast<int>(key.size()));
            const QString path = prefix.isEmpty() ? name : prefix + QLatin1Char('.') + name;
            FieldStats &stats = fields[path];
            stats.depth = depth;
            if (stats.lastDocument != document) {
                stats.lastDocument = document;
                ++stats.presentCount;
            }
            record(stats, el);

            if (el.type() == bsoncxx::type::k_document) {
                walk(el.get_document().value, path, depth + 1, fields, document);
            } else if (el.type() == bsoncxx::type::k_array) {
                for (const auto &item : el.get_array().value) {
                    ++stats.itemTypes[QString::fromLatin1(BsonUtils::BSONTypeToString(
                        item.type(), binarySubType(item), _uuidEncoding))];
                    // Fields of documents inside arrays share the array's path.
                    if (item.type() == bsoncxx::type::k_document)
                        walk(item.get_document().value, path, depth + 1, fields, document);
                }
            }
        }
    }

    void SchemaAccumulator::record(FieldStats &stats, const bsoncxx::document::element &el)
    {
        ++stats.types[QString::fromLatin1(
            BsonUtils::BSONTypeToString(el.type(), binarySubType(el), _uuidEncoding))];

        double number = 0;
        RangeKind kind = NoRange;
        bool trackValue = false;
        switch (el.type()) {
        case bsoncxx::type::k_int32:  number = el.get_int32().value;  kind = NumberRange; trackValue = true; break;
        case bsoncxx::type::k_int64:  number = static_cast<double>(el.get_int64().value); kind = NumberRange; trackValue = true; break;
        case bsoncxx::type::k_double: number = el.get_double().value; kind = NumberRange; trackValue = true; break;
        case bsoncxx::type::k_date:
            number = static_cast<double>(el.get_date().to_int64());
            kind = DateRange;
            trackValue = true;
            break;
        case bsoncxx::type::k_string:
        case bsoncxx::type::k_bool:
        case bsoncxx::type::k_decimal128:
        case bsoncxx::type::k_null:
            trackValue = true;   // ObjectIds etc. are unique: not worth counting
            break;
        default:
            break;
        }
        if (!trackValue)
            return;

        // pretty = 1 renders dates as ISODate("…") instead of milliseconds.
        QString text = QtUtils::toQString(
            BsonUtils::jsonString(el, TenGen, false, 1, _uuidEncoding, _timeZone));
        if (text.size() > kMaxValueChars)
            text = text.left(kMaxValueChars - 1) + QStringLiteral("…");

        auto it = stats.values.find(text);
        if (it != stats.values.end())
            ++it->second;
        else if (static_cast<int>(stats.values.size()) < kMaxTrackedValues)
            stats.values.emplace(text, 1);
        else
            stats.valuesTruncated = true;

        if (kind == NoRange)
            return;
        // A field mixing numbers and dates gets no range.
        if (stats.rangeKind != NoRange && stats.rangeKind != kind) {
            stats.rangeConflict = true;
            return;
        }
        stats.rangeKind = kind;
        if (!stats.hasRange || number < stats.minNumber) {
            stats.minNumber = number;
            stats.minText = text;
        }
        if (!stats.hasRange || number > stats.maxNumber) {
            stats.maxNumber = number;
            stats.maxText = text;
        }
        stats.hasRange = true;
    }

    SchemaField SchemaAccumulator::fieldFrom(const QString &path, const FieldStats &stats, qint64 documents)
    {
        SchemaField f;
        f.path = path;
        f.name = path.section(QLatin1Char('.'), -1);
        f.depth = stats.depth;
        f.presentCount = stats.presentCount;
        f.presence = documents > 0 ? static_cast<double>(stats.presentCount) / documents : 0.0;
        f.types = byCountDesc(stats.types);
        f.itemTypes = byCountDesc(stats.itemTypes);
        f.valuesTruncated = stats.valuesTruncated;
        f.distinctValues = static_cast<int>(stats.values.size());
        f.categorical = isCategorical(f, stats.values);
        f.topValues = byCountDesc(stats.values, f.categorical ? kMaxCategoryValues : kTopValues);
        f.letterCase = letterCase(stats.values);
        if (stats.hasRange && !stats.rangeConflict) {
            f.minValue = stats.minText;
            f.maxValue = stats.maxText;
        }
        return f;
    }

    SchemaResult SchemaAccumulator::result() const
    {
        SchemaResult result;
        result.ok = true;
        result.sampledDocuments = _documents + _recentDocuments;
        result.recentDocuments = _recentDocuments;
        // Statistics come from the random documents; without any, from the newest.
        const bool random = _documents > 0;
        for (const auto &[path, stats] : random ? _fields : _recentFields)
            result.fields.push_back(fieldFrom(path, stats, random ? _documents : _recentDocuments));
        if (random) {
            for (const auto &[path, stats] : _recentFields) {
                if (_fields.count(path))
                    continue;
                // Share among the newest documents, where alone the field was seen.
                SchemaField f = fieldFrom(path, stats, _recentDocuments);
                f.onlyRecent = true;
                result.fields.push_back(std::move(f));
            }
        }
        std::sort(result.fields.begin(), result.fields.end(),
                  [](const SchemaField &a, const SchemaField &b) { return pathLess(a.path, b.path); });
        return result;
    }

    SchemaResult SchemaAnalysis::analyzeCollection(IMongoTransport &transport,
                                                   const QString &database, const QString &collection,
                                                   int sampleSize, int maxTimeMs,
                                                   UUIDEncoding uuidEncoding, SupportedTimes timeZone)
    {
        namespace basic = bsoncxx::builder::basic;
        using basic::kvp;
        using basic::make_array;
        using basic::make_document;

        auto runCmd = [&](bsoncxx::document::view cmd) -> bsoncxx::document::value {
            const auto res = transport.runCommand(
                database,
                QString::fromStdString(bsoncxx::to_json(cmd, bsoncxx::ExtendedJsonMode::k_canonical)));
            if (!res.ok)
                throw std::runtime_error(res.error.toStdString());
            return bsoncxx::from_json(res.ejson.toStdString());
        };

        SchemaAccumulator accumulator(uuidEncoding, timeZone);
        qint64 total = estimatedDocumentCount(transport, database, collection, maxTimeMs);
        bool fullScan = false;
        QStringList indexes;
        const std::int32_t clampedTimeMs = std::max<std::int32_t>(1, maxTimeMs);

        try {
            if (total >= 0 && total <= kFullScanLimit) {
                fullScan = true;
                const auto reply = runCmd(make_document(
                    kvp("find", collection.toStdString()),
                    kvp("filter", make_document()),
                    kvp("maxTimeMS", clampedTimeMs)).view());
                for (const auto &doc : drainTransportCursor(
                         transport, database, collection, reply.view(),
                         static_cast<std::size_t>(kFullScanLimit), clampedTimeMs)) {
                    accumulator.add(doc.view());
                }
            } else {
                std::set<std::string> recentIds;
                const int recentWanted = static_cast<int>(sampleSize * kRecentShare);
                if (recentWanted > 0) {
                    const auto reply = runCmd(make_document(
                        kvp("find", collection.toStdString()),
                        kvp("filter", make_document()),
                        kvp("sort", make_document(kvp("_id", -1))),
                        kvp("limit", recentWanted),
                        kvp("maxTimeMS", clampedTimeMs)).view());
                    for (const auto &docVal : drainTransportCursor(
                             transport, database, collection, reply.view(),
                             static_cast<std::size_t>(recentWanted), clampedTimeMs)) {
                        const auto doc = docVal.view();
                        const auto id = doc["_id"];
                        if (!id || id.type() != bsoncxx::type::k_oid)
                            break;
                        recentIds.insert(id.get_oid().value.to_string());
                        accumulator.add(doc, /*recent=*/true);
                    }
                }
                const int wantSample = std::max(1, sampleSize - static_cast<int>(recentIds.size()));
                const auto sampleReply = runCmd(make_document(
                    kvp("aggregate", collection.toStdString()),
                    kvp("pipeline", make_array(
                        make_document(kvp("$sample", make_document(kvp("size", wantSample)))))),
                    kvp("cursor", make_document()),
                    kvp("maxTimeMS", clampedTimeMs)).view());
                for (const auto &docVal : drainTransportCursor(
                         transport, database, collection, sampleReply.view(),
                         static_cast<std::size_t>(wantSample), clampedTimeMs)) {
                    const auto doc = docVal.view();
                    const auto id = doc["_id"];
                    if (id && id.type() == bsoncxx::type::k_oid
                        && recentIds.count(id.get_oid().value.to_string()))
                        continue;
                    accumulator.add(doc);
                }
            }

            try {
                const auto idxReply = runCmd(
                    make_document(kvp("listIndexes", collection.toStdString())).view());
                for (const auto &idxDoc : drainTransportCursor(
                         transport, database, collection, idxReply.view(), 256, clampedTimeMs)) {
                    const auto index = idxDoc.view();
                    const auto name = index["name"];
                    const auto key = index["key"];
                    if (!key || key.type() != bsoncxx::type::k_document
                        || (name && name.type() == bsoncxx::type::k_string
                            && name.get_string().value == "_id_"))
                        continue;
                    indexes << QString::fromStdString(bsoncxx::to_json(
                        key.get_document().value, bsoncxx::ExtendedJsonMode::k_relaxed));
                }
            } catch (const std::exception &) {
            }
        } catch (const std::exception &ex) {
            SchemaResult failed;
            failed.error = QString::fromUtf8(ex.what());
            return failed;
        }

        SchemaResult result = accumulator.result();
        result.totalDocuments = total;
        result.fullScan = fullScan;
        result.indexes = indexes;
        result.analyzedAt = QDateTime::currentDateTimeUtc();
        return result;
    }

    SchemaResult SchemaAnalysis::analyzeCollection(const ConnectionSettings &connection,
                                                   const QString &database, const QString &collection,
                                                   int sampleSize, int maxTimeMs,
                                                   UUIDEncoding uuidEncoding, SupportedTimes timeZone)
    {
        if (connection.hasTransport()) {
#if defined(Q_OS_WASM)
            if (connection.transportKind() == "memory") {
                InMemoryDemoTransport demo(InMemoryDemoTransport::sharedStore());
                demo.connect(QStringLiteral("{}"));
                return analyzeCollection(demo, database, collection, sampleSize, maxTimeMs,
                                         uuidEncoding, timeZone);
            }
#endif
            SchemaResult failed;
            failed.error = QStringLiteral("Schema sampling is not available over gateway channels.");
            return failed;
        }
        SchemaAccumulator accumulator(uuidEncoding, timeZone);
        qint64 total = -1;
        bool fullScan = false;
        QStringList indexes;
        try {
            mongocxx::client client{mongocxx::uri{buildMongoUri(connection)},
                                    buildMongoClientOptions(connection)};
            auto coll = client.database(database.toStdString()).collection(collection.toStdString());
            const std::chrono::milliseconds maxTime(maxTimeMs);

            try {
                mongocxx::options::estimated_document_count countOpts;
                countOpts.max_time(maxTime);
                total = coll.estimated_document_count(countOpts);
            } catch (const std::exception &) {
                // Views have no count metadata: just sample.
            }

            if (total >= 0 && total <= kFullScanLimit) {
                fullScan = true;
                mongocxx::options::find opts;
                opts.max_time(maxTime);
                for (auto &&doc : coll.find({}, opts))
                    accumulator.add(doc);
            } else {
                // The newest documents first (ObjectId _ids grow with time), then
                // a random sample for the rest, without counting a document twice.
                std::set<std::string> recentIds;
                const int recentWanted = static_cast<int>(sampleSize * kRecentShare);
                if (recentWanted > 0) {
                    mongocxx::options::find opts;
                    opts.max_time(maxTime);
                    opts.sort(bsoncxx::builder::basic::make_document(bsoncxx::builder::basic::kvp("_id", -1)));
                    opts.limit(recentWanted);
                    for (auto &&doc : coll.find({}, opts)) {
                        const auto id = doc["_id"];
                        if (!id || id.type() != bsoncxx::type::k_oid)
                            break;   // not time-ordered: the random sample does it all
                        recentIds.insert(id.get_oid().value.to_string());
                        accumulator.add(doc, /*recent=*/true);
                    }
                }
                mongocxx::pipeline pipeline;
                pipeline.sample(std::max(1, sampleSize - static_cast<int>(recentIds.size())));
                mongocxx::options::aggregate opts;
                opts.max_time(maxTime);
                for (auto &&doc : coll.aggregate(pipeline, opts)) {
                    const auto id = doc["_id"];
                    if (id && id.type() == bsoncxx::type::k_oid
                        && recentIds.count(id.get_oid().value.to_string()))
                        continue;
                    accumulator.add(doc);
                }
            }

            try {
                for (auto &&index : coll.list_indexes()) {
                    const auto name = index["name"];
                    const auto key = index["key"];
                    if (!key || key.type() != bsoncxx::type::k_document
                        || (name && name.type() == bsoncxx::type::k_string && name.get_string().value == "_id_"))
                        continue;
                    indexes << QString::fromStdString(bsoncxx::to_json(key.get_document().value,
                                                                       bsoncxx::ExtendedJsonMode::k_relaxed));
                }
            } catch (const std::exception &) {
                // No listIndexes privilege: the schema is still useful.
            }
        } catch (const std::exception &ex) {
            SchemaResult failed;
            failed.error = QString::fromUtf8(ex.what());
            return failed;
        }
        SchemaResult result = accumulator.result();
        result.totalDocuments = total;
        result.fullScan = fullScan;
        result.indexes = indexes;
        result.analyzedAt = QDateTime::currentDateTimeUtc();
        return result;
    }

    qint64 SchemaAnalysis::estimatedDocumentCount(IMongoTransport &transport, const QString &database,
                                                  const QString &collection, int maxTimeMs)
    {
        try {
            namespace basic = bsoncxx::builder::basic;
            const auto cmd = basic::make_document(
                basic::kvp("count", collection.toStdString()),
                basic::kvp("maxTimeMS", std::max<std::int32_t>(1, maxTimeMs)));
            const auto res = transport.runCommand(
                database,
                QString::fromStdString(bsoncxx::to_json(cmd.view(), bsoncxx::ExtendedJsonMode::k_canonical)));
            if (!res.ok)
                return -1;
            const auto doc = bsoncxx::from_json(res.ejson.toStdString());
            const auto n = doc.view()["n"];
            if (!n)
                return -1;
            if (n.type() == bsoncxx::type::k_int32)
                return n.get_int32().value;
            if (n.type() == bsoncxx::type::k_int64)
                return n.get_int64().value;
            if (n.type() == bsoncxx::type::k_double) {
                const double d = n.get_double().value;
                if (!std::isfinite(d) || d < 0.0 || d >= 9223372036854774784.0)
                    return -1;
                return static_cast<qint64>(std::trunc(d));
            }
            return -1;
        } catch (const std::exception &) {
            return -1;
        }
    }

    qint64 SchemaAnalysis::estimatedDocumentCount(const ConnectionSettings &connection, const QString &database,
                                                  const QString &collection, int maxTimeMs)
    {
        if (connection.hasTransport()) {
#if defined(Q_OS_WASM)
            if (connection.transportKind() == "memory") {
                InMemoryDemoTransport demo(InMemoryDemoTransport::sharedStore());
                demo.connect(QStringLiteral("{}"));
                return estimatedDocumentCount(demo, database, collection, maxTimeMs);
            }
#endif
            return -1;
        }
        try {
            mongocxx::client client{mongocxx::uri{buildMongoUri(connection)},
                                    buildMongoClientOptions(connection)};
            mongocxx::options::estimated_document_count opts;
            opts.max_time(std::chrono::milliseconds(maxTimeMs));
            return client.database(database.toStdString()).collection(collection.toStdString())
                .estimated_document_count(opts);
        } catch (const std::exception &) {
            return -1;
        }
    }

    SchemaAnalysis::CollectionList SchemaAnalysis::listCollections(IMongoTransport &transport,
                                                                   const QString &database, int maxTimeMs)
    {
        CollectionList list;
        try {
            namespace basic = bsoncxx::builder::basic;
            const std::int32_t clampedTimeMs = std::max<std::int32_t>(1, maxTimeMs);
            const auto cmd = basic::make_document(
                basic::kvp("listCollections", 1),
                basic::kvp("maxTimeMS", clampedTimeMs));
            const auto res = transport.runCommand(
                database,
                QString::fromStdString(bsoncxx::to_json(cmd.view(), bsoncxx::ExtendedJsonMode::k_canonical)));
            if (!res.ok) {
                list.error = res.error;
                return list;
            }
            const auto doc = bsoncxx::from_json(res.ejson.toStdString());
            for (const auto &infoDoc : drainTransportCursor(
                     transport, database, QStringLiteral("$cmd.listCollections"),
                     doc.view(), 4096, clampedTimeMs)) {
                const auto info = infoDoc.view();
                CollectionEntry entry;
                if (const auto name = info["name"]; name && name.type() == bsoncxx::type::k_string)
                    entry.name = QtUtils::toQString(std::string(name.get_string().value));
                if (const auto type = info["type"]; type && type.type() == bsoncxx::type::k_string)
                    entry.type = QtUtils::toQString(std::string(type.get_string().value));
                if (!entry.name.isEmpty())
                    list.collections.push_back(entry);
            }
            for (CollectionEntry &entry : list.collections) {
                if (entry.type == QLatin1String("view"))
                    continue;
                entry.documents = estimatedDocumentCount(transport, database, entry.name, maxTimeMs);
            }
            list.ok = true;
        } catch (const std::exception &ex) {
            list.error = QString::fromUtf8(ex.what());
        }
        return list;
    }

    SchemaAnalysis::CollectionList SchemaAnalysis::listCollections(const ConnectionSettings &connection,
                                                                   const QString &database, int maxTimeMs)
    {
        if (connection.hasTransport()) {
#if defined(Q_OS_WASM)
            if (connection.transportKind() == "memory") {
                InMemoryDemoTransport demo(InMemoryDemoTransport::sharedStore());
                demo.connect(QStringLiteral("{}"));
                return listCollections(demo, database, maxTimeMs);
            }
#endif
            CollectionList denied;
            denied.error = QStringLiteral("Schema sampling is not available over gateway channels.");
            return denied;
        }
        CollectionList list;
        try {
            mongocxx::client client{mongocxx::uri{buildMongoUri(connection)},
                                    buildMongoClientOptions(connection)};
            auto db = client.database(database.toStdString());
            for (auto &&info : db.list_collections()) {
                CollectionEntry entry;
                if (const auto name = info["name"]; name && name.type() == bsoncxx::type::k_string)
                    entry.name = QtUtils::toQString(std::string(name.get_string().value));
                if (const auto type = info["type"]; type && type.type() == bsoncxx::type::k_string)
                    entry.type = QtUtils::toQString(std::string(type.get_string().value));
                if (!entry.name.isEmpty())
                    list.collections.push_back(entry);
            }
            for (CollectionEntry &entry : list.collections) {
                if (entry.type == QLatin1String("view"))
                    continue;
                try {
                    mongocxx::options::estimated_document_count opts;
                    opts.max_time(std::chrono::milliseconds(maxTimeMs));
                    entry.documents = db.collection(entry.name.toStdString()).estimated_document_count(opts);
                } catch (const std::exception &) {
                }
            }
            list.ok = true;
        } catch (const std::exception &ex) {
            list.error = QString::fromUtf8(ex.what());
        }
        return list;
    }
}
