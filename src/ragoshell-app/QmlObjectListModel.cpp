#include "ragoshell-app/QmlObjectListModel.h"

namespace RagoShell
{
    QmlObjectListModel::QmlObjectListModel(QObject *parent)
        : QAbstractListModel(parent)
    {
    }

    int QmlObjectListModel::rowCount(const QModelIndex &parent) const
    {
        return parent.isValid() ? 0 : _items.size();
    }

    QVariant QmlObjectListModel::data(const QModelIndex &index, int role) const
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= _items.size())
            return {};
        if (role == ObjectRole || role == Qt::DisplayRole)
            return QVariant::fromValue(_items.at(index.row()));
        return {};
    }

    QHash<int, QByteArray> QmlObjectListModel::roleNames() const
    {
        return {
            {ObjectRole, "itemObject"}
        };
    }

    QObject *QmlObjectListModel::at(int index) const
    {
        return (index >= 0 && index < _items.size()) ? _items.at(index) : nullptr;
    }

    int QmlObjectListModel::indexOf(QObject *obj) const
    {
        return _items.indexOf(obj);
    }

    void QmlObjectListModel::append(QObject *obj)
    {
        const int row = _items.size();
        beginInsertRows(QModelIndex(), row, row);
        _items.append(obj);
        endInsertRows();
        emit countChanged();
    }

    void QmlObjectListModel::removeAt(int index)
    {
        if (index < 0 || index >= _items.size())
            return;
        beginRemoveRows(QModelIndex(), index, index);
        _items.removeAt(index);
        endRemoveRows();
        emit countChanged();
    }

    void QmlObjectListModel::clear()
    {
        if (_items.isEmpty())
            return;
        beginResetModel();
        _items.clear();
        endResetModel();
        emit countChanged();
    }
}
