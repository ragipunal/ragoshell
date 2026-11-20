#include "ragoshell/core/bson/BsonTableModel.h"

#include <QBrush>

#include "ragoshell/core/bson/BsonTreeItem.h"
#include "ragoshell/core/bson/BsonTreeModel.h"
#include "ragoshell/core/utils/BsonUtils.h"
#include "ragoshell/core/utils/QtUtils.h"

namespace RagoShell
{
    BsonTableModelProxy::BsonTableModelProxy(QObject *parent) 
        : BaseClass(parent)
    {
       
    }

    int BsonTableModelProxy::rowCount(const QModelIndex &parent) const
    {
        if (parent.isValid() || !sourceModel())
            return 0;
        return sourceModel()->rowCount(parent);
    }

    QModelIndex BsonTableModelProxy::parent( const QModelIndex& /*index*/ ) const
    {
        return QModelIndex();
    }

    int BsonTableModelProxy::columnCount(const QModelIndex &parent) const
    {
        if (parent.isValid())
            return 0;
        return static_cast<int>(_columns.size());
    }

    QModelIndex BsonTableModelProxy::mapFromSource( const QModelIndex & sourceIndex ) const
    {
        int row = sourceIndex.row();
        int col = sourceIndex.column();

        BsonTreeItem *node = QtUtils::item<BsonTreeItem *>(sourceIndex);
        if (!node || _columns.size() <= col)
            return QModelIndex();

        BsonTreeItem *child = node->childByKey(_columns[col]);

        return createIndex( row, col, child );
    }

    QModelIndex BsonTableModelProxy::sibling(int row, int column, const QModelIndex &idx) const
    {
        return BaseClass::sibling(row, 0, idx);
    }

    QModelIndex BsonTableModelProxy::index( int row, int col, const QModelIndex& parent ) const
    {
        if (!sourceModel())
            return QModelIndex();
        BsonTreeItem *node = QtUtils::item<BsonTreeItem *>(sourceModel()->index(row, 0, parent));
        if (!node || _columns.size() <= col)
            return QModelIndex();

        BsonTreeItem *child = node->childByKey(_columns[col]);

        return createIndex( row, col, child );
    }

    QModelIndex BsonTableModelProxy::mapToSource( const QModelIndex &proxyIndex ) const
    {
        if (!proxyIndex.isValid() || !sourceModel())
            return QModelIndex();

        Q_ASSERT(proxyIndex.model() == this);
        if (!proxyIndex.internalPointer())
            return QModelIndex();

        return sourceModel()->index(proxyIndex.row(), 0);
    }

    void BsonTableModelProxy::setSourceModel( QAbstractItemModel* model )
    {
        _columns.clear();
        _columnLookup.clear();
        _root = nullptr;
        if (model) {
            BsonTreeItem *child = QtUtils::item<BsonTreeItem *>(model->index(0, 0));
            if (child) {
                _root = qobject_cast<BsonTreeItem *>(child->parent());
                if (_root) {
                    int count = _root->childrenCount();
                    for (int i = 0; i < count; ++i) {
                        BsonTreeItem *child = _root->child(i);
                        int countc = child->childrenCount();
                        for (int j = 0; j < countc; ++j) {
                            addColumn(child->child(j)->key());
                        }
                    }
                }
            }
        }
        return BaseClass::setSourceModel(model);
    }

    QHash<int, QByteArray> BsonTableModelProxy::roleNames() const
    {
        QHash<int, QByteArray> roles = BaseClass::roleNames();
        roles[BsonTreeModel::BsonKeyRole] = "key";
        roles[BsonTreeModel::BsonValueRole] = "value";
        roles[BsonTreeModel::BsonRawValueRole] = "rawValue";
        roles[BsonTreeModel::BsonTypeNameRole] = "typeName";
        roles[BsonTreeModel::BsonHasChildrenRole] = "expandable";
        roles[BsonTreeModel::BsonFieldPathRole] = "fieldPath";
        roles[BsonTreeModel::BsonDocIndexRole] = "docIndex";
        roles[BsonTreeModel::BsonEditableScalarRole] = "editableScalar";
        return roles;
    }

    QVariant BsonTableModelProxy::data(const QModelIndex &index, int role) const
    {
        QVariant result;

        if (!index.isValid())
            return result;

        BsonTreeItem *node = QtUtils::item<BsonTreeItem *>(index);
        const QString colName = column(index.column());

        if (role == BsonTreeModel::BsonDocIndexRole)
            return index.row();
        if (role == BsonTreeModel::BsonKeyRole)
            return BsonUtils::sanitizeForDisplay(colName, true);
        if (role == BsonTreeModel::BsonFieldPathRole)
            return colName;

        if (!node) {
            if (role == Qt::BackgroundRole) {
                return QBrush("#f5f3f2");
            }
            if (role == BsonTreeModel::BsonEditableScalarRole) {
                return !colName.isEmpty() && colName != QLatin1String("_id");
            }
            if (role == BsonTreeModel::BsonRawValueRole || role == BsonTreeModel::BsonValueRole)
                return QString();
            if (role == BsonTreeModel::BsonTypeNameRole)
                return QString();
            return result;
        }

        const bool isCut = node->type() == bsoncxx::type::k_string ||
                           node->type() == bsoncxx::type::k_code ||
                           node->type() == bsoncxx::type::k_codewscope;

        if (role == BsonTreeModel::BsonValueRole) {
            const QString rawSlice = isCut ? node->value().left(600) : node->value();
            const QString clean = BsonUtils::sanitizeForDisplay(rawSlice, true);
            return isCut ? clean.left(300) : clean;
        }
        if (role == BsonTreeModel::BsonRawValueRole)
            return node->value();
        if (role == BsonTreeModel::BsonTypeNameRole)
            return BsonTreeModel::bsonTypeName(node);
        if (role == BsonTreeModel::BsonEditableScalarRole)
            return BsonTreeModel::isEditableScalarItem(node);
        if (role == BsonTreeModel::BsonHasChildrenRole)
            return false;

        if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
            if (role == Qt::ToolTipRole) {
                const QString rawSlice = isCut ? node->value().left(1000) : node->value();
                const QString clean = BsonUtils::sanitizeForDisplay(rawSlice, false);
                result = isCut ? clean.left(500) : clean;
            }
            else {
                const QString rawSlice = isCut ? node->value().left(600) : node->value();
                const QString clean = BsonUtils::sanitizeForDisplay(rawSlice, true);
                result = isCut ? clean.left(300) : clean;
            }
        }

        return result;
    }

    QVariant BsonTableModelProxy::headerData(int section, Qt::Orientation orientation, int role) const
    {
        if (role != Qt::DisplayRole)
            return QVariant();

        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            return BsonUtils::sanitizeForDisplay(column(section), true);
        } else {
            return QString("%1").arg(section + 1);
        }
    }

    QString BsonTableModelProxy::column(int col) const
    {
        if (col < 0 || static_cast<size_t>(col) >= _columns.size())
            return QString();
        return _columns[col];
    }

    size_t BsonTableModelProxy::findIndexColumn(const QString &col) const
    {
        const auto it = _columnLookup.constFind(col);
        return (it != _columnLookup.constEnd()) ? it.value() : _columns.size();
    }

    size_t BsonTableModelProxy::addColumn(const QString &col)
    {
        const auto it = _columnLookup.constFind(col);
        if (it != _columnLookup.constEnd())
            return it.value();
        const size_t column = _columns.size();
        _columns.push_back(col);
        _columnLookup.insert(col, column);
        return column;
    }
}
