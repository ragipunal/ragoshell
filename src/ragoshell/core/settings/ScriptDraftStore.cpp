#include "ragoshell/core/settings/ScriptDraftStore.h"

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

        QJsonObject draftToJson(const ScriptDraft &d)
        {
            return QJsonObject{
                {"id", d.id},
                {"serverUuid", d.serverUuid},
                {"serverName", d.serverName},
                {"database", d.database},
                {"title", d.title},
                {"filePath", d.filePath},
                {"script", d.script},
                {"savedAt", static_cast<double>(d.savedAtMs)},
            };
        }

        ScriptDraft draftFromJson(const QJsonObject &o)
        {
            ScriptDraft d;
            d.id = o.value("id").toString();
            d.serverUuid = o.value("serverUuid").toString();
            d.serverName = o.value("serverName").toString();
            d.database = o.value("database").toString();
            d.title = o.value("title").toString();
            d.filePath = o.value("filePath").toString();
            d.script = o.value("script").toString();
            d.savedAtMs = static_cast<qint64>(o.value("savedAt").toDouble());
            return d;
        }
    }

    const ScriptDraft *ScriptDraftStore::find(const QString &id) const
    {
        auto it = std::find_if(_entries.begin(), _entries.end(),
                               [&id](const ScriptDraft &d) { return d.id == id; });
        return it == _entries.end() ? nullptr : &*it;
    }

    bool ScriptDraftStore::update(ScriptDraft draft)
    {
        if (draft.id.isEmpty())
            return false;
        remove(draft.id);
        if (draft.script.trimmed().isEmpty() || draft.script.size() > kMaxScriptChars)
            return false;

        _entries.insert(_entries.begin(), std::move(draft));
        if (static_cast<int>(_entries.size()) > kMaxDrafts)
            _entries.resize(kMaxDrafts);
        return true;
    }

    bool ScriptDraftStore::remove(const QString &id)
    {
        auto it = std::find_if(_entries.begin(), _entries.end(),
                               [&id](const ScriptDraft &d) { return d.id == id; });
        if (it == _entries.end())
            return false;
        _entries.erase(it);
        return true;
    }

    void ScriptDraftStore::clear()
    {
        _entries.clear();
    }

    int ScriptDraftStore::pruneOlderThan(qint64 nowMs)
    {
        const auto oldEnough = [nowMs](const ScriptDraft &d) { return nowMs - d.savedAtMs > kMaxAgeMs; };
        const auto removed = std::count_if(_entries.begin(), _entries.end(), oldEnough);
        _entries.erase(std::remove_if(_entries.begin(), _entries.end(), oldEnough), _entries.end());
        return static_cast<int>(removed);
    }

    QByteArray ScriptDraftStore::toJson() const
    {
        QJsonArray drafts;
        for (const ScriptDraft &d : _entries)
            drafts.append(draftToJson(d));
        const QJsonObject root{{"version", kFormatVersion}, {"drafts", drafts}};
        return QJsonDocument(root).toJson(QJsonDocument::Indented);
    }

    bool ScriptDraftStore::fromJson(const QByteArray &json, QString *error)
    {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (error)
                *error = parseError.errorString();
            return false;
        }

        // Same rules as update(), so a hand-edited file cannot break them.
        ScriptDraftStore loaded;
        const QJsonArray drafts = doc.object().value("drafts").toArray();
        for (qsizetype i = drafts.size() - 1; i >= 0; --i)   // update() puts each in front
            loaded.update(draftFromJson(drafts.at(i).toObject()));
        // Newest first by time, whatever order the file had.
        std::stable_sort(loaded._entries.begin(), loaded._entries.end(),
                         [](const ScriptDraft &a, const ScriptDraft &b) { return a.savedAtMs > b.savedAtMs; });
        _entries = std::move(loaded._entries);
        return true;
    }

    bool ScriptDraftStore::load(const QString &path, QString *error)
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

    bool ScriptDraftStore::save(const QString &path, QString *error) const
    {
        if (_entries.empty()) {
            if (QFile::exists(path) && !QFile::remove(path)) {
                if (error)
                    *error = QStringLiteral("cannot remove %1").arg(path);
                return false;
            }
            return true;
        }

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
