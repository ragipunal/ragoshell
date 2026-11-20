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

    void QmlBackend::handle(OpeningShellEvent *event)
    {
        QString title = event->shell->title();
        if (title.isEmpty())
            title = tr("Shell");
        auto *controller = new ShellController(event->shell, title, this);
        // A shell working in a database puts it in use for the schema collector.
        connect(controller, &ShellController::activeDatabaseChanged, this, [this, controller]() {
            schemaDatabaseInUse(controller->serverUuid(), controller->activeDatabase());
        });
        schemaDatabaseInUse(controller->serverUuid(), controller->activeDatabase());
        connect(controller, &ShellController::collectionReferenced, this, [this, controller](const QString &coll) {
            schemaCollectionInUse(controller->serverUuid(), controller->activeDatabase(), coll);
        });
        _shells.append(QVariant::fromValue<QObject *>(controller));
        _shellModel->append(controller);
        _tabs->addTab(QStringLiteral("shell"), controller, true);
        emit shellsChanged();
        emit shellOpened(controller);
    }


    // ---- EventBus handlers -------------------------------------------------

    void QmlBackend::handle(ConnectingEvent *)
    {
        _connecting = true;
        emit connectingChanged();
    }

    void QmlBackend::handle(ConnectionEstablishedEvent *event)
    {
        _connecting = false;
        emit connectingChanged();

        MongoServer *server = event->server;
        if (!server)
            return;

        ConnectionSettings *conn = server->connectionRecord();

        if (event->connectionType == ConnectionTest) {
            QVariantList steps;
            auto addStep = [&steps](const QString &label, const QString &status, const QString &detail) {
                steps.append(QVariantMap{
                    {QStringLiteral("label"), label},
                    {QStringLiteral("status"), status},
                    {QStringLiteral("detail"), detail}
                });
            };
            addStep(tr("Target Endpoint"), QStringLiteral("ok"),
                    QtUtils::toQString(conn->getReadableName()));
            if (conn->sshSettings() && conn->sshSettings()->enabled()) {
                addStep(tr("SSH Tunnel"), QStringLiteral("ok"),
                        tr("Tunnel established (%1:%2)")
                            .arg(QtUtils::toQString(conn->sshSettings()->host()))
                            .arg(conn->sshSettings()->port()));
            }
            if (conn->sslSettings() && conn->sslSettings()->sslEnabled()) {
                addStep(tr("TLS / SSL"), QStringLiteral("ok"),
                        tr("TLS handshake succeeded"));
            }
            addStep(tr("MongoDB Handshake"), QStringLiteral("ok"),
                    QStringLiteral("MongoDB %1 (%2)")
                        .arg(QString::number(server->version()),
                             server->getStorageEngineType().empty()
                                 ? QStringLiteral("wiredTiger")
                                 : QtUtils::toQString(server->getStorageEngineType())));
            if (conn->hasEnabledPrimaryCredential()) {
                addStep(tr("Authentication"), QStringLiteral("ok"),
                        tr("Authorized as '%1' on '%2'")
                            .arg(QtUtils::toQString(conn->primaryCredential()->userName()),
                                 QtUtils::toQString(conn->primaryCredential()->databaseName())));
            }
            addStep(tr("Database Discovery"), QStringLiteral("ok"),
                    tr("%n accessible database(s)", "", static_cast<int>(server->databases().size())));
            emit testDiagnosticSteps(steps);

            emit testResult(true, tr("Connected to %1 (MongoDB %2)")
                .arg(QtUtils::toQString(conn->getReadableName()),
                     QString::number(server->version())));
            QPointer<MongoServer> guard(server);
            QMetaObject::invokeMethod(this, [this, guard]() {
                if (guard)
                    _app->closeServer(guard);
            }, Qt::QueuedConnection);
            return;
        }

        if (event->connectionType == ConnectionRefresh) {
            if (!event->connInfo._dbVersionStr.empty()) {
                _serverVersionStrings.insert(server, QtUtils::toQString(event->connInfo._dbVersionStr));
                _tree->setServerVersionString(server, _serverVersionStrings.value(server));
            }
            if (conn->isReplicaSet() && _tree->hasServer(server)) {
                _tree->populateReplicaSetServer(server);
                setDatabaseListStatus(QtUtils::toQString(conn->getReadableName()),
                                      static_cast<int>(server->databases().size()));
            }
            return;
        }

        if (event->connectionType != ConnectionPrimary)
            return;

        QString uuid = conn->uuid();
        if (uuid.isEmpty() || (_serverUuids.values().contains(uuid) && !_serverUuids.contains(server)))
            uuid = QUuid::createUuid().toString();

        _serverUuids.insert(server, uuid);
        connect(server, &MongoServer::tunnelLocalPortChanged, this,
                [this, uuid](int oldPort, int newPort) {
                    for (const QVariant &v : std::as_const(_monitors)) {
                        if (auto *mon = qobject_cast<ServerMonitorController *>(v.value<QObject *>())) {
                            if (mon->serverUuid() == uuid)
                                mon->updateLocalPortIfMatching(oldPort, newPort);
                        }
                    }
                });
        if (!event->connInfo._dbVersionStr.empty())
            _serverVersionStrings.insert(server, QtUtils::toQString(event->connInfo._dbVersionStr));
        emit connectedServersChanged();
        _tree->addServerNode(server, uuid, _serverVersionStrings.value(server));

        // Replica Set connections already populated server->databases() and replicaSetInfo()
        // inside MongoServer::handle(EstablishConnectionResponse*) — populate immediately!
        if (conn->isReplicaSet()) {
            _tree->populateReplicaSetServer(server);
            setDatabaseListStatus(QtUtils::toQString(conn->getReadableName()),
                                  static_cast<int>(server->databases().size()));
        } else {
            server->loadDatabases();
            setStatus(tr("Connected to %1").arg(iso(QtUtils::toQString(conn->getReadableName()))));
        }

        // Scripts recovered from drafts that waited for this connection.
        openPendingRecoveries(uuid);
    }

    void QmlBackend::handle(ConnectionFailedEvent *event)
    {
        if (event->connectionType == ConnectionRefresh) {
            // Replica Set refreshes emit both ConnectionFailedEvent(MongoServer*) and
            // App::fireConnectionFailedEvent(App*); single-server refreshes only emit App*.
            auto *server = qobject_cast<MongoServer *>(event->sender());
            if (!server) {
                for (auto it = _serverUuids.cbegin(); it != _serverUuids.cend(); ++it) {
                    if (it.key() && it.key()->handle() == event->serverHandle) {
                        if (!it.key()->connectionRecord()->isReplicaSet())
                            server = it.key();
                        break;
                    }
                }
            }
            if (!server)
                return;
            _tree->setLoading(server, false);
            if (server->connectionRecord()->isReplicaSet() && _tree->hasServer(server)) {
                _tree->updateReplicaSetFolder(server, /*primaryReachable=*/false, /*expandFolder=*/true);
            }
            const QString msg = QtUtils::toQString(event->message);
            setStatus(tr("Refresh failed: %1").arg(msg));
            emit connectionError(_serverUuids.value(server), msg);
            return;
        }

        // For non-refresh Replica Set failures, MongoServer publishes ConnectionFailedEvent(this)
        // followed immediately by App::fireConnectionFailedEvent(_app). Defer closing the
        // unattached server until MongoServer::handleConnectionFailure unwinds, and emit the UI
        // error once on the App event.
        if (auto *failedServer = qobject_cast<MongoServer *>(event->sender())) {
            if ((event->connectionType == ConnectionPrimary ||
                 event->connectionType == ConnectionTest) &&
                !_tree->hasServer(failedServer)) {
                QPointer<MongoServer> guard(failedServer);
                QMetaObject::invokeMethod(this, [this, guard]() {
                    if (guard && !_tree->hasServer(guard))
                        _app->closeServer(guard);
                }, Qt::QueuedConnection);
            }
            return;
        }

        _connecting = false;
        emit connectingChanged();

        QString msg = QtUtils::toQString(event->message);

        if (event->connectionType == ConnectionTest) {
            if (_testServerHandle == -1 || event->serverHandle == _testServerHandle) {
                ConnectionSettings temp(true);
                temp.fromVariant(_lastTestMap);
                QVariantList steps;
                auto addStep = [&steps](const QString &label, const QString &status, const QString &detail) {
                    steps.append(QVariantMap{
                        {QStringLiteral("label"), label},
                        {QStringLiteral("status"), status},
                        {QStringLiteral("detail"), detail}
                    });
                };
                // Classify on the driver's own reason (the text after the last
                // "Reason:"): the wrapper text always says "TLS tunnel failure"
                // for TLS connections, even when the host simply timed out.
                const int reasonAt = msg.lastIndexOf(QLatin1String("Reason:"));
                const QString cause = (reasonAt >= 0 ? msg.mid(reasonAt + 7) : msg).trimmed();
                const QString causeLine = cause.section(QLatin1Char('\n'), 0, 0).trimmed();
                auto causeHas = [&cause](const char *needle) {
                    return cause.contains(QLatin1String(needle), Qt::CaseInsensitive);
                };

                const bool isSshErr = msg.contains(QLatin1String("SSH"), Qt::CaseInsensitive);
                const bool isTlsErr = !isSshErr &&
                                      (causeHas("TLS") || causeHas("SSL") || causeHas("certificate"));
                const bool isAuthErr = !isSshErr && !isTlsErr &&
                                       (causeHas("auth") || causeHas("SASL") || causeHas("SCRAM"));
                const bool isTimeout = causeHas("timeout") || causeHas("timed out");
                const bool isRefused = causeHas("connection refused");
                const bool isResolve = causeHas("resolve") || causeHas("no such host") ||
                                       causeHas("not known") || causeHas("host not found");
                const bool isNetErr = !isSshErr && !isTlsErr && !isAuthErr &&
                                      (isTimeout || isRefused || isResolve ||
                                       causeHas("no suitable servers") || causeHas("unreachable"));

                QString netHint;
                if (isResolve)
                    netHint = tr("Host name could not be resolved — check the address, DNS or VPN");
                else if (isRefused)
                    netHint = tr("Connection refused — is MongoDB listening on that port?");
                else if (isTimeout)
                    netHint = tr("Timed out reaching the server — check network, firewall or VPN");
                else
                    netHint = tr("Server is not reachable — check network, firewall or VPN");

                addStep(tr("Target Endpoint"),
                        isNetErr ? QStringLiteral("error") : QStringLiteral("ok"),
                        isNetErr ? netHint : QtUtils::toQString(temp.getFullAddress()));

                const QString notReached = tr("Not reached");
                const bool stopped = isNetErr || isSshErr;
                if (temp.sshSettings() && temp.sshSettings()->enabled()) {
                    addStep(tr("SSH Tunnel"),
                            isSshErr ? QStringLiteral("error") : QStringLiteral("ok"),
                            isSshErr ? causeLine : tr("Checked"));
                }
                if (temp.sslSettings() && temp.sslSettings()->sslEnabled()) {
                    addStep(tr("TLS / SSL"),
                            stopped ? QStringLiteral("skipped")
                                    : (isTlsErr ? QStringLiteral("error") : QStringLiteral("ok")),
                            stopped ? notReached
                                    : (isTlsErr ? causeLine : tr("Checked")));
                }
                const bool handshakeErr = !stopped && !isTlsErr && !isAuthErr;
                addStep(tr("MongoDB Handshake"),
                        handshakeErr ? QStringLiteral("error")
                                     : (isAuthErr ? QStringLiteral("ok") : QStringLiteral("skipped")),
                        handshakeErr ? causeLine
                                     : (isAuthErr ? tr("Reachable") : notReached));
                if (temp.hasEnabledPrimaryCredential() || isAuthErr) {
                    addStep(tr("Authentication"),
                            isAuthErr ? QStringLiteral("error") : QStringLiteral("skipped"),
                            isAuthErr ? causeLine : notReached);
                }
                emit testDiagnosticSteps(steps);
                emit testResult(false, msg);
            }
            return;
        }

        setStatus(tr("Connection failed: %1").arg(msg));
        QString failedUuid;
        for (auto it = _serverUuids.cbegin(); it != _serverUuids.cend(); ++it) {
            if (it.key() && it.key()->handle() == event->serverHandle) {
                failedUuid = it.value();
                break;
            }
        }
        emit connectionError(failedUuid, msg);
    }

    void QmlBackend::handle(MongoServerLoadingDatabasesEvent *event)
    {
        if (auto *server = qobject_cast<MongoServer *>(event->sender()))
            _tree->setLoading(server, true);
    }

    void QmlBackend::handle(ReplicaSetFolderLoading *event)
    {
        if (auto *server = qobject_cast<MongoServer *>(event->sender()))
            _tree->setReplicaSetFolderLoading(server, true);
    }

    void QmlBackend::handle(ReplicaSetFolderRefreshed *event)
    {
        auto *server = qobject_cast<MongoServer *>(event->sender());
        if (!server || !_tree->hasServer(server))
            return;

        if (event->isError()) {
            _tree->updateReplicaSetFolder(server, /*primaryReachable=*/false, event->expanded);
            const QString errMsg = QtUtils::toQString(event->error().errorMessage());
            setStatus(tr("Replica set primary unreachable: %1").arg(errMsg));
            if (event->error().showErrorWindow())
                emit connectionError(_serverUuids.value(server), errMsg);
            return;
        }

        _tree->updateReplicaSetFolder(server, /*primaryReachable=*/true, event->expanded);
    }

    void QmlBackend::handle(DatabaseListLoadedEvent *event)
    {
        auto *server = qobject_cast<MongoServer *>(event->sender());
        if (!server || !_tree->hasServer(server))
            return;
        if (event->isError()) {
            _tree->setLoading(server, false);
            setStatus(tr("Failed to load databases: %1")
                          .arg(QtUtils::toQString(event->error().errorMessage())));
            return;
        }
        _tree->setServerDatabases(server, event->list);
        setDatabaseListStatus(QtUtils::toQString(server->connectionRecord()->getReadableName()),
                              static_cast<int>(event->list.size()));

        // WholeDatabase mode: every database comes into use now.
        SchemaCollector::Options options;
        std::shared_ptr<const ConnectionSettings> connection;
        const QString uuid = _serverUuids.value(server);
        if (_schemaCollector && schemaOptions(uuid, &options, &connection)) {
            QStringList names;
            for (MongoDatabase *db : event->list) {
                if (db)
                    names << QtUtils::toQString(db->name());
            }
            _schemaCollector->databasesListed(uuid, connection, names, options);
        }
    }

    void QmlBackend::handle(MongoDatabaseCollectionsLoadingEvent *event)
    {
        if (auto *db = findDatabaseBySender(event->sender()))
            _tree->setCollectionsLoading(db, true);
    }

    void QmlBackend::handle(MongoDatabaseCollectionListLoadedEvent *event)
    {
        auto *db = findDatabaseBySender(event->sender());
        if (!db)
            return;
        if (event->isError()) {
            _tree->setCollectionsLoading(db, false);
            return;
        }
        _tree->setDatabaseCollections(db, event->collections);
        // The database is in use: the collector analyzes its collections.
        schemaDatabaseInUse(_serverUuids.value(db->server()), QtUtils::toQString(db->name()));
    }

    void QmlBackend::handle(MongoDatabaseFunctionsLoadingEvent *event)
    {
        if (auto *db = findDatabaseBySender(event->sender()))
            _tree->setFunctionsLoading(db, true);
    }

    void QmlBackend::handle(MongoDatabaseFunctionsLoadedEvent *event)
    {
        auto *db = findDatabaseBySender(event->sender());
        if (!db)
            return;
        if (event->isError()) {
            _tree->setFunctionsLoading(db, false);
            return;
        }
        _tree->setDatabaseFunctions(db, event->functions());
    }

    void QmlBackend::handle(MongoDatabaseUsersLoadingEvent *event)
    {
        if (auto *db = findDatabaseBySender(event->sender())) {
            const QString uuid = _serverUuids.value(db->server());
            if (!uuid.isEmpty())
                emit databaseUsersLoading(uuid, QtUtils::toQString(db->name()));
        }
    }

    void QmlBackend::handle(MongoDatabaseUsersLoadedEvent *event)
    {
        auto *db = findDatabaseBySender(event->sender());
        if (!db)
            return;
        const QString uuid = _serverUuids.value(db->server());
        const QString qDbName = QtUtils::toQString(db->name());
        if (uuid.isEmpty())
            return;

        if (event->isError()) {
            emit databaseUsersLoaded(uuid, qDbName, {},
                                     QtUtils::toQString(event->error().errorMessage()));
            return;
        }

        QVariantList list;
        for (const MongoUser &u : event->users()) {
            const QString uName = QtUtils::toQString(u.name());
            QStringList qRoles;
            for (const std::string &r : u.roles())
                qRoles.append(QtUtils::toQString(r));
            QStringList qMechs;
            for (const std::string &m : u.mechanisms())
                qMechs.append(QtUtils::toQString(m));
            const QString roleDb = u.userSource().empty()
                ? qDbName
                : QtUtils::toQString(u.userSource());
            const QString badge = qRoles.isEmpty()
                ? QString()
                : (qRoles.size() == 1
                       ? qRoles.first()
                       : QStringLiteral("%1 +%2").arg(qRoles.first()).arg(qRoles.size() - 1));
            list.append(QVariantMap{
                {QStringLiteral("name"),       uName},
                {QStringLiteral("roles"),      qRoles},
                {QStringLiteral("mechanisms"), qMechs},
                {QStringLiteral("userSource"), roleDb},
                {QStringLiteral("badgeText"),  badge}
            });
        }

        _databaseUsersCache.insert(uuid + QLatin1Char('/') + qDbName, list);
        emit databaseUsersLoaded(uuid, qDbName, list, QString());
    }

    void QmlBackend::handle(InsertDocumentResponse *event)
    {
        if (_pendingExplorerInserts > 0)
            --_pendingExplorerInserts;

        if (event->isError()) {
            _explorerInsertBatchHadError = true;
            _explorerInsertLastError = QtUtils::toQString(event->error().errorMessage());
            setStatus(_explorerInsertLastError);
        }

        if (_pendingExplorerInserts <= 0) {
            _pendingExplorerInserts = 0;
            const bool hadErr = _explorerInsertBatchHadError;
            const int count = _explorerInsertBatchTotal;
            const QString errMsg = _explorerInsertLastError;
            _explorerInsertBatchTotal = 0;
            _explorerInsertBatchHadError = false;
            _explorerInsertLastError.clear();
            if (hadErr) {
                emit documentInsertFinished(false, errMsg);
            } else {
                const QString okMsg = tr("Inserted %n document(s) into %1.%2", "", count)
                                          .arg(iso(_explorerInsertDb), iso(_explorerInsertColl));
                setStatus(okMsg);
                emit documentInsertFinished(true, okMsg);
            }
        }
    }

    void QmlBackend::handle(OperationFailedEvent *event)
    {
        QString msg = QtUtils::toQString(event->userFriendlyErrorMessage);
        if (!event->technicalErrorMessage.empty())
            msg += tr("\n\nError:\n%1").arg(QtUtils::toQString(event->technicalErrorMessage));
        setStatus(QtUtils::toQString(event->userFriendlyErrorMessage));
        emit connectionError(QString(), msg);
    }

    void QmlBackend::handle(LogEvent *event)
    {
        // Log events from the worker threads (and the SSH tunnel) all pass here.
        // Debug level ones are only worth the disk in debug mode.
        const bool debugLevel = event->level == LogEvent::RBM_DEBUG;
        if (!debugLevel || AppRegistry::instance().settingsManager()->debugMode())
            AppLog::write(event->mongoLogSeverity(), QtUtils::toQString(event->message).simplified());
        if (debugLevel)
            return;
        _logModel->append(QtUtils::toQString(event->severity()),
                          QtUtils::toQString(event->message));
    }
}
