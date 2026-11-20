#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>
#include <QDateTime>

namespace RagoShell
{
    /**
     * @brief Capped ring-buffer list model for application log lines.
     *        Fed by QmlBackend from EventBus LogEvent notifications.
     *        Roles: severity, time, message.
     */
    class LogModel : public QAbstractListModel
    {
        Q_OBJECT
        Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)

    public:
        enum Roles {
            SeverityRole = Qt::UserRole + 1,
            TimeRole,
            MessageRole
        };

        explicit LogModel(QObject *parent = nullptr);

        int rowCount(const QModelIndex &parent = {}) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        int count() const { return _entries.size(); }

        /// Appends a line; drops the oldest when over capacity.
        void append(const QString &severity, const QString &message);
        Q_INVOKABLE void clear();

    Q_SIGNALS:
        void countChanged();

    private:
        struct Entry {
            QString severity;
            QString time;
            QString message;
        };

        static constexpr int kCapacity = 500;
        QVector<Entry> _entries;
    };
}
