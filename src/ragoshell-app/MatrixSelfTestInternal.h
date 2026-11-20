#pragma once

#include "ragoshell-app/MatrixSelfTest.h"

#include <cmath>
#include <cstdio>
#include <functional>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUuid>

#include "ragoshell/core/AppRegistry.h"
#include "ragoshell/core/services/UiServices.h"
#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/settings/CredentialSettings.h"
#include "ragoshell/core/settings/ReplicaSetSettings.h"
#include "ragoshell/core/settings/SettingsManager.h"
#include "ragoshell/core/settings/SshSettings.h"
#include "ragoshell/core/settings/SslSettings.h"
#include "ragoshell/core/utils/AppPaths.h"
#include "ragoshell/core/utils/SchemaCatalog.h"
#include "ragoshell/core/utils/SchemaService.h"
#include "ragoshell-app/ConnectionListModel.h"
#include "ragoshell-app/QmlBackend.h"
#include "ragoshell-app/QmlSingleton.h"
#include "ragoshell-app/QmlUiServices.h"
#include "ragoshell-app/QueryHistoryModel.h"
#include "ragoshell-app/ScriptDraftModel.h"
#include "ragoshell-app/ServerMonitorController.h"
#include "ragoshell-app/ServerTreeModel.h"
#include "ragoshell-app/ShellController.h"

namespace RagoShell::MatrixTestDetail
{
        struct Harness
        {
            QTemporaryDir tempConfigDir;
            SettingsManager *settings = nullptr;
            QmlUiServices uiServices;
            std::unique_ptr<ConnectionListModel> connectionModel;
            std::unique_ptr<QueryHistoryModel> queryHistory;
            std::unique_ptr<SchemaCatalog> schemaCatalog;
            std::unique_ptr<SchemaService> schemaService;
            std::unique_ptr<ScriptDraftModel> scriptDrafts;
            std::unique_ptr<QmlBackend> backend;
            QString lastErrorUuid;
            QString lastErrorMsg;
            int hostKeysTrusted = 0;
            int failures = 0;

            Harness()
            {
                qputenv(productEnvName("CONFIG_DIR").toLatin1().constData(), tempConfigDir.path().toUtf8());
                qputenv(productEnvName("KEYSTORE").toLatin1().constData(), "file");

                settings = AppRegistry::instance().settingsManager();
                UiServices::setInstance(&uiServices);

                connectionModel = std::make_unique<ConnectionListModel>(settings);
                queryHistory = std::make_unique<QueryHistoryModel>(
                    appDataDir() + QStringLiteral("query_history.json"));
                schemaCatalog = std::make_unique<SchemaCatalog>(
                    appDataDir() + QStringLiteral("schema"));
                SchemaCatalog::setInstance(schemaCatalog.get());
                schemaService = std::make_unique<SchemaService>(schemaCatalog.get());
                SchemaService::setInstance(schemaService.get());
                scriptDrafts = std::make_unique<ScriptDraftModel>(
                    appDataDir() + QStringLiteral("drafts.json"));
                scriptDrafts->setStartupPromptEnabled(false);

                backend = std::make_unique<QmlBackend>();
                backend->setPersistUiState(false);

                QmlSingletonInstance<QmlBackend>::set(backend.get());
                QmlSingletonInstance<QueryHistoryModel>::set(queryHistory.get());
                QmlSingletonInstance<ScriptDraftModel>::set(scriptDrafts.get());
                QmlSingletonInstance<ConnectionListModel>::set(connectionModel.get());
                QmlSingletonInstance<QmlUiServices>::set(&uiServices);

                QObject::connect(backend.get(), &QmlBackend::connectionError, backend.get(),
                                 [this](const QString &uuid, const QString &msg) {
                    lastErrorUuid = uuid;
                    lastErrorMsg = msg;
                });

                QObject::connect(backend.get(), &QmlBackend::hostKeyRequested, backend.get(),
                                 [this](int id, const QString &, int, const QString &, const QString &, bool) {
                    ++hostKeysTrusted;
                    backend->answerHostKey(id, true);
                });
            }

            ~Harness()
            {
                if (backend) {
                    const QStringList uuids = backend->connectedUuids();
                    for (const QString &u : uuids)
                        backend->disconnectServer(u, true);
                    pumpMs(150);
                }
                backend.reset();
                scriptDrafts.reset();
                schemaService.reset();
                SchemaService::setInstance(nullptr);
                schemaCatalog.reset();
                SchemaCatalog::setInstance(nullptr);
                queryHistory.reset();
                connectionModel.reset();
                UiServices::setInstance(nullptr);
            }

            static void pumpMs(int ms)
            {
                QElapsedTimer t;
                t.start();
                while (t.elapsed() < ms)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            }

            static bool waitUntil(int timeoutMs, const std::function<bool()> &pred)
            {
                QElapsedTimer t;
                t.start();
                while (t.elapsed() < timeoutMs) {
                    if (pred())
                        return true;
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                }
                return pred();
            }

            void check(bool pass, const QString &name, const QString &detail = QString())
            {
                if (!pass)
                    ++failures;
                const QString line = QStringLiteral("MATRIXSELFTEST ")
                    + (pass ? QStringLiteral("PASS ") : QStringLiteral("FAIL "))
                    + name
                    + (detail.isEmpty() ? QString() : QStringLiteral(" -- ") + detail);
                std::fprintf(stderr, "%s\n", qPrintable(line));
                std::fflush(stderr);
            }

            QString saveAndConnect(ConnectionSettings &cs, bool expectSuccess = true, int timeoutMs = 15000)
            {
                if (cs.uuid().isEmpty())
                    cs.setUuid(QUuid::createUuid().toString());
                if (cs.schemaCollectionMode().empty())
                    cs.setSchemaCollectionMode("off");
                const QString uuid = cs.uuid();
                connectionModel->create(cs.toVariant().toMap());

                lastErrorUuid.clear();
                lastErrorMsg.clear();
                backend->connectToServer(uuid);

                if (expectSuccess) {
                    const bool ok = waitUntil(timeoutMs, [&]() {
                        return (backend->isServerConnected(uuid) && !backend->connecting())
                               || (!lastErrorMsg.isEmpty() && (lastErrorUuid.isEmpty() || lastErrorUuid == uuid));
                    });
                    if (!ok || !backend->isServerConnected(uuid)) {
                        if (lastErrorMsg.isEmpty())
                            lastErrorMsg = backend->statusText();
                        return QString();
                    }
                    // Wait briefly for initial database list to populate
                    waitUntil(6000, [&]() {
                        const QModelIndex srvIdx = findServerIndex(uuid);
                        if (!srvIdx.isValid())
                            return false;
                        return !backend->serverTree()->data(srvIdx, ServerTreeModel::LoadingRole).toBool()
                               && backend->serverTree()->rowCount(srvIdx) > 0;
                    });
                    return uuid;
                } else {
                    waitUntil(timeoutMs, [&]() {
                        return !lastErrorMsg.isEmpty() || backend->isServerConnected(uuid);
                    });
                    return uuid;
                }
            }

            QModelIndex findServerIndex(const QString &uuid) const
            {
                auto *tree = backend->serverTree();
                const int rows = tree->rowCount();
                for (int r = 0; r < rows; ++r) {
                    const QModelIndex idx = tree->index(r, 0);
                    if (tree->data(idx, ServerTreeModel::ServerUuidRole).toString() == uuid)
                        return idx;
                }
                return QModelIndex();
            }

            QModelIndex findChild(const QModelIndex &parent, const QString &type,
                                  const QString &rawName = QString()) const
            {
                auto *tree = backend->serverTree();
                const int rows = tree->rowCount(parent);
                for (int r = 0; r < rows; ++r) {
                    const QModelIndex idx = tree->index(r, 0, parent);
                    if (tree->data(idx, ServerTreeModel::TypeRole).toString() != type)
                        continue;
                    if (!rawName.isEmpty()
                        && tree->data(idx, ServerTreeModel::RawNameRole).toString() != rawName)
                        continue;
                    return idx;
                }
                return QModelIndex();
            }

            QModelIndex findDatabaseIndex(const QString &uuid, const QString &dbName) const
            {
                const QModelIndex srv = findServerIndex(uuid);
                if (!srv.isValid())
                    return QModelIndex();
                QModelIndex direct = findChild(srv, QStringLiteral("database"), dbName);
                if (direct.isValid())
                    return direct;
                const QModelIndex sysFolder = findChild(srv, QStringLiteral("systemDbFolder"));
                if (sysFolder.isValid())
                    return findChild(sysFolder, QStringLiteral("database"), dbName);
                return QModelIndex();
            }

            QStringList listedDatabaseNames(const QString &uuid) const
            {
                QStringList names;
                auto *tree = backend->serverTree();
                const QModelIndex srv = findServerIndex(uuid);
                if (!srv.isValid())
                    return names;
                const int rows = tree->rowCount(srv);
                for (int r = 0; r < rows; ++r) {
                    const QModelIndex idx = tree->index(r, 0, srv);
                    const QString t = tree->data(idx, ServerTreeModel::TypeRole).toString();
                    if (t == QLatin1String("database")) {
                        names << tree->data(idx, ServerTreeModel::RawNameRole).toString();
                    } else if (t == QLatin1String("systemDbFolder")) {
                        const int srows = tree->rowCount(idx);
                        for (int sr = 0; sr < srows; ++sr) {
                            const QModelIndex sidx = tree->index(sr, 0, idx);
                            if (tree->data(sidx, ServerTreeModel::TypeRole).toString() == QLatin1String("database"))
                                names << tree->data(sidx, ServerTreeModel::RawNameRole).toString();
                        }
                    }
                }
                return names;
            }

            bool expandAndWait(const QModelIndex &idx, int timeoutMs = 8000)
            {
                if (!idx.isValid())
                    return false;
                auto *tree = backend->serverTree();
                tree->expandIndex(idx);
                return waitUntil(timeoutMs, [&]() {
                    if (!idx.isValid())
                        return false;
                    if (tree->data(idx, ServerTreeModel::LoadingRole).toBool())
                        return false;
                    const int rc = tree->rowCount(idx);
                    if (rc == 1) {
                        const QModelIndex c0 = tree->index(0, 0, idx);
                        if (tree->data(c0, ServerTreeModel::TypeRole).toString() == QLatin1String("placeholder"))
                            return false;
                    }
                    return true;
                });
            }

            ShellController *runShellScript(const QString &uuid, const QString &dbName,
                                            const QString &script, int timeoutMs = 12000)
            {
                const int before = backend->shells().size();
                backend->openShellWithScript(uuid, dbName, script, true);
                if (!waitUntil(timeoutMs, [&]() { return backend->shells().size() > before; }))
                    return nullptr;
                auto *ctl = qobject_cast<ShellController *>(backend->shells().last().value<QObject *>());
                if (!ctl)
                    return nullptr;
                waitUntil(timeoutMs, [&]() {
                    return !ctl->busy() && (ctl->hasTextOutput() || !ctl->errorText().isEmpty());
                });
                return ctl;
            }
        };

        inline QString readTextTrimmed(const QString &path)
        {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly))
                return QString();
            return QString::fromUtf8(f.readAll()).trimmed();
        }

        inline void addAuth(ConnectionSettings &cs, const std::string &user, const std::string &pass,
                     const std::string &db = "admin", const std::string &mech = "DEFAULT")
        {
            auto *cred = new CredentialSettings();
            cred->setEnabled(true);
            cred->setUserName(user);
            cred->setUserPassword(pass);
            cred->setDatabaseName(db);
            cred->setMechanism(mech);
            cs.addCredential(cred);
        }


    void runStrictMtlsSuite(Harness &h, int port, const QString &fixturesDir);
    void runReplicaSetSuite(Harness &h, const QStringList &ports, const QString &fixturesDir);
    void runShardedSuite(Harness &h, int port, const QString &fixturesDir);
    void runSshSuite(Harness &h, int sshPort, const QString &fixturesDir);
}
