#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QObject>

namespace RagoShell
{
    /**
     * @brief Lightweight QAbstractListModel of QObject* items so QML Repeaters
     *        insert/remove individual tab pages in O(1) without resetting or
     *        rebuilding already-open tabs.
     */
    class QmlObjectListModel : public QAbstractListModel
    {
        Q_OBJECT
        Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)

    public:
        enum Roles {
            ObjectRole = Qt::UserRole + 1
        };

        explicit QmlObjectListModel(QObject *parent = nullptr);

        int rowCount(const QModelIndex &parent = QModelIndex()) const override;
        QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
        QHash<int, QByteArray> roleNames() const override;

        int count() const { return _items.size(); }
        Q_INVOKABLE QObject *at(int index) const;
        Q_INVOKABLE int indexOf(QObject *obj) const;

        void append(QObject *obj);
        void removeAt(int index);
        void clear();

    Q_SIGNALS:
        void countChanged();

    private:
        QList<QObject *> _items;
    };
}
