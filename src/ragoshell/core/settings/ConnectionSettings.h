#pragma once

#include <QString>
#include <QVariant>
#include <QVariantMap>

#include <mongocxx/uri.hpp>

#include "ragoshell/core/domain/HostAndPort.h"

namespace RagoShell
{
    class CredentialSettings;
    class SshSettings;
    class SslSettings;
    class ReplicaSetSettings;

    /**
     * @brief Represents connection record
     */
    class ConnectionSettings : public QObject
    {
        Q_OBJECT

    public:
        /**
         * @brief Creates ConnectionSettings with default values
         */
        ConnectionSettings(bool isClone);
        
        /**
        * @brief Creates ConnectionSettings from mongo connection string URI
        */
        ConnectionSettings(const mongocxx::uri& uri, bool isClone);

        explicit ConnectionSettings(QVariantMap map, bool isClone);

        /**
         * @brief Cleanup used resources
         */
        ~ConnectionSettings();

        /**
         * @brief Creates completely new ConnectionSettings by cloning this record.
         */
        ConnectionSettings *clone() const;

        /**
         * @brief Discards current state and applies state from 'source' ConnectionSettings.
         */
        void apply(const ConnectionSettings *source);

        /**
         * @brief Converts to QVariantMap
         */
        QVariant toVariant() const;
        void fromVariant(const QVariantMap &map);

        /**
         * @brief Name of connection
         */
        std::string connectionName() const { return _connectionName; }
        void setConnectionName(const std::string &connectionName) { _connectionName = connectionName; }

        /**
         * @brief Server host
         */
        std::string serverHost() const { return _host; }
        void setServerHost(const std::string &serverHost) { _host = serverHost; }

        /**
         * @brief Port of server
         */
        int serverPort() const { return _port; }
        void setServerPort(const int port) { _port = port; }

        /**
         * @brief Default database
         */
        std::string defaultDatabase() const { return _defaultDatabase; }
        void setDefaultDatabase(const std::string &defaultDatabase) { _defaultDatabase = defaultDatabase; }

        /**
         * @brief Background schema collection for this connection
         * ("off", "onUse", "wholeDatabase"); empty = the Preferences setting.
         */
        std::string schemaCollectionMode() const { return _schemaCollectionMode; }
        void setSchemaCollectionMode(const std::string &mode) { _schemaCollectionMode = mode; }

        /**
         * @brief Adds credential to this connection
         */
        void addCredential(CredentialSettings *credential);

        /**
         * @brief Clears and releases memory occupied by credentials
         */
        void clearCredentials();

        /**
         * @brief Checks whether this connection has primary credential
         * which is also enabled.
         */
        bool hasEnabledPrimaryCredential() const;

        /**
         * @brief Returns primary credential
         */
        CredentialSettings *primaryCredential() const;

        /**
         * @brief Returns number of credentials
         */
        int credentialCount() const { return _credentials.size(); }

        /**
         * @brief Returns all credentials
         */
        QList<CredentialSettings *> credentials() const { return _credentials; }

        /**
         * @brief Checks that auth required
         */
        /*bool isAuthNeeded() const {
            bool userSpecified = !_userName.isEmpty();
            bool passwordSpecified = !_userPassword.isEmpty();

            return (userSpecified || passwordSpecified);
        }*/

        /**
         * @brief Returns connection full address (i.e. locahost:8090)
         */
        std::string getFullAddress() const;

        std::string getReadableName() const
        {
            if (_connectionName.empty())
                return getFullAddress();

            return _connectionName;
        }

        HostAndPort hostAndPort() const;
        SshSettings *sshSettings() const { return _sshSettings.get(); }
        SslSettings *sslSettings() const { return _sslSettings.get(); }
        ReplicaSetSettings *replicaSetSettings() const { return _replicaSetSettings.get(); }

        // SRV topology is discovered by the driver; the app's replica set
        // handling (member list, primary rewrite) must not kick in.
        bool isReplicaSet() const { return _isReplicaSet && !_isSrv; }
        void setReplicaSet(bool flag) { _isReplicaSet = flag; }

        /**
         * @brief DNS seedlist connection (mongodb+srv://). serverHost() is the
         * SRV hostname; port, replica set members and SSH tunnel are unused.
         */
        bool isSrv() const { return _isSrv; }
        void setSrv(bool flag) { _isSrv = flag; }

        QString uuid() const { return _uuid; }
        void setUuid(QString const& uuid) { _uuid = uuid; }

        std::string colorTag() const { return _colorTag; }
        void setColorTag(const std::string &tag) { _colorTag = tag; }

        /// Wire path selector — empty = native driver; "memory" = in-browser
        /// demo catalog (WASM playground); "agent" / "ws://…" / "wss://…" =
        /// WebSocket gateway (Mod B/C). docs/TransportSeam.md §4, docs/LocalAgent.md.
        std::string transportKind() const { return _transportKind; }
        void setTransportKind(const std::string &kind) { _transportKind = kind; }
        bool hasTransport() const { return !_transportKind.empty(); }

        /// Pairing token for the WebSocket hello handshake (Mod C agent).
        std::string transportToken() const { return _transportToken; }
        void setTransportToken(const std::string &token) { _transportToken = token; }

    private:
        CredentialSettings *findCredential(const std::string &databaseName) const;

        std::string _connectionName;
        std::string _host;
        int _port;
        std::string _defaultDatabase;
        std::string _schemaCollectionMode;
        std::string _colorTag;
        std::string _transportKind;
        std::string _transportToken;
        mutable QList<CredentialSettings *> _credentials;
        std::unique_ptr<SshSettings> _sshSettings;
        std::unique_ptr<SslSettings> _sslSettings;
        bool _isReplicaSet;
        bool _isSrv = false;
        std::unique_ptr<ReplicaSetSettings> _replicaSetSettings;
        
        // Flag to check if this is a clone(copy) or original ConnectionSettings
        // Note: If this is not a clone connection settings, this object is original connection 
        //       ConnectionSettings object which is loaded/saved into RagoShell config. file.
        bool _clone = false;

        // If this is a clone connection settings, unique ID will be used to identify from which 
        // original connection settings this object is cloned.
        // -1 for invalid(uninitialized) unique ID which should not be seen in theory
        int _uniqueId = -1;

        // UUID string taken from QUuid. 
        // It is used to identify the unique ID of a connection settings object
        QString _uuid;
    };
}

Q_DECLARE_METATYPE(RagoShell::ConnectionSettings *)
