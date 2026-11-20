#include "ragoshell/core/settings/QueryHistoryStore.h"

#include <algorithm>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace RagoShell
{
    namespace
    {
        constexpr int kFormatVersion = 1;

        QJsonObject entryToJson(const QueryHistoryEntry &e)
        {
            QJsonObject o{
                {"script", e.script},
                {"database", e.database},
                {"serverName", e.serverName},
                {"serverUuid", e.serverUuid},
                {"executedAt", e.executedAtMs},
                {"durationMs", e.durationMs},
                {"resultCount", e.resultCount},
                {"ok", e.ok},
            };
            if (!e.error.isEmpty())
                o.insert("error", e.error);
            if (e.favorite)
                o.insert("favorite", true);
            if (!e.name.isEmpty())
                o.insert("name", e.name);
            return o;
        }

        QueryHistoryEntry entryFromJson(const QJsonObject &o)
        {
            QueryHistoryEntry e;
            e.script = o.value("script").toString();
            e.database = o.value("database").toString();
            e.serverName = o.value("serverName").toString();
            e.serverUuid = o.value("serverUuid").toString();
            e.executedAtMs = static_cast<qint64>(o.value("executedAt").toDouble());
            e.durationMs = static_cast<qint64>(o.value("durationMs").toDouble());
            e.resultCount = o.value("resultCount").toInt();
            e.ok = o.value("ok").toBool(true);
            e.error = o.value("error").toString();
            e.favorite = o.value("favorite").toBool();
            e.name = o.value("name").toString();
            return e;
        }
    }

    const QueryHistoryEntry *QueryHistoryStore::find(qint64 id) const
    {
        auto it = std::find_if(_entries.begin(), _entries.end(),
                               [id](const QueryHistoryEntry &e) { return e.id == id; });
        return it == _entries.end() ? nullptr : &*it;
    }

    qint64 QueryHistoryStore::record(QueryHistoryEntry entry)
    {
        if (entry.script.trimmed().isEmpty() || entry.script.size() > kMaxScriptChars)
            return 0;
        if (entry.error.size() > kMaxErrorChars)
            entry.error = entry.error.left(kMaxErrorChars) + QStringLiteral("…");

        auto same = std::find_if(_entries.begin(), _entries.end(), [&entry](const QueryHistoryEntry &e) {
            return e.script == entry.script && e.database == entry.database &&
                   e.serverUuid == entry.serverUuid;
        });
        if (same != _entries.end()) {
            // Keep identity and snippet data; refresh the run stats.
            entry.id = same->id;
            entry.favorite = same->favorite;
            entry.name = same->name;
            _entries.erase(same);
        } else {
            entry.id = _nextId++;
        }
        _entries.insert(_entries.begin(), entry);
        trim();
        return entry.id;
    }

    bool QueryHistoryStore::setFavorite(qint64 id, bool favorite)
    {
        for (auto &e : _entries) {
            if (e.id == id) {
                e.favorite = favorite;
                if (!favorite)
                    trim();
                return true;
            }
        }
        return false;
    }

    bool QueryHistoryStore::rename(qint64 id, const QString &name)
    {
        for (auto &e : _entries) {
            if (e.id == id) {
                e.name = name.trimmed();
                return true;
            }
        }
        return false;
    }

    bool QueryHistoryStore::remove(qint64 id)
    {
        const auto before = _entries.size();
        _entries.erase(std::remove_if(_entries.begin(), _entries.end(),
                                      [id](const QueryHistoryEntry &e) { return e.id == id; }),
                       _entries.end());
        return _entries.size() != before;
    }

    void QueryHistoryStore::clearHistory()
    {
        _entries.erase(std::remove_if(_entries.begin(), _entries.end(),
                                      [](const QueryHistoryEntry &e) { return !e.favorite; }),
                       _entries.end());
    }

    void QueryHistoryStore::trim()
    {
        int plain = 0;
        _entries.erase(std::remove_if(_entries.begin(), _entries.end(),
                                      [&plain](const QueryHistoryEntry &e) {
                                          return !e.favorite && ++plain > kMaxEntries;
                                      }),
                       _entries.end());
    }

    QByteArray QueryHistoryStore::toJson() const
    {
        QJsonArray arr;
        for (const auto &e : _entries)
            arr.append(entryToJson(e));
        const QJsonObject root{{"version", kFormatVersion}, {"entries", arr}};
        return QJsonDocument(root).toJson(QJsonDocument::Indented);
    }

    bool QueryHistoryStore::fromJson(const QByteArray &json, QString *error)
    {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (error)
                *error = parseError.errorString();
            return false;
        }
        std::vector<QueryHistoryEntry> loaded;
        qint64 nextId = 1;
        for (const QJsonValue &v : doc.object().value("entries").toArray()) {
            QueryHistoryEntry e = entryFromJson(v.toObject());
            if (e.script.isEmpty())
                continue;
            e.id = nextId++;
            loaded.push_back(std::move(e));
        }
        _entries = std::move(loaded);
        _nextId = nextId;
        trim();
        return true;
    }

    bool QueryHistoryStore::load(const QString &path, QString *error)
    {
        QFile file(path);
        if (!file.exists()) {
            _entries.clear();
            return true;
        }
        if (!file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = file.errorString();
            return false;
        }
        return fromJson(file.readAll(), error);
    }

    bool QueryHistoryStore::save(const QString &path, QString *error) const
    {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            if (error)
                *error = file.errorString();
            return false;
        }
        file.write(toJson());
        if (!file.commit()) {
            if (error)
                *error = file.errorString();
            return false;
        }
        return true;
    }
}
