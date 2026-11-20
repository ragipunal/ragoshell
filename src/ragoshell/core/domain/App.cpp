#include <sstream>
#include "ragoshell/core/domain/App.h"

#include <QHash>

#include "ragoshell/core/services/UiServices.h"

#include "ragoshell/core/domain/MongoServer.h"
#include "ragoshell/core/domain/MongoShell.h"
#include "ragoshell/core/domain/MongoCollection.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/ReplicaSetSettings.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/mongodb/SshTunnelWorker.h"
#include "ragoshell/core/EventBus.h"
#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell/core/utils/StdUtils.h"
#include "ragoshell/core/utils/Logger.h"

namespace RagoShell
{
    namespace detail
    {
        QString buildCollectionQuery(const std::string &collectionName, const QString &postfix)
        {
            QString qCollectionName = QtUtils::toQString(collectionName);

            QString pattern;

            // Use db.getCollection() to avoid having to enumerate and special case "reserved" names
            pattern = "db.getCollection(\'%1\').%2";

            // Escape '\' symbol
            qCollectionName.replace(QChar('\\'), "\\\\");

            return pattern.arg(qCollectionName).arg(postfix);
        }
    }

    App::~App()
    {
        for (auto &entry : _pendingSshWorkers) {
            if (entry.second)
                entry.second->stop();
        }
        _pendingSshWorkers.clear();
    }

    App::App(EventBus *const bus) : QObject(),
        _bus(bus), _lastServerHandle(0) {
        _bus->subscribe(this, EstablishSshConnectionResponse::Type);
        _bus->subscribe(this, ListenSshConnectionResponse::Type);
        _bus->subscribe(this, LogEvent::Type);
    }

    std::unique_ptr<MongoServer>
    App::continueOpenServer(int serverHandle, ConnectionSettings* connSettings, 
                            ConnectionType type, int localport)
    {
        ConnectionSettings* connSettingsClone = connSettings->clone();

        // Modify connection settings when SSH tunnel is used
        if ((type == ConnectionPrimary || type == ConnectionTest)
            && !connSettingsClone->isReplicaSet()
            && !connSettingsClone->isSrv()
            && connSettingsClone->sshSettings()->enabled()
        ) {
            connSettingsClone->setServerHost("127.0.0.1");
            connSettingsClone->setServerPort(localport);
        }

        auto server { std::make_unique<MongoServer>(serverHandle, connSettingsClone, type) };
        server->runWorkerThread();

        auto replicaSetStr = QString::fromStdString(connSettings->connectionName()) + " [Replica Set]";
        replicaSetStr = (connSettings->replicaSetSettings()->members().size() > 0)
            ? replicaSetStr + QString::fromStdString(connSettings->replicaSetSettings()->members()[0])
            : replicaSetStr + "";

        QString serverAddress = connSettings->isReplicaSet()
            ? replicaSetStr
            : QString::fromStdString(connSettings->getFullAddress());

        LOG_MSG(QString("Connecting to %1...").arg(serverAddress), RagoShell::LogSeverity::Info);
        server->tryConnect();
        return server;
    }

    /**
    * Creates and opens new server connection.
    * @param connection: ConnectionSettings, that will be owned by MongoServer.
    * @param visible: should this server be visible in UI (explorer) or not.
    */
    std::unique_ptr<MongoServer> 
    App::openServerInternal(ConnectionSettings* connSettings, ConnectionType type) 
    {
        ++_lastServerHandle;

        if (type == ConnectionPrimary)
            _bus->publish(new ConnectingEvent(this));

        // When connection is SECONDARY or SSH not enabled or replica set or SRV,
        // then continue without SSH Tunnel
        if (type == ConnectionSecondary || !connSettings->sshSettings()->enabled()
            || connSettings->isReplicaSet() || connSettings->isSrv()
        ) {
            return continueOpenServer(_lastServerHandle, connSettings, type);
        }

        // Open SSH channel and only after that open connection
        LOG_MSG(QString("Creating SSH tunnel to %1:%2...")
            .arg(QtUtils::toQString(connSettings->sshSettings()->host()))
            .arg(connSettings->sshSettings()->port()), RagoShell::LogSeverity::Info);

        ConnectionSettings *settingsCopy = connSettings->clone();
        auto sshWorker = std::make_unique<SshTunnelWorker>(settingsCopy);
        SshTunnelWorker *rawWorker = sshWorker.get();
        const int serverHandle = _lastServerHandle;
        _pendingSshWorkers[serverHandle] = std::move(sshWorker);
        rawWorker->startInitialConnect(this, serverHandle, type);
        return nullptr;
    }

    bool App::openServer(ConnectionSettings *connection, ConnectionType type) 
    {
        SshSettings *ssh = connection->sshSettings();

        if (!connection->isReplicaSet() && !connection->isSrv() && ssh->enabled() && ssh->askPassword() &&
            (type == ConnectionPrimary || type == ConnectionTest)) {
            bool ok = false;

            bool isByKey = ssh->authMethod() == "publickey";
            std::string passText = isByKey ? "passphrase" : "password";

            std::stringstream s;
            s << "In order to continue, please provide the " << passText;

            if (isByKey)
                s << " for the key file";

            s << "." << std::endl << std::endl;

            if (ssh->authMethod() == "publickey")
                s << "Private Key:  " << ssh->privateKeyFile() << std::endl;

            s << "Server: " << ssh->host() << std::endl;
            s << "User: " << ssh->userName() << std::endl;


            s << std::endl << "Enter your " << passText << " that will never be stored:";

            QString userInput;
            if (auto *ui = UiServices::instance())
                userInput = ui->getText(tr("SSH Authentication"),
                                        QtUtils::toQString(s.str()), true, &ok);

            if (!ok)
                return false;

            ssh->setAskedPassword(QtUtils::toStdString(userInput));
        }

        SslSettings *sslSettings = connection->sslSettings();

        if (sslSettings->sslEnabled() && sslSettings->usePemFile() && sslSettings->askPassphrase() 
            && (type == ConnectionPrimary || type == ConnectionTest)) 
        {
            if (!askSslPassphrasePromptDialog(connection))
            {
                return false;
            }
        }

        if (auto srv = openServerInternal(connection, type))
            _servers.push_back(std::move(srv));
        return true;
    }

    /**
     * @brief Closes MongoServer connection and frees all resources, owned
     * by MongoServer. Finally, specified MongoServer will also be deleted.
     */
    void App::closeServer(MongoServer *server)
    {
        _servers.erase(std::remove_if(_servers.begin(), _servers.end(), 
            [&](auto const& el) { return el.get() == server; }), _servers.end());
    }

    void App::openShell(MongoCollection *collection, const QString &filePathToSave)
    {
        ConnectionSettings *connection = collection->database()->server()->connectionRecord();
        auto const& dbname = collection->database()->name();
        connection->setDefaultDatabase(dbname);
        QString const& script = detail::buildCollectionQuery(collection->name(), "find({})");
        openShell(collection->database()->server(), connection, 
            ScriptInfo(script, true, dbname, CursorPosition(0, -2), QtUtils::toQString(dbname), filePathToSave)
        );
    }

    void App::openShell(MongoServer *server, const QString &script, const std::string &dbName,
                        bool execute, const QString &shellName,
                        const CursorPosition &cursorPosition, const QString &filePathToSave)
    {
        ConnectionSettings *connection = server->connectionRecord();

        if (!dbName.empty())
            connection->setDefaultDatabase(dbName);

        openShell(server, connection, 
            ScriptInfo(script, execute, dbName, cursorPosition, shellName, filePathToSave)
        );
    }

    void App::openShell(MongoDatabase *database, const QString &script,
                               bool execute, const QString &shellName,
                               const CursorPosition &cursorPosition, const QString &filePathToSave)
    {
        ConnectionSettings *connection = database->server()->connectionRecord();
        connection->setDefaultDatabase(database->name());
        openShell(database->server(), connection, ScriptInfo(script, execute, database->name(), 
                                                             cursorPosition, shellName, filePathToSave));
    }

    void App::openShell(MongoServer* server, ConnectionSettings* connection, const ScriptInfo &scriptInfo)
    {
        // Transport-mode connections (WASM demo / gateway) have no shell
        // path yet — ScriptEngine owns a raw mongocxx client that would
        // bypass the configured channel (8.6 review M2). Fail loudly at
        // the call sites; refuse here for every other caller.
        if (connection->hasTransport())
            return;

        auto serverClone{ openServerInternal(connection, ConnectionSecondary) };
        if (!serverClone || !server)
            return;

        auto shell{ std::make_unique<MongoShell>(serverClone.get(), scriptInfo) };
        _servers.push_back(std::move(serverClone));
        // Connection between explorer's server and tab's MongoShells
        _bus->subscribe(server, ReplicaSetRefreshed::Type, shell.get()); 
        _bus->publish(new OpeningShellEvent(this, shell.get()));
        shell->execute();
        _shells.push_back(std::move(shell));
        return;
    }

    /**
     * @brief Closes MongoShell and frees all resources, owned by specified MongoShell.
     * Finally, specified MongoShell will also be deleted.
     */
    void App::closeShell(MongoShell *shell)
    {
        auto const itr = std::find_if(_shells.begin(), _shells.end(), 
            [&](auto const& el) { return el.get() == shell; }
        );
        
        // Do nothing, if this shell not owned by this App.
        if (itr == _shells.end())
            return;

        closeServer(shell->server());
        _shells.erase(itr);
    }

    void App::handle(EstablishSshConnectionResponse *event) {
        std::unique_ptr<SshTunnelWorker> sshWorker;
        auto pendingIt = _pendingSshWorkers.find(event->serverHandle);
        if (pendingIt != _pendingSshWorkers.end()) {
            sshWorker = std::move(pendingIt->second);
            _pendingSshWorkers.erase(pendingIt);
        }

        if (event->isError()) {
            if (sshWorker) {
                sshWorker->stop();
                sshWorker.reset();
            }
            _bus->publish(new ConnectionFailedEvent(
                this, event->serverHandle, event->connectionType, event->error().errorMessage(),
                ConnectionFailedEvent::SshConnection));
            return;
        }

        LOG_MSG(QString("SSH tunnel created successfully"), RagoShell::LogSeverity::Info);

        auto server = continueOpenServer(event->serverHandle, event->settings, event->connectionType, event->localport);
        if (server && sshWorker)
            server->attachSshTunnel(std::move(sshWorker));
        if (server)
            _servers.push_back(std::move(server));
    }

    void App::handle(LogEvent *event) {
        LOG_MSG(event->message, event->mongoLogSeverity());

        if (!event->informUser)
            return;

        if (auto *ui = UiServices::instance()) {
            ui->showMessage(QString::fromStdString(event->severity()),
                            QtUtils::toQString(event->severity() + ": " + event->message),
                            event->uiLevel());
        }
    }

    void App::handle(ListenSshConnectionResponse *event) {
        if (event->isError()) {
            _bus->publish(
                new ConnectionFailedEvent(this, event->serverHandle, event->connectionType, 
                    event->error().errorMessage(), ConnectionFailedEvent::SshChannel)
            );
            return;
        }

        LOG_MSG(QString("SSH tunnel closed."), RagoShell::LogSeverity::Error);
    }

    void App::fireConnectionFailedEvent(int serverHandle, ConnectionType type, std::string errormsg,
                                        ConnectionFailedEvent::Reason reason) {
        _bus->publish(new ConnectionFailedEvent(this, serverHandle, type, errormsg, reason));
    }

    void App::updateSecondaryShellsLocalPort(MongoServer *primaryServer, int oldLocalPort, int newLocalPort)
    {
        if (!primaryServer || !primaryServer->connectionRecord() || oldLocalPort <= 0 || newLocalPort <= 0)
            return;

        const QString primaryUuid = primaryServer->connectionRecord()->uuid();
        for (const auto &srv : _servers) {
            if (!srv || srv.get() == primaryServer || !srv->connectionRecord())
                continue;
            if (srv->connectionRecord()->uuid() == primaryUuid &&
                srv->connectionRecord()->serverHost() == "127.0.0.1" &&
                srv->connectionRecord()->serverPort() == oldLocalPort) {
                srv->updateTunnelLocalPort(newLocalPort);
            }
        }
    }

    bool App::askSslPassphrasePromptDialog(ConnectionSettings *connSettings) const
    {
        auto sslSettings = connSettings->sslSettings();
        bool ok = false;

        std::stringstream s;
        s << "In order to continue, please provide the passphrase";
        s << "." << std::endl << std::endl;

        s << "Server: " << connSettings->serverHost() << ":" << connSettings->serverPort() << std::endl;
        s << "PEM file: " << sslSettings->pemKeyFile() << std::endl;

        s << std::endl << "Enter your PEM key passphrase (will never be stored):";

        QString userInput;
        if (auto *ui = UiServices::instance())
            userInput = ui->getText(tr("TLS Authentication"),
                                    QtUtils::toQString(s.str()), true, &ok);

        if (!ok)
        {
            return false;
        }

        sslSettings->setPemPassPhrase(QtUtils::toStdString(userInput));
        return ok;
    }

}
