#pragma once
#include <vector>
#include <QAbstractItemModel>
#include "ragoshell/core/Core.h"

namespace RagoShell
{
    class BsonTreeItem;

    class BsonTreeModel : public QAbstractItemModel
    {
        Q_OBJECT

    public:
        typedef QAbstractItemModel BaseClass;

        /// Column-free roles for QML TreeView / TableView delegates.
        enum BsonRole {
            BsonKeyRole = Qt::UserRole + 1,
            BsonValueRole,
            BsonRawValueRole,
            BsonTypeNameRole,
            BsonHasChildrenRole,
            BsonFieldPathRole,
            BsonDocIndexRole,
            BsonEditableScalarRole
        };

        static QString bsonTypeName(BsonTreeItem *item);
        static QString buildFieldPath(const BsonTreeItem *item);
        static bool isEditableScalarItem(const BsonTreeItem *item);
        int documentIndexForItem(const BsonTreeItem *item) const;
        QHash<int, QByteArray> roleNames() const override;
        explicit BsonTreeModel(const std::vector<MongoDocumentPtr> &documents, QObject *parent = nullptr);
        ~BsonTreeModel() override;
        QVariant data(const QModelIndex &index, int role) const override;

        int rowCount(const QModelIndex &parent = QModelIndex()) const override;
        int columnCount(const QModelIndex &parent) const override;

        QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
        Qt::ItemFlags flags(const QModelIndex &index) const override;
        QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
        QModelIndex parent(const QModelIndex &index) const override;

        void insertItem(BsonTreeItem *parent, BsonTreeItem *children);
        void removeitem(BsonTreeItem *children);

        void fetchMore(const QModelIndex &parent) override;
        bool canFetchMore(const QModelIndex &parent) const override;
        bool hasChildren(const QModelIndex &parent = QModelIndex()) const override;
    protected:
        const std::vector<MongoDocumentPtr> _documents;
        BsonTreeItem *_root;
    };
}
