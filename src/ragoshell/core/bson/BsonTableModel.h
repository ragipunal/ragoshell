#pragma once
#include <vector>

#include <QAbstractProxyModel>
#include <QHash>

namespace RagoShell
{
    class BsonTreeItem;

    class BsonTableModelProxy : public QAbstractProxyModel
    {
        Q_OBJECT

    public:
        typedef QAbstractProxyModel BaseClass;
        typedef std::vector<QString> ColumnsValuesType;

        explicit BsonTableModelProxy(QObject *parent = nullptr);
        QHash<int, QByteArray> roleNames() const override;
        QVariant data(const QModelIndex &index, int role) const override;

        int rowCount(const QModelIndex &parent = QModelIndex()) const override;
        int columnCount(const QModelIndex &parent) const override;

        QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
        QModelIndex index(int row, int col, const QModelIndex &index) const override;
        QModelIndex mapFromSource(const QModelIndex &sourceIndex) const override;
        QModelIndex mapToSource(const QModelIndex &proxyIndex) const override;
        void setSourceModel(QAbstractItemModel *model) override;
        QModelIndex parent(const QModelIndex &index) const override;
        QModelIndex sibling(int row, int column, const QModelIndex &idx) const override;
    private:
        QString column(int col) const;
        size_t addColumn(const QString &col);
        size_t findIndexColumn(const QString &col) const;

        ColumnsValuesType _columns;
        QHash<QString, size_t> _columnLookup;
        BsonTreeItem *_root = nullptr;
    };
}
