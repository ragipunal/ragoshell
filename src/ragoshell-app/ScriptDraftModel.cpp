#include "ragoshell-app/ScriptDraftModel.h"

#include <QDateTime>

#include "ragoshell/core/utils/Logger.h"

namespace RagoShell
{
    namespace
    {
        constexpr int kPreviewChars = 200;

        QString preview(const QString &script)
        {
            QString s = script.simplified();
            if (s.size() > kPreviewChars)
                s = s.left(kPreviewChars - 1) + QStringLiteral("…");
            return s;
        }
    }

    ScriptDraftModel::ScriptDraftModel(const QString &filePath, QObject *parent)
        : QAbstractListModel(parent),
          _filePath(filePath)
    {
        QString error;
        if (!_store.load(_filePath, &error))
            LOG_MSG(QStringLiteral("Script drafts not loaded (%1): %2").arg(_filePath, error),
                    LogSeverity::Warning);
        if (_store.pruneOlderThan(QDateTime::currentMSecsSinceEpoch()) > 0)
            saveNow();
        rebuild();
        if (!_visible.empty())
            LOG_MSG(QStringLiteral("%1 unsaved script draft(s) from the last session").arg(_visible.size()),
                    LogSeverity::Info, false);
    }

    int ScriptDraftModel::rowCount(const QModelIndex &parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(_visible.size());
    }

    QVariant ScriptDraftModel::data(const QModelIndex &index, int role) const
    {
        if (!index.isValid() || index.row() >= static_cast<int>(_visible.size()))
            return QVariant();
        const ScriptDraft *d = _store.find(_visible[index.row()]);
        if (!d)
            return QVariant();
        switch (role) {
        case IdRole:         return d->id;
        case TitleRole:      return d->title;
        case ServerNameRole: return d->serverName;
        case ServerUuidRole: return d->serverUuid;
        case DatabaseRole:   return d->database;
        case FilePathRole:   return d->filePath;
        case PreviewRole:    return preview(d->script);
        case ScriptRole:     return d->script;
        case SavedAtRole:
            return QDateTime::fromMSecsSinceEpoch(d->savedAtMs).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        default:             return QVariant();
        }
    }

    QHash<int, QByteArray> ScriptDraftModel::roleNames() const
    {
        return {
            {IdRole, "draftId"},
            {TitleRole, "title"},
            {ServerNameRole, "serverName"},
            {ServerUuidRole, "serverUuid"},
            {DatabaseRole, "database"},
            {FilePathRole, "filePath"},
            {PreviewRole, "preview"},
            {ScriptRole, "script"},
            {SavedAtRole, "savedAt"},
        };
    }

    void ScriptDraftModel::update(ScriptDraft draft)
    {
        if (draft.id.isEmpty())
            return;
        draft.savedAtMs = QDateTime::currentMSecsSinceEpoch();
        const QString id = draft.id;
        _liveIds.insert(id);
        if (!_store.update(std::move(draft)))
            _liveIds.remove(id);          // blank or oversized: nothing is kept
        rebuild();
        saveNow();
    }

    void ScriptDraftModel::remove(const QString &id)
    {
        _liveIds.remove(id);
        if (_store.remove(id)) {
            rebuild();
            saveNow();
        }
    }

    void ScriptDraftModel::claim(const QString &id)
    {
        if (_liveIds.contains(id))
            return;
        _liveIds.insert(id);
        rebuild();
    }

    bool ScriptDraftModel::find(const QString &id, ScriptDraft *out) const
    {
        const ScriptDraft *d = _store.find(id);
        if (!d)
            return false;
        if (out)
            *out = *d;
        return true;
    }

    void ScriptDraftModel::discardAll()
    {
        const std::vector<QString> ids = _visible;
        for (const QString &id : ids)
            _store.remove(id);
        rebuild();
        saveNow();
    }

    bool ScriptDraftModel::takeStartupPrompt()
    {
        const bool show = _startupPrompt && !_visible.empty();
        _startupPrompt = false;
        return show;
    }

    void ScriptDraftModel::rebuild()
    {
        std::vector<QString> visible;
        for (const ScriptDraft &d : _store.entries()) {
            if (!_liveIds.contains(d.id))
                visible.push_back(d.id);
        }
        if (visible == _visible)
            return;
        const int oldCount = static_cast<int>(_visible.size());
        beginResetModel();
        _visible = std::move(visible);
        endResetModel();
        if (oldCount != static_cast<int>(_visible.size()))
            emit countChanged();
    }

    void ScriptDraftModel::saveNow()
    {
        QString error;
        if (!_store.save(_filePath, &error))
            LOG_MSG(QStringLiteral("Cannot save script drafts (%1): %2").arg(_filePath, error),
                    LogSeverity::Error);
    }
}
