#include "ragoshell-app/QmlBackend.h"

#include <cmath>
#include <functional>
#include <memory>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QKeySequence>
#include <QPointer>
#include <QFile>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QTextStream>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell-app/CloudService.h"
#include "ragoshell-app/QmlSingleton.h"
#include "ragoshell/core/EventBus.h"
#include "ragoshell/core/EventError.h"
#include "ragoshell/core/domain/App.h"
#include "ragoshell/core/domain/HostAndPort.h"
#include "ragoshell/core/domain/MongoServer.h"
#include "ragoshell/core/domain/MongoDatabase.h"
#include "ragoshell/core/domain/MongoCollection.h"
#include "ragoshell/core/domain/MongoCollectionInfo.h"
#include "ragoshell/core/domain/MongoNamespace.h"
#include "ragoshell/core/domain/MongoShell.h"
#include "ragoshell/core/events/MongoEvents.h"
#include "ragoshell/core/mongodb/MongoWorker.h"
#include "ragoshell/core/mongodb/MongoUriUtils.h"
#include "ragoshell/core/mongodb/ReplicaSet.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/settings/ReplicaSetSettings.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/utils/AppLog.h"
#include "ragoshell/core/utils/AppPaths.h"
#include "ragoshell/core/utils/HostKeyPrompter.h"
#include "ragoshell/core/utils/SchemaCatalog.h"
#include "ragoshell/core/utils/SchemaService.h"
#include "ragoshell/core/utils/LanguageCatalog.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/utils/CollectionTransfer.h"
#include "ragoshell/core/utils/Logger.h"
#include "ragoshell/core/utils/MongoJsonUtils.h"
#include "ragoshell/core/utils/QtUtils.h"
#include "ragoshell-app/ServerTreeModel.h"
#include "ragoshell-app/ServerMonitorController.h"
#include "ragoshell-app/ShellController.h"
#include "ragoshell-app/BackgroundMonitor.h"
#include "ragoshell-app/LocalNetworkPermission.h"
#include "ragoshell/core/utils/LocalNetwork.h"
#include "ragoshell-app/LogModel.h"
#include "ragoshell-app/ScriptDraftModel.h"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/types/bson_value/view.hpp>
#include <mongocxx/uri.hpp>

#include "ragoshell-app/QmlBackendInternal.h"

namespace RagoShell
{
    using namespace QmlBackendDetail;

    // ---- IndexRequestProxy -------------------------------------------------

    IndexRequestProxy::IndexRequestProxy(QmlBackend *backend, MongoServer *server,
                                         const QString &dbName, const QString &collName,
                                         Mode mode)
        : QObject(backend),
          _backend(backend),
          _server(server),
          _dbName(dbName),
          _collName(collName),
          _mode(mode)
    {
    }

    void IndexRequestProxy::handle(LoadCollectionIndexesResponse *event)
    {
        if (_backend && _server)
            _backend->onIndexesLoaded(_server, _dbName, _collName, event);
        deleteLater();
    }

    void IndexRequestProxy::handle(AddEditIndexResponse *event)
    {
        if (_backend && _server)
            _backend->onIndexSaved(_server, _dbName, _collName, event);
        // On success, MongoWorker immediately follows AddEditIndexResponse with
        // LoadCollectionIndexesResponse to the same sender — deleteLater() there.
        if (event->isError())
            deleteLater();
    }

    void IndexRequestProxy::handle(DropCollectionIndexResponse *event)
    {
        if (_backend && _server)
            _backend->onIndexDropped(_server, _dbName, _collName, event);
        deleteLater();
    }

    // ---- QmlBackend --------------------------------------------------------

    QmlBackend::QmlBackend(QObject *parent)
        : QObject(parent),
          _bus(AppRegistry::instance().bus()),
          _app(AppRegistry::instance().app()),
          _tree(new ServerTreeModel(this)),
          _shellModel(new QmlObjectListModel(this)),
          _monitorModel(new QmlObjectListModel(this)),
          _tabs(new TabModel(this)),
          _logModel(new LogModel(this))
    {
        _bus->subscribe(this, ConnectingEvent::Type);
        _bus->subscribe(this, ConnectionEstablishedEvent::Type);
        _bus->subscribe(this, ConnectionFailedEvent::Type);
        _bus->subscribe(this, DatabaseListLoadedEvent::Type);
        _bus->subscribe(this, MongoServerLoadingDatabasesEvent::Type);
        _bus->subscribe(this, ReplicaSetFolderLoading::Type);
        _bus->subscribe(this, ReplicaSetFolderRefreshed::Type);
        _bus->subscribe(this, MongoDatabaseCollectionListLoadedEvent::Type);
        _bus->subscribe(this, MongoDatabaseCollectionsLoadingEvent::Type);
        _bus->subscribe(this, MongoDatabaseFunctionsLoadedEvent::Type);
        _bus->subscribe(this, MongoDatabaseFunctionsLoadingEvent::Type);
        _bus->subscribe(this, MongoDatabaseUsersLoadedEvent::Type);
        _bus->subscribe(this, MongoDatabaseUsersLoadingEvent::Type);
        _bus->subscribe(this, OpeningShellEvent::Type);
        _bus->subscribe(this, OperationFailedEvent::Type);
        _bus->subscribe(this, LogEvent::Type);

        // Background schema analysis pauses on a metered network (when asked) or a phone that is
        // not in front; the monitor also notices that the network came back or the computer woke.
        _monitor = new BackgroundMonitor(this);
        connect(_monitor, &BackgroundMonitor::backgroundAllowedChanged, this, [](bool allowed) {
            if (SchemaService *service = SchemaService::instance())
                service->setBackgroundPaused(!allowed);
        });
        if (SchemaService *service = SchemaService::instance())
            service->setBackgroundPaused(!_monitor->backgroundAllowed());
        connect(_monitor, &BackgroundMonitor::connectionsMayHaveBroken, this, [this]() {
            // Give the network a moment to settle, then look at every open connection.
            QTimer::singleShot(3000, this, &QmlBackend::recheckConnections);
        });

        // SSH tunnels run on worker threads; their host key questions come here and go to the dialog.
        HostKeyPrompter::instance().setHandler([this](const HostKeyQuestion &q) {
            QMetaObject::invokeMethod(this, [this, q]() {
                emit hostKeyRequested(q.id, q.host, q.port, q.keyType, q.fingerprint, q.changed);
            }, Qt::QueuedConnection);
        });

        // A failed connection must not open recovered scripts when the server is
        // connected by hand later.
        connect(this, &QmlBackend::connectionError, this, [this](const QString &uuid, const QString &) {
            _pendingRecoveries.remove(uuid);
        });

        connect(_tree, &ServerTreeModel::indexesLoadRequested,
                this, &QmlBackend::loadCollectionIndexes);

        // Background schema collection (main creates the service and catalog first).
        if (SchemaService::instance() && SchemaCatalog::instance()) {
            _schemaCollector = new SchemaCollector(SchemaService::instance(), SchemaCatalog::instance(), {}, this);
            _tree->setSchemaSources(SchemaCatalog::instance(), _schemaCollector);
        }
    }

    bool QmlBackend::schemaOptions(const QString &uuid, SchemaCollector::Options *options,
                                   std::shared_ptr<const ConnectionSettings> *connection) const
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || !server->connectionRecord())
            return false;
        SettingsManager *settings = AppRegistry::instance().settingsManager();
        const QString own = QString::fromStdString(server->connectionRecord()->schemaCollectionMode());
        options->mode = !own.isEmpty() ? SettingsManager::schemaCollectionModeFromId(own)
                        : settings ? settings->schemaCollectionMode()
                                   : SettingsManager::SchemaCollectionMode::OnUse;
        if (settings) {
            options->sampleSize = settings->schemaBackgroundSampleSize();
            options->maxCollections = settings->schemaBackgroundMaxCollections();
            options->valuesIncluded = settings->aiIncludeSampleValues();
            options->uuidEncoding = settings->uuidEncoding();
            options->timeZone = settings->timeZone();
        }
        options->serverVersion = serverVersionString(uuid);
        *connection = std::shared_ptr<const ConnectionSettings>(server->connectionRecord()->clone());
        return true;
    }

    void QmlBackend::schemaDatabaseInUse(const QString &uuid, const QString &dbName)
    {
        SchemaCollector::Options options;
        std::shared_ptr<const ConnectionSettings> connection;
        if (_schemaCollector && !dbName.isEmpty() && schemaOptions(uuid, &options, &connection))
            _schemaCollector->databaseInUse(uuid, connection, dbName, options);
    }

    void QmlBackend::schemaCollectionInUse(const QString &uuid, const QString &dbName, const QString &collName)
    {
        SchemaCollector::Options options;
        std::shared_ptr<const ConnectionSettings> connection;
        if (_schemaCollector && !dbName.isEmpty() && !collName.isEmpty() && schemaOptions(uuid, &options, &connection))
            _schemaCollector->collectionInUse(uuid, connection, dbName, collName, options);
    }

    void QmlBackend::forgetSchema(const QString &uuid)
    {
        if (uuid.isEmpty())
            return;
        if (SchemaService *service = SchemaService::instance())
            service->cancel(uuid);
        if (_schemaCollector)
            _schemaCollector->disconnected(uuid);   // listed again when a database is used
        if (SchemaCatalog *catalog = SchemaCatalog::instance()) {
            if (!catalog->clear(uuid))
                LOG_MSG(QStringLiteral("Could not remove the schema catalog of connection %1").arg(uuid),
                        LogSeverity::Warning);
        }
        _tree->refreshSchema(uuid);
    }

    void QmlBackend::analyzeDatabaseSchema(const QString &uuid, const QString &dbName)
    {
        SchemaCollector::Options options;
        std::shared_ptr<const ConnectionSettings> connection;
        if (_schemaCollector && !dbName.isEmpty() && schemaOptions(uuid, &options, &connection))
            _schemaCollector->analyzeDatabase(uuid, connection, dbName, options);
    }

    QmlBackend::~QmlBackend()
    {
        HostKeyPrompter::instance().setHandler(nullptr);
        _transferCancel.store(true);
        if (_transferThread && _transferThread->joinable())
            _transferThread->join();
        _bus->unsubscibe(this);
    }


    void QmlBackend::retranslate()
    {
        // The status line is text in the old language; it shows "Ready" again.
        setStatus(QString());
        _tree->retranslate();
        _tabs->retranslate();
    }

    void QmlBackend::setStatus(const QString &text, bool databaseList)
    {
        _statusIsDatabaseList = databaseList;
        if (_statusText == text)
            return;
        _statusText = text;
        emit statusTextChanged();
    }

    void QmlBackend::setDatabaseListStatus(const QString &serverName, int count)
    {
        setStatus(tr("%1: %n database(s)", "", count).arg(iso(serverName)), true);
    }

    MongoServer *QmlBackend::findServerByUuid(const QString &uuid) const
    {
        return _serverUuids.key(uuid, nullptr);
    }

    std::unique_ptr<ConnectionSettings> QmlBackend::cloneConnection(const QString &uuid) const
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || !server->connectionRecord())
            return nullptr;
        return std::unique_ptr<ConnectionSettings>(server->connectionRecord()->clone());
    }

    MongoDatabase *QmlBackend::findDatabaseBySender(QObject *sender) const
    {
        for (MongoServer *server : _serverUuids.keys()) {
            for (MongoDatabase *db : server->databases()) {
                if (db == sender)
                    return db;
            }
        }
        return nullptr;
    }

    void QmlBackend::connectToServer(const QString &uuid)
    {
        if (MongoServer *existing = findServerByUuid(uuid)) {
            emit _tree->serverPopulated(_tree->serverIndex(existing));
            return;
        }
        auto *settings = AppRegistry::instance().settingsManager();
        ConnectionSettings *conn = settings->getConnectionSettingsByUuid(uuid);
        if (!conn) {
            emit connectionError(uuid, tr("Connection settings not found"));
            return;
        }
        setStatus(tr("Connecting to %1...").arg(iso(QtUtils::toQString(conn->getReadableName()))));
        whenLocalNetworkAllowed(networkHosts(*conn), [self = QPointer<QmlBackend>(this), uuid] {
            if (!self)
                return;
            auto *settings = AppRegistry::instance().settingsManager();
            if (ConnectionSettings *current = settings->getConnectionSettingsByUuid(uuid)) {
                std::unique_ptr<ConnectionSettings> clone(current->clone());
                // Hosted channel (docs/HostedGateway.md §5): the hello
                // token is the cached entitlement+jwt — injected only into
                // this in-memory clone, never written to settings.
                if (clone->transportKind() == "hosted") {
                    const QString jwt =
                        QmlSingletonInstance<CloudService>::get()->entitlementJwt();
                    if (jwt.isEmpty()) {
                        emit self->connectionError(
                            uuid, tr("Sign in with a Pro plan to use the cloud gateway."));
                        return;
                    }
                    clone->setTransportToken(jwt.toStdString());
                }
                self->_app->openServer(clone.release(), ConnectionPrimary);
            }
        });
    }

    QVariantList QmlBackend::explorerTargets() const
    {
        QVariantList result;
        QHash<QString, QString> serverNames;
        auto *settings = AppRegistry::instance().settingsManager();
        for (ConnectionSettings *conn : settings->connections()) {
            const QString name = QtUtils::toQString(conn->getReadableName());
            serverNames.insert(conn->uuid(), name);
            result.append(QVariantMap{
                {QStringLiteral("kind"), QStringLiteral("connection")},
                {QStringLiteral("title"), name},
                {QStringLiteral("subtitle"), QtUtils::toQString(conn->getFullAddress())},
                {QStringLiteral("uuid"), conn->uuid()},
                {QStringLiteral("db"), QString()},
                {QStringLiteral("collection"), QString()},
                {QStringLiteral("connected"), isServerConnected(conn->uuid())}
            });
        }

        // Databases and collections below connected servers; indexes are not searched.
        std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
            const int rows = _tree->rowCount(parent);
            for (int r = 0; r < rows; ++r) {
                const QModelIndex index = _tree->index(r, 0, parent);
                const QString type = _tree->data(index, ServerTreeModel::TypeRole).toString();
                const QString uuid = _tree->data(index, ServerTreeModel::ServerUuidRole).toString();
                const QString db = _tree->data(index, ServerTreeModel::DbNameRole).toString();
                const QString raw = _tree->data(index, ServerTreeModel::RawNameRole).toString();
                if (type == QLatin1String("database")) {
                    result.append(QVariantMap{
                        {QStringLiteral("kind"), type}, {QStringLiteral("title"), raw},
                        {QStringLiteral("subtitle"), serverNames.value(uuid)}, {QStringLiteral("uuid"), uuid},
                        {QStringLiteral("db"), raw}, {QStringLiteral("collection"), QString()},
                        {QStringLiteral("connected"), true}});
                } else if (type == QLatin1String("collection")) {
                    result.append(QVariantMap{
                        {QStringLiteral("kind"), type}, {QStringLiteral("title"), db + QLatin1Char('.') + raw},
                        {QStringLiteral("subtitle"), serverNames.value(uuid)}, {QStringLiteral("uuid"), uuid},
                        {QStringLiteral("db"), db}, {QStringLiteral("collection"), raw},
                        {QStringLiteral("connected"), true}});
                    continue;
                }
                if (type == QLatin1String("server") || type == QLatin1String("database")
                    || type == QLatin1String("systemDbFolder")
                    || type == QLatin1String("systemCollFolder"))
                    walk(index);
            }
        };
        walk(QModelIndex());
        return result;
    }

    void QmlBackend::disconnectServer(const QString &uuid, bool closeOpenShells)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server)
            return;
        const QString serverName = server->connectionRecord()
            ? QtUtils::toQString(server->connectionRecord()->getReadableName())
            : QString();

        // Close any live Server Monitor tabs bound to this server or its RS members
        const QString memberPrefix = uuid + QStringLiteral("@");
        for (int i = _monitors.size() - 1; i >= 0; --i) {
            auto *mon = qobject_cast<ServerMonitorController *>(
                _monitors.at(i).value<QObject *>());
            if (mon && (mon->serverUuid() == uuid || mon->serverUuid().startsWith(memberPrefix)))
                closeMonitor(i);
        }

        if (closeOpenShells) {
            for (int i = _shells.size() - 1; i >= 0; --i) {
                auto *sh = qobject_cast<ShellController *>(
                    _shells.at(i).value<QObject *>());
                if (sh && sh->serverUuid() == uuid)
                    closeShell(i);
            }
        }

        // Queued and running schema samples of this server are not wanted any more.
        if (SchemaService *schema = SchemaService::instance())
            schema->cancel(uuid);
        if (_schemaCollector)
            _schemaCollector->disconnected(uuid);

        const QString userCachePrefix = uuid + QLatin1Char('/');
        for (auto it = _databaseUsersCache.begin(); it != _databaseUsersCache.end(); ) {
            if (it.key().startsWith(userCachePrefix))
                it = _databaseUsersCache.erase(it);
            else
                ++it;
        }
        _serverVersionStrings.remove(server);
        _serverUuids.remove(server);
        emit connectedServersChanged();
        _tree->removeServerNode(server);
        _app->closeServer(server);
        if (_serverUuids.isEmpty())
            setStatus(tr("Disconnected"));
        else
            setStatus(tr("Disconnected from %1").arg(iso(serverName)));
    }

    int QmlBackend::openTabsCountForServer(const QString &uuid) const
    {
        if (uuid.isEmpty())
            return 0;
        int count = 0;
        const QString memberPrefix = uuid + QStringLiteral("@");
        for (const QVariant &v : _shells) {
            if (auto *sh = qobject_cast<ShellController *>(v.value<QObject *>())) {
                if (sh->serverUuid() == uuid)
                    ++count;
            }
        }
        for (const QVariant &v : _monitors) {
            if (auto *mon = qobject_cast<ServerMonitorController *>(v.value<QObject *>())) {
                if (mon->serverUuid() == uuid || mon->serverUuid().startsWith(memberPrefix))
                    ++count;
            }
        }
        return count;
    }

    int QmlBackend::dirtyShellsCountForServer(const QString &uuid) const
    {
        if (uuid.isEmpty())
            return 0;
        int count = 0;
        for (const QVariant &v : _shells) {
            if (auto *sh = qobject_cast<ShellController *>(v.value<QObject *>())) {
                if (sh->serverUuid() == uuid && sh->dirty())
                    ++count;
            }
        }
        return count;
    }

    void QmlBackend::refreshServer(const QString &uuid)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server)
            return;
        server->loadDatabases();
    }

    void QmlBackend::refreshReplicaSetFolder(const QString &uuid)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || !server->connectionRecord()->isReplicaSet())
            return;
        server->tryRefreshReplicaSetFolder(true);
    }

    void QmlBackend::replicaSetStatus(const QString &uuid)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || !server->connectionRecord()->isReplicaSet())
            return;

        if (server->replicaSetInfo() && !server->replicaSetInfo()->primary.empty()) {
            openShellWithScript(uuid, QString(), QStringLiteral("rs.status()"), true);
            return;
        }

        // Primary unreachable: try running rs.status() on a reachable secondary member
        HostAndPort onlineMember;
        if (server->replicaSetInfo()) {
            for (const auto &member : server->replicaSetInfo()->membersAndHealths) {
                if (member.second) {
                    onlineMember = HostAndPort(member.first);
                    break;
                }
            }
        }

        if (onlineMember.empty()) {
            emit connectionError(uuid, tr("Unable to find a reachable replica set member to run rs.status()."));
            return;
        }

        openMemberShell(uuid, QString::fromStdString(onlineMember.toString()),
                        QStringLiteral("rs.status()"), true);
    }

    void QmlBackend::openMemberShell(const QString &uuid,
                                     const QString &memberAddress,
                                     const QString &script,
                                     bool execute)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || memberAddress.isEmpty())
            return;

        HostAndPort hp(memberAddress.toStdString());
        if (server->connectionRecord()->hasTransport()) {
            emit connectionError(uuid, tr("Shell tabs are not yet supported over "
                                        "gateway/demo connection channels."));
            return;
        }
        std::unique_ptr<ConnectionSettings> memberConn(server->connectionRecord()->clone());
        memberConn->setConnectionName(
            hp.toString() + " [member of " + memberConn->connectionName() + "]");
        memberConn->setServerHost(hp.host());
        memberConn->setServerPort(hp.port());
        memberConn->setReplicaSet(false);
        memberConn->replicaSetSettings()->deleteAllMembers();

        QString title = QString::fromStdString(hp.toString());
        ScriptInfo info(script, execute && !script.isEmpty(), std::string(),
                        CursorPosition(), title);
        _app->openShell(server, memberConn.get(), info);
    }

    void QmlBackend::openMemberDirectConnection(const QString &uuid,
                                                const QString &memberAddress)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || memberAddress.isEmpty())
            return;

        HostAndPort hp(memberAddress.toStdString());
        ConnectionSettings *memberConn = server->connectionRecord()->clone();
        memberConn->setUuid(QUuid::createUuid().toString());
        memberConn->setConnectionName(
            hp.toString() + " [member of " + memberConn->connectionName() + "]");
        memberConn->setServerHost(hp.host());
        memberConn->setServerPort(hp.port());
        memberConn->setReplicaSet(false);
        memberConn->replicaSetSettings()->deleteAllMembers();

        setStatus(tr("Connecting to %1...").arg(iso(QString::fromStdString(hp.toString()))));
        _app->openServer(memberConn, ConnectionPrimary);
    }

    void QmlBackend::openShell(const QString &uuid, const QString &dbName,
                               const QString &collection)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server) {
            emit connectionError(uuid, tr("Server is not connected"));
            return;
        }

        QString script;
        bool execute = false;
        if (!collection.isEmpty()) {
            script = detail::buildCollectionQuery(
                QtUtils::toStdString(collection), QStringLiteral("find({})"));
            execute = true;
        }

        if (server->connectionRecord()->hasTransport()) {
            emit connectionError(uuid, tr("Shell tabs are not yet supported over "
                                        "gateway/demo connection channels."));
            return;
        }

        std::unique_ptr<ConnectionSettings> shellConn(server->connectionRecord()->clone());
        if (!dbName.isEmpty())
            shellConn->setDefaultDatabase(QtUtils::toStdString(dbName));

        QString title = !collection.isEmpty()
            ? collection
            : (!dbName.isEmpty()
                ? dbName
                : QtUtils::toQString(server->connectionRecord()->getReadableName()));
        if (title.isEmpty())
            title = tr("Shell");

        ScriptInfo info(script, execute, QtUtils::toStdString(dbName),
                        CursorPosition(), title);
        _app->openShell(server, shellConn.get(), info);
    }

    void QmlBackend::openShellWithScript(const QString &uuid,
                                         const QString &dbName,
                                         const QString &script,
                                         bool execute)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server) {
            emit connectionError(uuid, tr("Server is not connected"));
            return;
        }

        if (server->connectionRecord()->hasTransport()) {
            emit connectionError(uuid, tr("Shell tabs are not yet supported over "
                                        "gateway/demo connection channels."));
            return;
        }

        std::unique_ptr<ConnectionSettings> shellConn(server->connectionRecord()->clone());
        if (!dbName.isEmpty())
            shellConn->setDefaultDatabase(QtUtils::toStdString(dbName));

        QString title = dbName.isEmpty()
            ? QtUtils::toQString(server->connectionRecord()->getReadableName())
            : dbName;
        if (title.isEmpty())
            title = tr("Shell");

        ScriptInfo info(script, execute, QtUtils::toStdString(dbName),
                        CursorPosition(), title);
        _app->openShell(server, shellConn.get(), info);
    }

    void QmlBackend::closeShell(int index)
    {
        if (index < 0 || index >= _shells.size())
            return;
        auto *controller = qobject_cast<ShellController *>(
            _shells.at(index).value<QObject *>());
        _shells.removeAt(index);
        _shellModel->removeAt(index);
        _tabs->removeObject(controller);
        emit shellsChanged();
        if (controller) {
            // The tab is closed on purpose: its unsaved text is not a draft any more.
            if (ScriptDraftModel *drafts = ScriptDraftModel::instance())
                drafts->remove(controller->draftId());
            // Detach first: closeShell() destroys the MongoShell, while the
            // controller lives on until deleteLater() and must not touch it.
            MongoShell *shell = controller->shell();
            controller->detachShell();
            _app->closeShell(shell);
            controller->deleteLater();
        }
    }

    void QmlBackend::trimMemory()
    {
        if (SchemaCatalog *catalog = SchemaCatalog::instance())
            catalog->dropMemoryCache();
        LOG_MSG(QStringLiteral("Memory trimmed"), LogSeverity::Info, false);
    }

    bool QmlBackend::autoRefreshing(const QString &uuid) const
    {
        return _autoRefreshUntilMs.value(uuid, 0) > QDateTime::currentMSecsSinceEpoch();
    }

    void QmlBackend::recheckConnections()
    {
        if (_serverUuids.isEmpty())
            return;
        const qint64 until = QDateTime::currentMSecsSinceEpoch() + 20000;
        setStatus(tr("The network changed. Checking the open connections…"));
        const QList<QString> uuids = _serverUuids.values();
        for (const QString &uuid : uuids) {
            _autoRefreshUntilMs.insert(uuid, until);
            if (MongoServer *server = findServerByUuid(uuid))
                server->recheckAfterNetworkChange();
        }
    }

    void QmlBackend::answerHostKey(int id, bool trust)
    {
        HostKeyPrompter::instance().answer(id, trust);
    }

    void QmlBackend::discardOpenDrafts()
    {
        ScriptDraftModel *drafts = ScriptDraftModel::instance();
        if (!drafts)
            return;
        for (const QVariant &v : std::as_const(_shells)) {
            if (auto *controller = qobject_cast<ShellController *>(v.value<QObject *>()))
                drafts->remove(controller->draftId());
        }
    }

    void QmlBackend::recoverDraft(const QString &draftId)
    {
        ScriptDraftModel *drafts = ScriptDraftModel::instance();
        ScriptDraft draft;
        if (!drafts || !drafts->find(draftId, &draft))
            return;

        if (MongoServer *server = findServerByUuid(draft.serverUuid)) {
            openRecoveredDraft(server, draft);
            return;
        }

        auto *settings = AppRegistry::instance().settingsManager();
        if (draft.serverUuid.isEmpty() || !settings->getConnectionSettingsByUuid(draft.serverUuid)) {
            emit connectionError(draft.serverUuid,
                                 tr("The connection \"%1\" of this script no longer exists. "
                                    "Copy the script from the list instead.").arg(draft.serverName));
            return;
        }

        // Connect once, even when several drafts of this server are chosen meanwhile.
        const bool connecting = _pendingRecoveries.contains(draft.serverUuid);
        if (!_pendingRecoveries.contains(draft.serverUuid, draftId))
            _pendingRecoveries.insert(draft.serverUuid, draftId);
        if (!connecting)
            connectToServer(draft.serverUuid);
    }

    void QmlBackend::openRecoveredDraft(MongoServer *server, const ScriptDraft &draft)
    {
        std::unique_ptr<ConnectionSettings> shellConn(server->connectionRecord()->clone());
        if (!draft.database.isEmpty())
            shellConn->setDefaultDatabase(QtUtils::toStdString(draft.database));

        QString title = draft.title;
        if (title.isEmpty())
            title = draft.database.isEmpty() ? tr("Shell") : draft.database;

        ScriptInfo info(draft.script, false, QtUtils::toStdString(draft.database),
                        CursorPosition(), title, draft.filePath, draft.id);
        _app->openShell(server, shellConn.get(), info);
    }

    void QmlBackend::openPendingRecoveries(const QString &uuid)
    {
        ScriptDraftModel *drafts = ScriptDraftModel::instance();
        MongoServer *server = findServerByUuid(uuid);
        if (!drafts || !server)
            return;
        // QMultiHash returns the newest insertion first; open them in the order they were chosen.
        const QStringList ids = _pendingRecoveries.values(uuid);
        _pendingRecoveries.remove(uuid);
        for (auto it = ids.crbegin(); it != ids.crend(); ++it) {
            ScriptDraft draft;
            if (drafts->find(*it, &draft))
                openRecoveredDraft(server, draft);
        }
    }

    void QmlBackend::closeTabObject(QObject *object)
    {
        if (!object) {
            _tabs->setWelcomeOpen(false);
            return;
        }
        for (int i = 0; i < _shells.size(); ++i) {
            if (_shells.at(i).value<QObject *>() == object) {
                closeShell(i);
                return;
            }
        }
        for (int i = 0; i < _monitors.size(); ++i) {
            if (_monitors.at(i).value<QObject *>() == object) {
                closeMonitor(i);
                return;
            }
        }
    }

    void QmlBackend::duplicateShell(int index, const QString &currentScript)
    {
        if (index < 0 || index >= _shells.size())
            return;
        auto *controller = qobject_cast<ShellController *>(
            _shells.at(index).value<QObject *>());
        if (!controller)
            return;

        const QString uuid = controller->serverUuid();
        const QString db = controller->activeDatabase();
        const QString script = !currentScript.isEmpty() ? currentScript : controller->script();
        openShellWithScript(uuid, db, script, false);
    }

    void QmlBackend::closeOtherShells(int keepIndex)
    {
        if (keepIndex < 0 || keepIndex >= _shells.size())
            return;
        for (int i = _shells.size() - 1; i >= 0; --i) {
            if (i != keepIndex)
                closeShell(i);
        }
    }

    void QmlBackend::closeShellsToRight(int fromIndex)
    {
        if (fromIndex < 0 || fromIndex >= _shells.size() - 1)
            return;
        for (int i = _shells.size() - 1; i > fromIndex; --i)
            closeShell(i);
    }

}
