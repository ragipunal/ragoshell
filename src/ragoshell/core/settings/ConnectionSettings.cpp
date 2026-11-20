#include "ragoshell/core/settings/ConnectionSettings.h"

#include <stdio.h>

#include <QUuid>

#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/settings/ReplicaSetSettings.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell/utils/RagoCrypt.h"

#include <algorithm>

namespace
{
    const unsigned port = 27017;
    const char *defaultServerHost = "localhost";
    const char *defaultNameConnection = "New Connection";

    const int maxLength = 300;
}

namespace RagoShell
{
    /**
     * Creates ConnectionSettings with default values
     */
    ConnectionSettings::ConnectionSettings(bool isClone) : QObject(),
        _connectionName(defaultNameConnection),
        _host(defaultServerHost),
        _port(port),
        _sshSettings(new SshSettings()),
        _sslSettings(new SslSettings()),
        _isReplicaSet(false),
        _replicaSetSettings(new ReplicaSetSettings()),
        _clone(isClone)
    { }

    ConnectionSettings::ConnectionSettings(const mongocxx::uri& uri, bool isClone)  
        : _connectionName(defaultNameConnection),
        _host(defaultServerHost),
        _port(port),
        _sshSettings(new SshSettings()),
        _sslSettings(new SslSettings()),
        _isReplicaSet((!uri.replica_set().empty() || uri.hosts().size() > 1)),
        _replicaSetSettings(new ReplicaSetSettings(uri)),
        _clone(isClone)
    {
        // Disabling unfinished feature : Export / Import to / from MongoURI
        /*
        if (!uri.getServers().empty()) {
            _host = uri.getServers().front().host();
            _port = uri.getServers().front().port();
        }

        auto str = std::string(uri.getOptions().getStringField("ssl"));
        auto sslEnabled = ("true" == str);
        if (sslEnabled) {
            _sslSettings->enableSSL(true);
            _sslSettings->setAllowInvalidCertificates(true);
        }

        auto credential = new CredentialSettings();
        credential->setUserName(uri.getUser());
        credential->setUserPassword(uri.getPassword());
        credential->setDatabaseName(uri.getDatabase());
        if (!credential->userName().empty() && !credential->userPassword().empty()) {   // todo:
            credential->setEnabled(true);
        }
        addCredential(credential);
        */
    }

    void ConnectionSettings::fromVariant(const QVariantMap &map) {
        setConnectionName(QtUtils::toStdString(map.value("connectionName").toString()));
        setServerHost(QtUtils::toStdString(map.value("serverHost").toString().left(maxLength)));
        setServerPort(map.value("serverPort").toInt());
        setDefaultDatabase(QtUtils::toStdString(map.value("defaultDatabase").toString()));
        setSchemaCollectionMode(QtUtils::toStdString(map.value("schemaCollectionMode").toString()));
        setColorTag(QtUtils::toStdString(map.value("colorTag").toString()));
        setTransportKind(QtUtils::toStdString(map.value("transportKind").toString()));
        // "transportTokenEncrypted" supersedes the plaintext key used by early
        // 8.3 builds — same *Encrypted convention as userPasswordEncrypted.
        if (map.contains("transportToken"))
            setTransportToken(QtUtils::toStdString(map.value("transportToken").toString()));
        else if (map.contains("transportTokenEncrypted"))
            setTransportToken(RagoCrypt::decrypt(
                map.value("transportTokenEncrypted").toString().toStdString()));
        setReplicaSet(map.value("isReplicaSet").toBool());
        setSrv(map.value("isSrv").toBool());

        QVariantList list = map.value("credentials").toList();
        for (QVariantList::const_iterator it = list.begin(); it != list.end(); ++it) {
            QVariant var = *it;
            CredentialSettings *credential = new CredentialSettings(var.toMap());
            addCredential(credential);
        }

        if (map.contains("ssh")) {
            _sshSettings->fromVariant(map.value("ssh").toMap());
        }

        if (map.contains("ssl")) {
            _sslSettings->fromVariant(map.value("ssl").toMap());
        }

        if (isReplicaSet()) {
            _replicaSetSettings->fromVariant(map.value("replicaSet").toMap());
        }

        // If UUID has never been created or is empty, create a new one. Otherwise load the existing.
        if (!map.contains("uuid") || map.value("uuid").toString().isEmpty())
            _uuid = QUuid::createUuid().toString();
        else
            _uuid = map.value("uuid").toString();


//#ifdef MONGO_SSL
//      ,SSLInfo(map.value("sslEnabled").toBool(),QtUtils::toStdString(map.value("sslPemKeyFile").toString()))
//#endif
    }

    /**
     * Cleanup used resources
     */
    ConnectionSettings::~ConnectionSettings()
    {
        clearCredentials();
    }

    /**
     * Creates completely new ConnectionSettings by cloning this record.
     */
    ConnectionSettings *ConnectionSettings::clone() const
    {
        auto settings = new ConnectionSettings(true);
        settings->setUuid(_uuid);
        settings->apply(this);
        return settings;
    }

    /**
     * Discards current state and applies state from 'source' ConnectionSettings.
     */
    void ConnectionSettings::apply(const ConnectionSettings *source)
    {
        setConnectionName(source->connectionName());
        setServerHost(source->serverHost());
        setServerPort(source->serverPort());
        setDefaultDatabase(source->defaultDatabase());
        setSchemaCollectionMode(source->schemaCollectionMode());
        setColorTag(source->colorTag());
        setTransportKind(source->transportKind());
        setTransportToken(source->transportToken());
        setReplicaSet(source->isReplicaSet());
        setSrv(source->isSrv());

        clearCredentials();
        QList<CredentialSettings *> cred = source->credentials();
        for (QList<CredentialSettings *>::iterator it = cred.begin(); it != cred.end(); ++it) {
            addCredential((*it)->clone());
        }

        _sshSettings.reset(source->sshSettings()->clone());
        _sslSettings.reset(source->sslSettings()->clone());
        _replicaSetSettings.reset(source->_replicaSetSettings->clone());

//#ifdef MONGO_SSL
//        setSslInfo(source->sslInfo());
//#endif
    }

    /**
     * @brief Converts to QVariantMap
     */
    QVariant ConnectionSettings::toVariant() const
    {
        QVariantMap map;
        map.insert("connectionName", QtUtils::toQString(connectionName()));
        map.insert("serverHost", QtUtils::toQString(serverHost()));
        map.insert("serverPort", serverPort());
        map.insert("defaultDatabase", QtUtils::toQString(defaultDatabase()));
        if (!_schemaCollectionMode.empty())
            map.insert("schemaCollectionMode", QtUtils::toQString(_schemaCollectionMode));
        if (!_colorTag.empty())
            map.insert("colorTag", QtUtils::toQString(_colorTag));
        if (!_transportKind.empty())
            map.insert("transportKind", QtUtils::toQString(_transportKind));
        if (!_transportToken.empty())
            map.insert("transportTokenEncrypted",
                       QtUtils::toQString(RagoCrypt::encrypt(_transportToken)));
        map.insert("isReplicaSet", isReplicaSet());
        if (isReplicaSet())
            map.insert("replicaSet", _replicaSetSettings->toVariant());
        if (isSrv())
            map.insert("isSrv", true);

#ifdef MONGO_SSL
        SSLInfo infl = _info.sslInfo();
        map.insert("sslEnabled", infl._sslSupport);
        map.insert("sslPemKeyFile", QtUtils::toQString(infl._sslPEMKeyFile));
#endif
        QVariantList list;
        for (auto const& cred : _credentials)
            list.append(cred->toVariant());
        
        map.insert("credentials", list);
        map.insert("ssh", _sshSettings->toVariant());
        map.insert("ssl", _sslSettings->toVariant());
        map.insert("uuid", _uuid); 

        return map;
    }

     CredentialSettings *ConnectionSettings::findCredential(const std::string &databaseName) const
     {
         CredentialSettings *result = nullptr;
         for (QList<CredentialSettings *>::const_iterator it = _credentials.begin(); it != _credentials.end(); ++it) {
             CredentialSettings *cred = *it;
             if (cred->databaseName() == databaseName) {
                 result = cred;
                 break;
             }
         }
         return result;
     }

    /**
     * @brief Adds credential to this connection
     */
    void ConnectionSettings::addCredential(CredentialSettings *credential)
    {
        if (!findCredential(credential->databaseName()))
            _credentials.append(credential);
    }

    /**
     * @brief Checks whether this connection has primary credential
     * which is also enabled.
     */
    bool ConnectionSettings::hasEnabledPrimaryCredential() const
    {
        if (_credentials.count() == 0)
            return false;

        return primaryCredential()->enabled();
    }

    /**
     * @brief Returns primary credential
     */
    CredentialSettings *ConnectionSettings::primaryCredential() const
    {
        if (_credentials.count() == 0) {
            _credentials.append(new CredentialSettings());
        }

        return _credentials.at(0);
    }

    /**
     * @brief Clears and releases memory occupied by credentials
     */
    void ConnectionSettings::clearCredentials()
    {
        qDeleteAll(_credentials);
        _credentials.clear();
    }

    std::string ConnectionSettings::getFullAddress() const
    {
        if (_isSrv)
            return _host;
        return hostAndPort().toString();
    }

    HostAndPort ConnectionSettings::hostAndPort() const 
    {
        // If it doesn't look like IPv6 address,
        // treat it like IPv4 or literal hostname
        if (_host.find(':') == std::string::npos) {
            return HostAndPort(_host, _port);
        }

        // The following code assumes, that it is IPv6 address
        // If address contains square brackets ("["), remove them:
        std::string hostCopy = _host;
        if (_host.find('[') != std::string::npos) {
            hostCopy.erase(std::remove(hostCopy.begin(), hostCopy.end(), '['), hostCopy.end());
            hostCopy.erase(std::remove(hostCopy.begin(), hostCopy.end(), ']'), hostCopy.end());
        }

        return HostAndPort(hostCopy, _port);
    }

}
