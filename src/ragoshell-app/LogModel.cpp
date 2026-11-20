#include "ragoshell-app/LogModel.h"

namespace RagoShell
{
    LogModel::LogModel(QObject *parent) : QAbstractListModel(parent) {}

    int LogModel::rowCount(const QModelIndex &parent) const
    {
        return parent.isValid() ? 0 : _entries.size();
    }

    QVariant LogModel::data(const QModelIndex &index, int role) const
    {
        if (!index.isValid() || index.row() >= _entries.size())
            return {};
        const Entry &e = _entries.at(index.row());
        switch (role) {
        case SeverityRole: return e.severity;
        case TimeRole:     return e.time;
        case MessageRole:  return e.message;
        }
        return {};
    }

    QHash<int, QByteArray> LogModel::roleNames() const
    {
        return {
            {SeverityRole, "severity"},
            {TimeRole, "time"},
            {MessageRole, "message"}
        };
    }

    void LogModel::append(const QString &severity, const QString &message)
    {
        if (_entries.size() >= kCapacity) {
            beginRemoveRows({}, 0, 0);
            _entries.removeFirst();
            endRemoveRows();
        }
        int row = _entries.size();
        beginInsertRows({}, row, row);
        _entries.append({severity,
                         QDateTime::currentDateTime().toString("HH:mm:ss.zzz"),
                         message});
        endInsertRows();
        emit countChanged();
    }

    void LogModel::clear()
    {
        if (_entries.isEmpty())
            return;
        beginResetModel();
        _entries.clear();
        endResetModel();
        emit countChanged();
    }
}
