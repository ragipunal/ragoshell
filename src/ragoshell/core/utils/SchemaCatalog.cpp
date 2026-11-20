#include "ragoshell/core/utils/SchemaCatalog.h"

#include <algorithm>
#include <cmath>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QSaveFile>
#include <QUrl>

namespace RagoShell
{
    namespace
    {
        constexpr int kFormatVersion = 1;

        SchemaCatalog *g_instance = nullptr;

        // Database names may hold characters that are not valid in file names.
        QString fileSafe(const QString &name)
        {
            return QString::fromLatin1(QUrl::toPercentEncoding(name));
        }

        QString cacheKey(const QString &connectionId, const QString &database)
        {
            return connectionId + QLatin1Char('\n') + database;
        }

        QJsonArray countsToJson(const std::vector<std::pair<QString, qint64>> &counts)
        {
            QJsonArray a;
            for (const auto &[name, n] : counts)
                a.append(QJsonArray{name, static_cast<double>(n)});
            return a;
        }

        std::vector<std::pair<QString, qint64>> countsFromJson(const QJsonValue &value)
        {
            std::vector<std::pair<QString, qint64>> counts;
            for (const QJsonValue &v : value.toArray()) {
                const QJsonArray pair = v.toArray();
                if (pair.size() == 2)
                    counts.emplace_back(pair[0].toString(), static_cast<qint64>(pair[1].toDouble()));
            }
            return counts;
        }

        QJsonObject fieldToJson(const SchemaField &f)
        {
            QJsonObject o{{"path", f.path},
                          {"depth", f.depth},
                          {"presentCount", static_cast<double>(f.presentCount)},
                          {"presence", f.presence},
                          {"types", countsToJson(f.types)},
                          {"distinctValues", f.distinctValues}};
            if (!f.itemTypes.empty())
                o["itemTypes"] = countsToJson(f.itemTypes);
            if (!f.topValues.empty())
                o["topValues"] = countsToJson(f.topValues);
            if (f.valuesTruncated)
                o["valuesTruncated"] = true;
            if (!f.minValue.isEmpty())
                o["min"] = f.minValue;
            if (!f.maxValue.isEmpty())
                o["max"] = f.maxValue;
            if (f.categorical)
                o["categorical"] = true;
            if (!f.letterCase.isEmpty())
                o["letterCase"] = f.letterCase;
            if (f.onlyRecent)
                o["onlyRecent"] = true;
            return o;
        }

        SchemaField fieldFromJson(const QJsonObject &o)
        {
            SchemaField f;
            f.path = o.value("path").toString();
            f.name = f.path.section(QLatin1Char('.'), -1);
            f.depth = o.value("depth").toInt();
            f.presentCount = static_cast<qint64>(o.value("presentCount").toDouble());
            f.presence = o.value("presence").toDouble();
            f.types = countsFromJson(o.value("types"));
            f.itemTypes = countsFromJson(o.value("itemTypes"));
            f.topValues = countsFromJson(o.value("topValues"));
            f.valuesTruncated = o.value("valuesTruncated").toBool();
            f.minValue = o.value("min").toString();
            f.maxValue = o.value("max").toString();
            f.distinctValues = o.value("distinctValues").toInt();
            f.categorical = o.value("categorical").toBool();
            f.letterCase = o.value("letterCase").toString();
            f.onlyRecent = o.value("onlyRecent").toBool();
            return f;
        }
    }

    SchemaCatalog::SchemaCatalog(const QString &rootDir) : _root(rootDir)
    {
    }

    SchemaCatalog *SchemaCatalog::instance()
    {
        return g_instance;
    }

    void SchemaCatalog::setInstance(SchemaCatalog *catalog)
    {
        g_instance = catalog;
    }

    QString SchemaCatalog::filePath(const QString &connectionId, const QString &database) const
    {
        return QDir(_root).filePath(fileSafe(connectionId) + QLatin1Char('/') + fileSafe(database)
                                    + QStringLiteral(".json"));
    }

    SchemaCatalog::Collections &SchemaCatalog::load(const QString &connectionId, const QString &database)
    {
        const QString key = cacheKey(connectionId, database);
        auto it = _cache.find(key);
        if (it != _cache.end())
            return it->second;

        Collections collections;
        QFile file(filePath(connectionId, database));
        if (file.open(QIODevice::ReadOnly)) {
            // A damaged or newer file is ignored: the schema is sampled again.
            const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
            if (root.value("version").toInt() == kFormatVersion) {
                const QJsonObject colls = root.value("collections").toObject();
                for (auto c = colls.begin(); c != colls.end(); ++c) {
                    if (auto snapshot = fromJson(c.value().toObject())) {
                        snapshot->database = database;
                        snapshot->collection = c.key();
                        collections[c.key()] = std::move(*snapshot);
                    }
                }
            }
        }
        return _cache.emplace(key, std::move(collections)).first->second;
    }

    bool SchemaCatalog::save(const QString &connectionId, const QString &database, QString *error)
    {
        const Collections &collections = load(connectionId, database);
        const QString path = filePath(connectionId, database);
        if (collections.empty()) {
            QFile::remove(path);
            return true;
        }
        QJsonObject colls;
        for (const auto &[name, snapshot] : collections)
            colls[name] = toJson(snapshot);
        const QJsonObject root{{"version", kFormatVersion}, {"database", database}, {"collections", colls}};

        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            if (error)
                *error = file.errorString();
            return false;
        }
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        if (!file.commit()) {
            if (error)
                *error = file.errorString();
            return false;
        }
        return true;
    }

    std::optional<SchemaSnapshot> SchemaCatalog::find(const QString &connectionId, const QString &database,
                                                      const QString &collection)
    {
        QMutexLocker lock(&_mutex);
        const Collections &collections = load(connectionId, database);
        const auto it = collections.find(collection);
        if (it == collections.end())
            return std::nullopt;
        return it->second;
    }

    bool SchemaCatalog::store(const QString &connectionId, const SchemaSnapshot &snapshot, QString *error)
    {
        QMutexLocker lock(&_mutex);
        SchemaSnapshot kept = snapshot.valuesIncluded ? snapshot : withoutValues(snapshot);
        kept.result.ok = true;
        kept.result.error.clear();
        load(connectionId, snapshot.database)[snapshot.collection] = std::move(kept);
        return save(connectionId, snapshot.database, error);
    }

    bool SchemaCatalog::remove(const QString &connectionId, const QString &database, const QString &collection)
    {
        QMutexLocker lock(&_mutex);
        if (load(connectionId, database).erase(collection) == 0)
            return false;
        return save(connectionId, database, nullptr);
    }

    void SchemaCatalog::dropMemoryCache()
    {
        QMutexLocker lock(&_mutex);
        _cache.clear();
    }

    bool SchemaCatalog::clear(const QString &connectionId)
    {
        QMutexLocker lock(&_mutex);
        for (auto it = _cache.begin(); it != _cache.end();) {
            if (it->first.startsWith(connectionId + QLatin1Char('\n')))
                it = _cache.erase(it);
            else
                ++it;
        }
        QDir dir(QDir(_root).filePath(fileSafe(connectionId)));
        return !dir.exists() || dir.removeRecursively();
    }

    QStringList SchemaCatalog::analyzedCollections(const QString &connectionId, const QString &database)
    {
        QMutexLocker lock(&_mutex);
        QStringList names;
        for (const auto &entry : load(connectionId, database))
            names << entry.first;
        return names;   // std::map: already sorted
    }

    SchemaCatalog::Coverage SchemaCatalog::coverage(const QString &connectionId, const QString &database,
                                                    const QStringList &collections, const QDateTime &now)
    {
        QMutexLocker lock(&_mutex);
        const Collections &known = load(connectionId, database);
        Coverage c;
        c.total = static_cast<int>(collections.size());
        for (const QString &name : collections) {
            const auto it = known.find(name);
            if (it == known.end())
                continue;
            ++c.analyzed;
            if (isStale(it->second, now))
                ++c.stale;
        }
        return c;
    }

    bool SchemaCatalog::isStale(const SchemaSnapshot &snapshot, const QDateTime &now, qint64 currentCount)
    {
        const QDateTime at = snapshot.result.analyzedAt;
        if (!at.isValid() || at.daysTo(now) > kMaxAgeDays)
            return true;
        const qint64 before = snapshot.result.totalDocuments;
        if (currentCount < 0 || before < 0)
            return false;
        // Small collections: a few documents more or less is not a change.
        const double base = std::max<double>(static_cast<double>(before), 100.0);
        return std::abs(static_cast<double>(currentCount - before)) / base > kMaxCountChange;
    }

    QJsonObject SchemaCatalog::toJson(const SchemaSnapshot &s)
    {
        QJsonArray fields;
        for (const SchemaField &f : s.result.fields)
            fields.append(fieldToJson(f));
        return QJsonObject{{"serverVersion", s.serverVersion},
                           {"sampleSize", s.sampleSize},
                           {"valuesIncluded", s.valuesIncluded},
                           {"analyzedAt", s.result.analyzedAt.toUTC().toString(Qt::ISODate)},
                           {"sampledDocuments", static_cast<double>(s.result.sampledDocuments)},
                           {"recentDocuments", static_cast<double>(s.result.recentDocuments)},
                           {"totalDocuments", static_cast<double>(s.result.totalDocuments)},
                           {"fullScan", s.result.fullScan},
                           {"indexes", QJsonArray::fromStringList(s.result.indexes)},
                           {"fields", fields}};
    }

    std::optional<SchemaSnapshot> SchemaCatalog::fromJson(const QJsonObject &o)
    {
        if (!o.value("fields").isArray())
            return std::nullopt;
        SchemaSnapshot s;
        s.serverVersion = o.value("serverVersion").toString();
        s.sampleSize = o.value("sampleSize").toInt();
        s.valuesIncluded = o.value("valuesIncluded").toBool(true);
        s.result.ok = true;
        s.result.analyzedAt = QDateTime::fromString(o.value("analyzedAt").toString(), Qt::ISODate);
        s.result.sampledDocuments = static_cast<qint64>(o.value("sampledDocuments").toDouble());
        s.result.recentDocuments = static_cast<qint64>(o.value("recentDocuments").toDouble());
        s.result.totalDocuments = static_cast<qint64>(o.value("totalDocuments").toDouble(-1));
        s.result.fullScan = o.value("fullScan").toBool();
        for (const QJsonValue &v : o.value("indexes").toArray())
            s.result.indexes << v.toString();
        for (const QJsonValue &v : o.value("fields").toArray())
            s.result.fields.push_back(fieldFromJson(v.toObject()));
        return s;
    }

    SchemaSnapshot SchemaCatalog::withoutValues(SchemaSnapshot snapshot)
    {
        snapshot.valuesIncluded = false;
        for (SchemaField &f : snapshot.result.fields) {
            f.topValues.clear();
            f.minValue.clear();
            f.maxValue.clear();
            f.letterCase.clear();
        }
        return snapshot;
    }
}
