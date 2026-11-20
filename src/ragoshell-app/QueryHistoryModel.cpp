#include "ragoshell-app/QueryHistoryModel.h"

#include <QDateTime>

#include "ragoshell/core/utils/Logger.h"

namespace RagoShell
{
    namespace
    {
        constexpr int kSaveDelayMs = 1500;
        constexpr int kPreviewChars = 160;

        QString preview(const QString &script)
        {
            QString s = script.simplified();
            if (s.size() > kPreviewChars)
                s = s.left(kPreviewChars - 1) + QStringLiteral("…");
            return s;
        }
    }

    QueryHistoryModel::QueryHistoryModel(const QString &filePath, QObject *parent)
        : QAbstractListModel(parent),
          _filePath(filePath)
    {
        QString error;
        if (!_store.load(_filePath, &error))
            LOG_MSG(QStringLiteral("Query history not loaded (%1): %2").arg(_filePath, error),
                    LogSeverity::Warning);
        rebuild();

        _saveTimer.setSingleShot(true);
        _saveTimer.setInterval(kSaveDelayMs);
        connect(&_saveTimer, &QTimer::timeout, this, &QueryHistoryModel::saveNow);
    }

    QueryHistoryModel::~QueryHistoryModel()
    {
        if (_saveTimer.isActive())
            saveNow();
    }

    int QueryHistoryModel::rowCount(const QModelIndex &parent) const
    {
        return parent.isValid() ? 0 : static_cast<int>(_visible.size());
    }

    QVariant QueryHistoryModel::data(const QModelIndex &index, int role) const
    {
        if (!index.isValid() || index.row() >= static_cast<int>(_visible.size()))
            return QVariant();
        // Defensive: a store rewritten without reloadFromStore() must not index past its end.
        const int storeIndex = _visible[index.row()];
        if (storeIndex < 0 || storeIndex >= static_cast<int>(_store.entries().size()))
            return QVariant();
        const QueryHistoryEntry &e = _store.entries()[storeIndex];
        switch (role) {
        case IdRole:          return e.id;
        case ScriptRole:      return e.script;
        case PreviewRole:     return preview(e.script);
        case DatabaseRole:    return e.database;
        case ServerNameRole:  return e.serverName;
        case ServerUuidRole:  return e.serverUuid;
        case ExecutedAtRole:
            return QDateTime::fromMSecsSinceEpoch(e.executedAtMs).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        case DurationMsRole:  return e.durationMs;
        case ResultCountRole: return e.resultCount;
        case OkRole:          return e.ok;
        case ErrorRole:       return e.error;
        case FavoriteRole:    return e.favorite;
        case NameRole:        return e.name;
        default:              return QVariant();
        }
    }

    QHash<int, QByteArray> QueryHistoryModel::roleNames() const
    {
        return {
            {IdRole, "entryId"},
            {ScriptRole, "script"},
            {PreviewRole, "preview"},
            {DatabaseRole, "database"},
            {ServerNameRole, "serverName"},
            {ServerUuidRole, "serverUuid"},
            {ExecutedAtRole, "executedAt"},
            {DurationMsRole, "durationMs"},
            {ResultCountRole, "resultCount"},
            {OkRole, "ok"},
            {ErrorRole, "error"},
            {FavoriteRole, "favorite"},
            {NameRole, "name"},
        };
    }

    void QueryHistoryModel::setFilterText(const QString &text)
    {
        if (_filterText == text)
            return;
        _filterText = text;
        emit filterChanged();
        rebuild();
    }

    void QueryHistoryModel::setSnippetsOnly(bool on)
    {
        if (_snippetsOnly == on)
            return;
        _snippetsOnly = on;
        emit filterChanged();
        rebuild();
    }

    void QueryHistoryModel::record(const QueryHistoryEntry &entry)
    {
        if (_store.record(entry) == 0)
            return;
        rebuild();
        scheduleSave();
    }

    qint64 QueryHistoryModel::saveSnippet(const QString &script, const QString &name,
                                          const QString &database, const QString &serverUuid,
                                          const QString &serverName)
    {
        QueryHistoryEntry e;
        e.script = script;
        e.database = database;
        e.serverUuid = serverUuid;
        e.serverName = serverName;
        e.executedAtMs = QDateTime::currentMSecsSinceEpoch();
        const qint64 id = _store.record(e);
        if (id == 0)
            return 0;
        _store.setFavorite(id, true);
        if (!name.trimmed().isEmpty())
            _store.rename(id, name);
        rebuild();
        scheduleSave();
        return id;
    }

    void QueryHistoryModel::setFavorite(qint64 id, bool favorite)
    {
        if (!_store.setFavorite(id, favorite))
            return;
        rebuild();
        scheduleSave();
    }

    void QueryHistoryModel::rename(qint64 id, const QString &name)
    {
        if (!_store.rename(id, name))
            return;
        rebuild();
        scheduleSave();
    }

    void QueryHistoryModel::remove(qint64 id)
    {
        if (!_store.remove(id))
            return;
        rebuild();
        scheduleSave();
    }

    void QueryHistoryModel::clearHistory()
    {
        _store.clearHistory();
        rebuild();
        scheduleSave();
    }

    void QueryHistoryModel::reloadFromStore()
    {
        rebuild();
        _saveTimer.stop();
        saveNow();
    }

    void QueryHistoryModel::rebuild()
    {
        const int oldCount = static_cast<int>(_visible.size());
        beginResetModel();
        _visible.clear();
        const QString needle = _filterText.trimmed();
        const auto &entries = _store.entries();
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            const QueryHistoryEntry &e = entries[i];
            if (_snippetsOnly && !e.favorite)
                continue;
            if (!needle.isEmpty() &&
                !e.script.contains(needle, Qt::CaseInsensitive) &&
                !e.name.contains(needle, Qt::CaseInsensitive) &&
                !e.database.contains(needle, Qt::CaseInsensitive))
                continue;
            _visible.push_back(i);
        }
        endResetModel();
        if (oldCount != static_cast<int>(_visible.size()))
            emit countChanged();
    }

    void QueryHistoryModel::flush()
    {
        if (_saveTimer.isActive()) {
            _saveTimer.stop();
            saveNow();
        }
    }

    void QueryHistoryModel::scheduleSave()
    {
        _saveTimer.start();
    }

    void QueryHistoryModel::saveNow()
    {
        QString error;
        if (!_store.save(_filePath, &error))
            LOG_MSG(QStringLiteral("Cannot save query history (%1): %2").arg(_filePath, error),
                    LogSeverity::Error);
    }
}
