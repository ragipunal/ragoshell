#pragma once

#include <QAbstractListModel>
#include <QVariantMap>

namespace RagoShell
{
    class ConnectionSettings;
    class SettingsManager;

    /**
     * @brief Exposes the persisted saved-connections list
     *        (SettingsManager::connections()) to QML.
     */
    class ConnectionListModel : public QAbstractListModel
    {
        Q_OBJECT
        Q_PROPERTY(int count READ rowCount NOTIFY connectionsChanged FINAL)
        Q_PROPERTY(int revision READ revision NOTIFY connectionsChanged FINAL)

    public:
        enum Roles {
            NameRole = Qt::UserRole + 1,
            HostRole,
            PortRole,
            AddressRole,     // "host:port"
            UuidRole,
            SshEnabledRole,
            SslEnabledRole,
            ReplicaSetRole,
            AuthEnabledRole,
            DefaultDatabaseRole,
            ColorTagRole
        };

        explicit ConnectionListModel(SettingsManager *settings, QObject *parent = nullptr);

        int rowCount(const QModelIndex &parent = {}) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        /// Full QVariantMap (toVariant() format) for editing in the dialog.
        Q_INVOKABLE QVariantMap get(int row) const;

        /// Creates a new saved connection from a toVariant()-compatible map;
        /// returns its uuid.
        Q_INVOKABLE QString create(const QVariantMap &map);

        /// Overwrites the connection at 'row' with data from 'map'.
        Q_INVOKABLE void update(int row, const QVariantMap &map);

        /// Duplicates the connection at 'row' with a fresh UUID and " (Copy)" suffix.
        Q_INVOKABLE void cloneConnection(int row);

        /// Deletes the connection, its analyzed schemas and its place on the
        /// MCP allow-list (a later connection must never inherit that).
        Q_INVOKABLE void remove(int row);
        Q_INVOKABLE void moveUp(int row);
        Q_INVOKABLE void moveDown(int row);
        Q_INVOKABLE void moveConnection(int fromRow, int toRow);

        /// Re-read the list from SettingsManager (e.g. after external changes).
        Q_INVOKABLE void refresh();

        int revision() const { return _revision; }

        /// Lookup persisted colorTag (e.g. "#ef4444") for a connection UUID.
        /// Accepts optional 'revision' argument so QML bindings re-evaluate on updates.
        Q_INVOKABLE QString colorTagForUuid(const QString &uuid, int revision = 0) const;

    Q_SIGNALS:
        void connectionsChanged();

    private:
        SettingsManager *_settings;
        int _revision = 0;
    };
}
