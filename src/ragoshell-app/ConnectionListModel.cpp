#include "ragoshell-app/ConnectionListModel.h"

#include <algorithm>
#include <QUuid>

#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell/core/utils/SchemaCatalog.h"

namespace RagoShell
{
    ConnectionListModel::ConnectionListModel(SettingsManager *settings, QObject *parent)
        : QAbstractListModel(parent), _settings(settings)
    {
    }

    int ConnectionListModel::rowCount(const QModelIndex &parent) const
    {
        return parent.isValid() ? 0
                                : static_cast<int>(_settings->connections().size());
    }

    QVariant ConnectionListModel::data(const QModelIndex &index, int role) const
    {
        if (!index.isValid() || index.row() < 0 ||
            index.row() >= static_cast<int>(_settings->connections().size()))
            return {};

        ConnectionSettings *conn = _settings->connections().at(index.row());
        switch (role) {
        case NameRole:
            return QtUtils::toQString(conn->connectionName());
        case HostRole:
            return QtUtils::toQString(conn->serverHost());
        case PortRole:
            return conn->serverPort();
        case AddressRole:
            return QtUtils::toQString(conn->getFullAddress());
        case UuidRole:
            return conn->uuid();
        case SshEnabledRole:
            return conn->sshSettings() && conn->sshSettings()->enabled();
        case SslEnabledRole:
            return conn->sslSettings() && conn->sslSettings()->sslEnabled();
        case ReplicaSetRole:
            return conn->isReplicaSet();
        case AuthEnabledRole:
            return conn->hasEnabledPrimaryCredential();
        case DefaultDatabaseRole:
            return QtUtils::toQString(conn->defaultDatabase());
        case ColorTagRole:
            return QtUtils::toQString(conn->colorTag());
        }
        return {};
    }

    QHash<int, QByteArray> ConnectionListModel::roleNames() const
    {
        return {
            {NameRole, "name"},
            {HostRole, "host"},
            {PortRole, "port"},
            {AddressRole, "address"},
            {UuidRole, "uuid"},
            {SshEnabledRole, "sshEnabled"},
            {SslEnabledRole, "sslEnabled"},
            {ReplicaSetRole, "isReplicaSet"},
            {AuthEnabledRole, "authEnabled"},
            {DefaultDatabaseRole, "defaultDatabase"},
            {ColorTagRole, "colorTag"}
        };
    }

    QVariantMap ConnectionListModel::get(int row) const
    {
        if (row < 0 || row >= static_cast<int>(_settings->connections().size()))
            return {};
        ConnectionSettings *conn = _settings->connections().at(row);
        QVariantMap map = conn->toVariant().toMap();

        // Populate in-memory decrypted secrets so editing a connection in QML
        // preserves existing passwords/passphrases (toVariant() only writes
        // encrypted keys for disk persistence).
        QVariantList creds = map.value("credentials").toList();
        const auto &credObjs = conn->credentials();
        for (int i = 0; i < creds.size() && i < credObjs.size(); ++i) {
            QVariantMap cm = creds.at(i).toMap();
            cm.insert("userPassword", QtUtils::toQString(credObjs.at(i)->userPassword()));
            creds[i] = cm;
        }
        map.insert("credentials", creds);

        if (conn->sshSettings()) {
            QVariantMap ssh = map.value("ssh").toMap();
            ssh.insert("userPassword", QtUtils::toQString(conn->sshSettings()->userPassword()));
            ssh.insert("passphrase", QtUtils::toQString(conn->sshSettings()->passphrase()));
            map.insert("ssh", ssh);
        }

        if (conn->sslSettings()) {
            QVariantMap ssl = map.value("ssl").toMap();
            ssl.insert("pemPassPhrase", QtUtils::toQString(conn->sslSettings()->pemPassPhrase()));
            map.insert("ssl", ssl);
        }

        return map;
    }

    QString ConnectionListModel::create(const QVariantMap &map)
    {
        beginInsertRows({}, rowCount(), rowCount());
        auto *conn = new ConnectionSettings(false);
        conn->fromVariant(map);
        const QString uuid = conn->uuid();
        SettingsManager::addConnection(conn);
        endInsertRows();
        _settings->save();
        ++_revision;
        emit connectionsChanged();
        return uuid;
    }

    void ConnectionListModel::update(int row, const QVariantMap &map)
    {
        if (row < 0 || row >= rowCount())
            return;
        ConnectionSettings temp(true);  // temporary clone; not owned by SettingsManager
        temp.fromVariant(map);
        ConnectionSettings *conn = _settings->connections().at(row);
        conn->apply(&temp);
        emit dataChanged(index(row), index(row));
        _settings->save();
        ++_revision;
        emit connectionsChanged();
    }

    void ConnectionListModel::cloneConnection(int row)
    {
        if (row < 0 || row >= rowCount())
            return;
        QVariantMap map = get(row);
        map.insert(QStringLiteral("uuid"), QUuid::createUuid().toString());
        const QString baseName = map.value(QStringLiteral("connectionName")).toString().trimmed();
        map.insert(QStringLiteral("connectionName"),
                   (baseName.isEmpty() ? QStringLiteral("Connection") : baseName) + QStringLiteral(" (Copy)"));
        create(map);
    }

    void ConnectionListModel::remove(int row)
    {
        if (row < 0 || row >= rowCount())
            return;
        beginRemoveRows({}, row, row);
        ConnectionSettings *conn = _settings->connections().at(row);
        // Its analyzed schemas go with it.
        if (SchemaCatalog *catalog = SchemaCatalog::instance())
            catalog->clear(conn->uuid());
        // Its place on the MCP allow-list goes too: connectionsChanged reaches
        // McpSettingsBridge::pruneMissing(), which reports a failed write.
        _settings->removeConnection(conn);
        endRemoveRows();
        _settings->save();
        ++_revision;
        emit connectionsChanged();
    }

    void ConnectionListModel::moveUp(int row)
    {
        moveConnection(row, row - 1);
    }

    void ConnectionListModel::moveDown(int row)
    {
        moveConnection(row, row + 1);
    }

    void ConnectionListModel::moveConnection(int fromRow, int toRow)
    {
        if (fromRow < 0 || fromRow >= rowCount() ||
            toRow < 0 || toRow >= rowCount() || fromRow == toRow)
            return;
        auto conns = _settings->connections();
        ConnectionSettings *moved = conns.at(static_cast<size_t>(fromRow));
        conns.erase(conns.begin() + fromRow);
        conns.insert(conns.begin() + toRow, moved);
        _settings->reorderConnections(conns);
        const int lo = std::min(fromRow, toRow);
        const int hi = std::max(fromRow, toRow);
        emit dataChanged(index(lo), index(hi));
        _settings->save();
        ++_revision;
        emit connectionsChanged();
    }

    void ConnectionListModel::refresh()
    {
        beginResetModel();
        endResetModel();
        ++_revision;
        emit connectionsChanged();
    }

    QString ConnectionListModel::colorTagForUuid(const QString &uuid, int) const
    {
        if (!_settings || uuid.isEmpty())
            return QString();
        const int atPos = uuid.indexOf(QLatin1Char('@'));
        const QString baseUuid = atPos > 0 ? uuid.left(atPos) : uuid;
        if (ConnectionSettings *conn = _settings->getConnectionSettingsByUuid(baseUuid))
            return QtUtils::toQString(conn->colorTag());
        return QString();
    }
}
