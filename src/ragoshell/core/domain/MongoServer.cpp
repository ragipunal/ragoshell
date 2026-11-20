#include <sstream>
#include <QPointer>
#include "ragoshell/core/domain/MongoServer.h"

#include "ragoshell/core/domain/MongoDatabase.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/settings/ReplicaSetSettings.h"
#include "ragoshell/core/mongodb/MongoWorker.h"
#include "ragoshell/core/mongodb/SshTunnelWorker.h"
#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/domain/App.h"
#include "ragoshell/core/EventBus.h"
#include "ragoshell/core/events/MongoEventsInfo.h"
#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell/core/UiSignals.h"
#include "ragoshell/utils/common.h"
#include "ragoshell/utils/StringOperations.h"


namespace RagoShell {
    R_REGISTER_EVENT(MongoServerLoadingDatabasesEvent)

    MongoServer::MongoServer(int handle, ConnectionSettings *settings, ConnectionType connectionType) 
        : QObject(),
        _version(0.0f),
        _connectionType(connectionType),
        _worker(nullptr),
        _isConnected(false),
        _connSettings(settings),
        _handle(handle),
        _bus(AppRegistry::instance().bus()),
        _app(AppRegistry::instance().app()),
        _replicaSetInfo(nullptr),
        _isExplorerServer(connectionType == ConnectionPrimary)
    {}

    bool MongoServer::isConnected() const {
        return _isConnected;
    }

    ConnectionSettings *MongoServer::connectionRecord() const {
        return _connSettings.get();
    }

    MongoServer::~MongoServer() {
        clearDatabases();

        if (_worker) {
            _worker->stopAndDelete();
        }

        if (_sshTunnel) {
            _sshTunnel->stop();
            _sshTunnel.reset();
        }

        // MongoWorker "_worker" is not deleted here, because it is now owned by
        // another thread (call to moveToThread() made in MongoWorker constructor).
        // It will be deleted by this thread by means of "deleteLater()", which
        // is also specified in MongoWorker constructor.
    }

    void MongoServer::tryConnect() 
    {
        _bus->send(_worker, new EstablishConnectionRequest(this, _connectionType, _connSettings->uuid().toStdString()));
    }

    void MongoServer::tryRefresh() 
    {
        _bus->send(_worker, new EstablishConnectionRequest(this, ConnectionRefresh, _connSettings->uuid().toStdString()));
    }

    void MongoServer::tryRefreshReplicaSetConnection()
    {
        _bus->send(_worker, new EstablishConnectionRequest(this, ConnectionRefresh, _connSettings->uuid().toStdString()));
    }
    
    void MongoServer::tryRefreshReplicaSetFolder(bool expanded, bool showLoading /*= true*/)
    {
        if (!_connSettings->isReplicaSet())
            return;

        if (showLoading)
            _bus->publish(new ReplicaSetFolderLoading(this));

        _bus->send(_worker, new RefreshReplicaSetFolderRequest(this, expanded));
    }

    QStringList MongoServer::getDatabasesNames() const 
    {
        QStringList result;
        for (QList<MongoDatabase *>::const_iterator it = _databases.begin(); it != _databases.end(); ++it) {
            MongoDatabase *datab = *it;
            result.append(QtUtils::toQString(datab->name()));
        }
        return result;
    }

    void MongoServer::addDatabase(MongoDatabase *database) {
        _databases.append(database);
    }

    void MongoServer::createDatabase(const std::string &dbName) 
    {
        _bus->send(_worker, new CreateDatabaseRequest(this, dbName));
    }

    MongoDatabase *MongoServer::findDatabaseByName(const std::string &dbName) const 
    {
        for (QList<MongoDatabase *>::const_iterator it = _databases.begin(); it != _databases.end(); ++it) {
            MongoDatabase *datab = *it;
            if (datab->name() == dbName) {
                return datab;
            }
        }
        return NULL;
    }

    void MongoServer::dropDatabase(const std::string &dbName) {
        _bus->send(_worker, new DropDatabaseRequest(this, dbName));
    }

    void MongoServer::insertDocuments(const std::vector<bsoncxx::document::value> &objCont,
                                      const MongoNamespace &ns) {
        for (std::vector<bsoncxx::document::value>::const_iterator it = objCont.begin(); it != objCont.end(); it++) {
            insertDocument(it->view(), ns);
        }
    }

    void MongoServer::insertDocument(bsoncxx::document::view_or_value obj, const MongoNamespace &ns) {
        _bus->send(_worker, new InsertDocumentRequest(this, obj, ns));
    }

    void MongoServer::saveDocuments(const std::vector<bsoncxx::document::value> &objCont, const MongoNamespace &ns) {
        for (std::vector<bsoncxx::document::value>::const_iterator it = objCont.begin(); it != objCont.end(); it++) {
            saveDocument(it->view(), ns);
        }
    }

    void MongoServer::saveDocument(bsoncxx::document::view_or_value obj, const MongoNamespace &ns) {
        _bus->send(_worker, new InsertDocumentRequest(this, obj, ns, true));
    }

    void MongoServer::removeDocuments(bsoncxx::document::view_or_value query, const MongoNamespace &ns, 
                                      RemoveDocumentCount removeCount, int index) 
    {
        _bus->send(_worker, new RemoveDocumentRequest(this, query, ns, removeCount, index));
    }

    void MongoServer::loadDatabases() 
    {
        if (_sshTunnel && !_sshTunnel->isRunning() &&
            !_sshTunnel->isHaltedByFatalError() && !_sshTunnel->isStopped()) {
            _pendingNetworkRefresh = true;
            if (_sshTunnel->onNetworkResumed()) {
                _bus->publish(new MongoServerLoadingDatabasesEvent(this));
                return;
            }
            _pendingNetworkRefresh = false;
        }

        _bus->publish(new MongoServerLoadingDatabasesEvent(this));
        if (_connSettings->isReplicaSet()) {
            tryRefreshReplicaSetConnection();
        }
        else {  // single server
            _bus->send(_worker, new LoadDatabaseNamesRequest(this));
        }
    }

    void MongoServer::attachSshTunnel(std::unique_ptr<SshTunnelWorker> tunnel)
    {
        if (_sshTunnel) {
            _sshTunnel->stop();
            _sshTunnel.reset();
        }
        _sshTunnel = std::move(tunnel);
        if (!_sshTunnel)
            return;

        connect(_sshTunnel.get(), &SshTunnelWorker::tunnelReconnected, this,
                [this](int localPort, bool portChanged) {
                    const int previousPort = _connSettings ? _connSettings->serverPort() : 0;
                    if (portChanged && localPort > 0) {
                        updateTunnelLocalPort(localPort);
                        if (previousPort > 0) {
                            if (_app)
                                _app->updateSecondaryShellsLocalPort(this, previousPort, localPort);
                            Q_EMIT tunnelLocalPortChanged(previousPort, localPort);
                        }
                    }

                    if (!_isExplorerServer)
                        return;

                    if (_pendingNetworkRefresh || portChanged || !_isConnected) {
                        _pendingNetworkRefresh = false;
                        loadDatabases();
                    } else {
                        tryRefresh();
                    }
                },
                Qt::QueuedConnection);

        connect(_sshTunnel.get(), &SshTunnelWorker::tunnelPermanentlyFailed, this,
                [this](int errorKind, const QString &errorMessage) {
                    if (!_isExplorerServer)
                        return;

                    _pendingNetworkRefresh = false;
                    _isConnected = false;
                    const std::string msg = errorMessage.toStdString();
                    LOG_MSG("SSH tunnel permanently disconnected: " + msg, RagoShell::LogSeverity::Error);
                    const auto reason = (errorKind == static_cast<int>(SshTunnelErrorKind::HostKey) ||
                                         errorKind == static_cast<int>(SshTunnelErrorKind::Auth))
                        ? ConnectionFailedEvent::SshConnection
                        : ConnectionFailedEvent::SshChannel;
                    if (_bus)
                        _bus->publish(new ConnectionFailedEvent(this, _handle, ConnectionRefresh, msg, reason));
                },
                Qt::QueuedConnection);
    }

    void MongoServer::recheckAfterNetworkChange()
    {
        if (_sshTunnel) {
            if (_sshTunnel->isHaltedByFatalError()) {
                _isConnected = false;
                if (_bus) {
                    _bus->publish(new ConnectionFailedEvent(this, _handle, ConnectionRefresh,
                                                            _sshTunnel->lastErrorMessage(),
                                                            ConnectionFailedEvent::SshConnection));
                }
                return;
            }
            const bool alreadyRunning = _sshTunnel->isRunning();
            _pendingNetworkRefresh = true;
            if (_sshTunnel->onNetworkResumed()) {
                if (alreadyRunning && _sshTunnel->isRunning()) {
                    _pendingNetworkRefresh = false;
                    loadDatabases();
                }
                return;
            }
            _pendingNetworkRefresh = false;
        }
        loadDatabases();
    }

    void MongoServer::updateTunnelLocalPort(int newPort)
    {
        if (newPort <= 0)
            return;
        if (_connSettings)
            _connSettings->setServerPort(newPort);
        if (_worker)
            _worker->updateLocalServerPort(newPort);
    }

    void MongoServer::clearDatabases() 
    {
        qDeleteAll(_databases);
        _databases.clear();
    }

    void MongoServer::handle(EstablishConnectionResponse *event) 
    {
        if (event->connectionType != ConnectionRefresh)
            _connectionType = event->connectionType;

        // In any case, replica set info must be updated, there might be reachable secondary(ies).
        // Also cached set name must be updated or cleared for failed connections.
        updateReplicaSetSettings(event);

        // --- Connection Failed
        if (event->isError()) {
            handleConnectionFailure(event);
            return;
        }

        // --- Connections Successful
        // Save various information after successful connection
        const ConnectionInfo &info = event->info;
        if (info._version > 0.0f)
            _version = info._version;
        if (!info._storageEngineType.empty())
            _storageEngineType = info._storageEngineType;
        _isConnected = true;

        // ConnectionRefresh is used just to update connection view (_version, _storageEngineType, _repPrimary etc..)
        // So we return here after updating(refreshing) information related to connection view.
        if (ConnectionRefresh == event->connectionType) {
            if (_connSettings->isReplicaSet()) {
                // If it is replica set connection, do not return yet.
                LOG_MSG("Replica set refreshed. [" + _connSettings->connectionName() + ']', 
                         RagoShell::LogSeverity::Info);
            }
            else {  // single server
                LOG_MSG("Server refreshed.", RagoShell::LogSeverity::Info);
                return;
            }
        }

        if (ConnectionSecondary != _connectionType) {
            clearDatabases();
            for (auto const& dbname : info._databases) {
                MongoDatabase *db  = new MongoDatabase(this, dbname);
                addDatabase(db);
            }
        }

        if (ConnectionPrimary == event->connectionType)
            LOG_MSG("Establish connection successful. Connection: " + _connSettings->connectionName(),
                     RagoShell::LogSeverity::Info);

        // Save connected db version if not saved before and if this is primary connection.
        QString const versionStr = QString::fromStdString(info._dbVersionStr);
        auto const& settingsManager = AppRegistry::instance().settingsManager();
        if (ConnectionPrimary == _connectionType && !versionStr.isEmpty() &&
            !settingsManager->dbVersionsConnected().contains(versionStr)) {
            settingsManager->addDbVersionConnected(versionStr);
            settingsManager->save();
        }

        if (_connSettings->isReplicaSet()) {
            _bus->publish(new ConnectionEstablishedEvent(this, event->connectionType, info));
            // In order to do first connection much faster, time consuming refresh 
            // "repSetMonitor->startOrContinueRefresh(). refreshAll()" is being requested after 
            // successful connection.
            if (ConnectionPrimary == event->connectionType)
                _bus->send(_worker, new RefreshReplicaSetFolderRequest(this, false));
        } else {
            _bus->publish(new ConnectionEstablishedEvent(this, event->connectionType, info));
        }

    }

    void MongoServer::handle(RefreshReplicaSetFolderResponse *event)
    {
        handleReplicaSetRefreshEvents(event->isError(), event->error(), event->replicaSet, event->expanded);
    }

    void MongoServer::handle(LoadDatabaseNamesResponse *event) 
    {
        if (event->isError()) {
            _bus->publish(new DatabaseListLoadedEvent(this, event->error()));
            return;
        }

        clearDatabases();

        for (auto const& dbname : event->databaseNames) 
            addDatabase(new MongoDatabase(this, dbname));

        _bus->publish(new DatabaseListLoadedEvent(this, _databases));
        LOG_MSG("Database list refreshed. Connection: " + _connSettings->connectionName(), 
                 RagoShell::LogSeverity::Info);
    }

    void MongoServer::handle(InsertDocumentResponse *event) 
    {
        if (event->isError()) {
            hideProgressBar();
            if (_connSettings->isReplicaSet()) {
                if (ConnectionPrimary == _connectionType) { // Insert document from explorer context menu
                    if (EventError::SetPrimaryUnreachable == event->error().errorCode()) {
                        auto refreshEvent = ReplicaSetRefreshed(this, event->error(), event->error().replicaSetInfo());
                        handle(&refreshEvent);
                    }
                }
                else {  // Insert document from tab results window (Notifier, OutputWindow widget)
                    _bus->publish(new InsertDocumentResponse(this, event->error()));
                }
            }
            genericEventErrorHandler(event, "Failed to insert document.", _bus, this);
        }
        else {
            _bus->publish(new InsertDocumentResponse(this, event->error()));
            LOG_MSG("Document inserted.", RagoShell::LogSeverity::Info);
        }
    }

    void MongoServer::handle(RemoveDocumentResponse *event) 
    {        
        if (event->removeCount == RemoveDocumentCount::MULTI && event->index > 0)
            return;

        std::string subStr;
        switch (event->removeCount) {
            case RemoveDocumentCount::ONE:    subStr = "document."; break;
            case RemoveDocumentCount::MULTI:  subStr = "documents."; break;
            case RemoveDocumentCount::ALL:    subStr = "all documents."; break;
            default:                          subStr = "(logic error)."; break;
        }

        if (event->isError()) {
            hideProgressBar();
            if (_connSettings->isReplicaSet() &&
                EventError::SetPrimaryUnreachable == event->error().errorCode()) {
                auto refreshEvent = ReplicaSetRefreshed(this, event->error(), event->error().replicaSetInfo());
                handle(&refreshEvent);
            }
            genericEventErrorHandler(event, "Failed to remove " + subStr, _bus, this);
        }
        else {  // success
            _bus->publish(new RemoveDocumentResponse(this, event->error(), event->removeCount, event->index));
            LOG_MSG("Removed " + subStr, RagoShell::LogSeverity::Info);
        }
    }

    void MongoServer::runWorkerThread() 
    {
        _worker = new MongoWorker(_connSettings->clone(),
                                  AppRegistry::instance().settingsManager()->startupScript(),
                                  AppRegistry::instance().settingsManager()->batchSize(),
                                  AppRegistry::instance().settingsManager()->mongoTimeoutSec(),
                                  AppRegistry::instance().settingsManager()->shellTimeoutSec());
    }

    void MongoServer::handle(CreateDatabaseResponse *event) 
    {
        if (event->isError()) {
            if (_connSettings->isReplicaSet() &&
                EventError::SetPrimaryUnreachable == event->error().errorCode()) {
                auto refreshEvent = ReplicaSetRefreshed(this, event->error(), event->error().replicaSetInfo());
                handle(&refreshEvent);
            }
            genericEventErrorHandler(event, "Failed to create database \'" + event->database + "\'.", _bus, this);
        }
        else {
            loadDatabases();
            LOG_MSG("Database \'" + event->database + "\' created.", RagoShell::LogSeverity::Info);
        }
    }

    void MongoServer::handle(DropDatabaseResponse *event) 
    {
        if (event->isError()) {
            if (_connSettings->isReplicaSet() &&
                EventError::SetPrimaryUnreachable == event->error().errorCode()) {
                auto refreshEvent = ReplicaSetRefreshed(this, event->error(), event->error().replicaSetInfo());
                handle(&refreshEvent);
            }
            genericEventErrorHandler(event, "Failed to drop database \'" + event->database + "\'.", _bus, this);
        }
        else {
            loadDatabases();
            LOG_MSG("Database \'" + event->database + "\' dropped.", RagoShell::LogSeverity::Info);
        }
    }

    void MongoServer::handle(ReplicaSetRefreshed *event) 
    {
        handleReplicaSetRefreshEvents(event->isError(), event->error(), event->replicaSet, false);
    }

    void MongoServer::changeWorkerShellTimeout(int newTimeout)
    {
        _worker->changeTimeout(newTimeout);
    }

    void MongoServer::handleReplicaSetRefreshEvents(bool isError, EventError eventError, 
                                                    ReplicaSet const& replicaSet, bool expanded)
    {
        if (isError) { // Primary is unreachable
            _replicaSetInfo.reset(new ReplicaSet(replicaSet));
            LOG_MSG("Replica set folder refreshed with error. " + eventError.errorMessage() +
                    ". Connection: " + _connSettings->connectionName(), RagoShell::LogSeverity::Error);
            _bus->publish(new ReplicaSetFolderRefreshed(this, eventError, true));
            return;
        }

        // Primary is reachable
        // Update replica set settings and mongo server _replicaSetInfo 
        _replicaSetInfo.reset(new ReplicaSet(replicaSet));
        _connSettings->setServerHost(_replicaSetInfo->primary.host());
        _connSettings->setServerPort(_replicaSetInfo->primary.port());
        _connSettings->replicaSetSettings()->setCachedSetName(
            _connSettings->replicaSetSettings()->setNameUserEntered().empty() ? _replicaSetInfo->setName : "");

        LOG_MSG("Replica set folder refreshed. Connection: " + _connSettings->connectionName(),
                 RagoShell::LogSeverity::Info);
        _bus->publish(new ReplicaSetFolderRefreshed(this, expanded));
    }

    void MongoServer::updateReplicaSetSettings(EstablishConnectionResponse* event)
    {
        if (!_connSettings->isReplicaSet())
            return;

        _replicaSetInfo.reset(new ReplicaSet(event->replicaSet));
        if (!_replicaSetInfo->primary.empty()) {
            _connSettings->setServerHost(_replicaSetInfo->primary.host());
            _connSettings->setServerPort(_replicaSetInfo->primary.port());
        }
        _connSettings->replicaSetSettings()->setCachedSetName(
            _connSettings->replicaSetSettings()->setNameUserEntered().empty() ? _replicaSetInfo->setName : "");

        if (ConnectionPrimary != _connectionType)
            return;

        if (_connSettings->replicaSetSettings()->setNameUserEntered().empty()) {
            // Cache replica set name for 2 times faster first connection 
            ConnectionSettings* originalConnSettings = AppRegistry::instance().settingsManager()
                ->getConnectionSettingsByUuid(event->info._uuid);
            if (originalConnSettings) {
                auto setName = event->isError() ? "" : _replicaSetInfo->setName;
                originalConnSettings->replicaSetSettings()->setCachedSetName(setName);
                AppRegistry::instance().settingsManager()->save();
                LOG_MSG("Replica set name cached as \"" + setName + "\".", RagoShell::LogSeverity::Info);
            }
            else
                LOG_MSG("Failed to cache the replica set name.", RagoShell::LogSeverity::Warning);
        }
        else { // User entered set name is not empty, clear cached set name just in case
            ConnectionSettings* originalConnSettings = AppRegistry::instance().settingsManager()
                ->getConnectionSettingsByUuid(event->info._uuid);
            if (originalConnSettings && !originalConnSettings->replicaSetSettings()->cachedSetName().empty()) {
                originalConnSettings->replicaSetSettings()->setCachedSetName("");
                AppRegistry::instance().settingsManager()->save();
                LOG_MSG("Replica set's cached set name cleared. Using user entered set name.", 
                        RagoShell::LogSeverity::Info);
            }
        }
    }

    void MongoServer::handleConnectionFailure(EstablishConnectionResponse* event)
    {
        _isConnected = false;

        std::stringstream ss("Unknown error");
        auto eventErrorReason = event->errorReason;

        if (_connSettings->isReplicaSet())
        {
            ss.clear();
            std::string server = "";
            if (_connSettings->replicaSetSettings()->members().size() > 0)
                server = "[" + _connSettings->replicaSetSettings()->members().front() + "]";

            if (event->error().errorCode() == EventError::ErrorCode::ServerHasDifferentMembers) {
                ss << "Cannot connect to replica set \"" << _connSettings->connectionName() << "\"" << server
                   << ". \n\nA primary with different host name [" << event->replicaSet.primary.toString() << 
                   "] found in server side. "
                   "Please double check if same host names and ports are used as in server's replica set"
                   " configuration. \nIf same set name is used for different replica sets, this configuration"
                   " is supported only on different instances of RagoShell. "
                   " Please open a new RagoShell instance for each replica set which has the same set name."
                   "\n\nReason:\n" << event->error().errorMessage();
            }
            else {
                ss << "Cannot connect to replica set \"" << _connSettings->connectionName() << "\"" << server
                   << ". \nSet's primary is unreachable.\n\nReason:\n" << event->error().errorMessage();
            }

            _bus->publish(new ConnectionFailedEvent(this, _handle, event->connectionType, ss.str(),
                ConnectionFailedEvent::MongoConnection));
            _app->fireConnectionFailedEvent(_handle, event->connectionType, ss.str(),
                ConnectionFailedEvent::MongoConnection);
            LOG_MSG("Establish connection failed. " + event->error().errorMessage() + ". Connection: "
                + _connSettings->connectionName(), RagoShell::LogSeverity::Error);
        }
        else    // single server 
        {
            if (EstablishConnectionResponse::ErrorReason::MongoSslConnection == eventErrorReason)
            {
                auto reason = ConnectionFailedEvent::SslConnection;
                ss.clear();
                ss << "Cannot connect to the MongoDB at " << connectionRecord()->getFullAddress()
                    << ".\n\nError:\n" << "TLS connection failure: " << event->error().errorMessage();
                _app->fireConnectionFailedEvent(_handle, event->connectionType, ss.str(), reason);
            }
            else
            {
                auto reason = (EstablishConnectionResponse::ErrorReason::MongoAuth == eventErrorReason) ?
                    ConnectionFailedEvent::MongoAuth : ConnectionFailedEvent::MongoConnection;
                ss.clear();
                ss << "Cannot connect to the MongoDB at " << connectionRecord()->getFullAddress()
                    << ".\n\nError:\n" << event->error().errorMessage();
                _app->fireConnectionFailedEvent(_handle, event->connectionType, ss.str(), reason);
            }

            // When connection cannot be established, we should cleanup this instance of MongoServer if it wasn't
            // shown in UI (i.e. it is not a Secondary connection that is used for shells tab)
            if (event->connectionType == ConnectionPrimary || event->connectionType == ConnectionTest) 
            {
                LOG_MSG("Establish connection failed. " + event->error().errorMessage() +
                        ". Connection: " + _connSettings->connectionName(), 
                        RagoShell::LogSeverity::Error);
                QPointer<MongoServer> guard(this);
                App *app = _app;
                QMetaObject::invokeMethod(app, [app, guard]() {
                    if (guard)
                        app->closeServer(guard);
                }, Qt::QueuedConnection);
            }
        }
    }

    void MongoServer::hideProgressBar() const
    {
        emit UiSignals::instance().hideProgressBarRequested();
    }

}   // namespace RagoShell
