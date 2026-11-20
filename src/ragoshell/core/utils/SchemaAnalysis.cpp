#include "ragoshell/core/utils/SchemaAnalysis.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <set>
#include <string>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/options/aggregate.hpp>
#include <mongocxx/options/estimated_document_count.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell/core/mongodb/MongoUriUtils.h"
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

    SchemaResult SchemaAnalysis::analyzeCollection(const ConnectionSettings &connection,
                                                   const QString &database, const QString &collection,
                                                   int sampleSize, int maxTimeMs,
                                                   UUIDEncoding uuidEncoding, SupportedTimes timeZone)
    {
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

    qint64 SchemaAnalysis::estimatedDocumentCount(const ConnectionSettings &connection, const QString &database,
                                                  const QString &collection, int maxTimeMs)
    {
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

    SchemaAnalysis::CollectionList SchemaAnalysis::listCollections(const ConnectionSettings &connection,
                                                                   const QString &database, int maxTimeMs)
    {
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
            mongocxx::options::estimated_document_count opts;
            opts.max_time(std::chrono::milliseconds(maxTimeMs));
            for (CollectionEntry &entry : list.collections) {
                if (entry.type == QLatin1String("view"))
                    continue;   // no count metadata
                try {
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
