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

    void QmlBackend::createDatabase(const QString &uuid, const QString &name)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || name.isEmpty())
            return;

        const std::string stdName = QtUtils::toStdString(name);
        if (server->connectionRecord()->isReplicaSet()) {
            // MongoDB only materializes a database once a collection exists, and RS
            // refresh queries listDatabases directly — mirror ExplorerServerTreeItem
            // by adding an in-memory MongoDatabase node immediately.
            if (!server->findDatabaseByName(stdName)) {
                auto *newDb = new MongoDatabase(server, stdName);
                server->addDatabase(newDb);
                _tree->populateReplicaSetServer(server);
                LOG_MSG("Database '" + stdName + "' created.", RagoShell::LogSeverity::Info);
                setStatus(tr("Database '%1' created").arg(iso(name)));
            }
        } else {
            server->createDatabase(stdName);
        }
    }

    void QmlBackend::dropDatabase(const QString &uuid, const QString &name)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (server && !name.isEmpty())
            server->dropDatabase(QtUtils::toStdString(name));
    }

    void QmlBackend::refreshDatabase(const QString &uuid, const QString &dbName)
    {
        refreshCollections(uuid, dbName);
    }

    void QmlBackend::refreshCollections(const QString &uuid, const QString &dbName)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server)
            return;
        if (auto *db = server->findDatabaseByName(QtUtils::toStdString(dbName)))
            db->loadCollections();
    }

    void QmlBackend::refreshFunctions(const QString &uuid, const QString &dbName)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server)
            return;
        if (auto *db = server->findDatabaseByName(QtUtils::toStdString(dbName)))
            db->loadFunctions();
    }

    void QmlBackend::refreshUsers(const QString &uuid, const QString &dbName)
    {
        requestDatabaseUsers(uuid, dbName);
    }

    QStringList QmlBackend::serverDatabaseNames(const QString &uuid) const
    {
        QStringList out{QStringLiteral("admin"), QStringLiteral("$external")};
        MongoServer *server = findServerByUuid(uuid);
        if (!server)
            return out;
        QStringList extras;
        for (MongoDatabase *db : server->databases()) {
            if (!db)
                continue;
            const QString n = QtUtils::toQString(db->name());
            if (!n.isEmpty() && !out.contains(n) && !extras.contains(n))
                extras.append(n);
        }
        extras.sort(Qt::CaseInsensitive);
        out.append(extras);
        return out;
    }

    void QmlBackend::requestDatabaseUsers(const QString &uuid, const QString &dbName)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server)
            return;
        const QString targetDb = dbName.trimmed().isEmpty() ? QStringLiteral("admin") : dbName.trimmed();
        const std::string stdDbName = QtUtils::toStdString(targetDb);
        auto *db = server->findDatabaseByName(stdDbName);
        if (!db && !stdDbName.empty()) {
            db = new MongoDatabase(server, stdDbName);
            server->addDatabase(db);
        }
        if (db)
            db->loadUsers();
    }

    QVariantList QmlBackend::cachedDatabaseUsers(const QString &uuid, const QString &dbName) const
    {
        const QString targetDb = dbName.trimmed().isEmpty() ? QStringLiteral("admin") : dbName.trimmed();
        return _databaseUsersCache.value(uuid + QLatin1Char('/') + targetDb);
    }

    void QmlBackend::refreshIndexes(const QString &uuid, const QString &dbName,
                                    const QString &collName)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || dbName.isEmpty() || collName.isEmpty())
            return;
        loadCollectionIndexes(server, dbName, collName);
    }

    void QmlBackend::loadCollectionIndexes(MongoServer *server, const QString &dbName,
                                           const QString &collName)
    {
        if (!server || !_serverUuids.contains(server) || !server->worker())
            return;

        _tree->setIndexesLoading(server, dbName, collName, true);

        MongoCollectionInfo collInfo(
            MongoNamespace(QtUtils::toStdString(dbName), QtUtils::toStdString(collName)).toString());
        auto *proxy = new IndexRequestProxy(
            this, server, dbName, collName, IndexRequestProxy::Mode::LoadIndexes);
        _bus->send(server->worker(), new LoadCollectionIndexesRequest(proxy, collInfo));
    }

    void QmlBackend::createCollection(const QString &uuid, const QString &dbName,
                                      const QString &collName)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || collName.isEmpty())
            return;
        if (auto *db = server->findDatabaseByName(QtUtils::toStdString(dbName)))
            db->createCollection(QtUtils::toStdString(collName), 0, false, 0,
                                 bsoncxx::from_json("{}"));
    }

    bool QmlBackend::createCollectionAdvanced(const QString &uuid, const QString &dbName,
                                              const QString &collName,
                                              bool capped, qint64 sizeBytes, int maxDocs,
                                              const QString &validatorJson,
                                              const QString &validationLevel,
                                              const QString &validationAction,
                                              const QString &storageEngineJson,
                                              const QString &extraOptionsJson)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || collName.trimmed().isEmpty()) {
            emit connectionError(uuid, tr("Collection name is required."));
            return false;
        }
        if (capped && sizeBytes <= 0) {
            emit connectionError(uuid, tr("Capped collections require a positive size in bytes."));
            return false;
        }
        auto *db = server->findDatabaseByName(QtUtils::toStdString(dbName));
        if (!db) {
            emit connectionError(uuid, tr("Database '%1' not found.").arg(dbName));
            return false;
        }

        namespace basic = bsoncxx::builder::basic;
        basic::document extraBuilder;

        auto appendParsedSubdoc = [&](const QString &keyName, const QString &rawJson) -> bool {
            const QString trimmed = rawJson.trimmed();
            if (trimmed.isEmpty() || trimmed == QLatin1String("{}"))
                return true;
            bsoncxx::document::value parsed = bsoncxx::from_json("{}");
            QString err;
            if (!MongoJsonUtils::parseSingleDocument(trimmed, parsed, &err)) {
                emit connectionError(uuid, tr("Invalid %1 JSON: %2").arg(keyName, err));
                return false;
            }
            extraBuilder.append(basic::kvp(keyName.toStdString(), parsed.view()));
            return true;
        };

        if (!validatorJson.trimmed().isEmpty() && validatorJson.trimmed() != QLatin1String("{}")) {
            if (!appendParsedSubdoc(QStringLiteral("validator"), validatorJson))
                return false;
            if (!validationLevel.trimmed().isEmpty())
                extraBuilder.append(basic::kvp("validationLevel", validationLevel.trimmed().toStdString()));
            if (!validationAction.trimmed().isEmpty())
                extraBuilder.append(basic::kvp("validationAction", validationAction.trimmed().toStdString()));
        }

        if (!appendParsedSubdoc(QStringLiteral("storageEngine"), storageEngineJson))
            return false;

        if (!extraOptionsJson.trimmed().isEmpty() && extraOptionsJson.trimmed() != QLatin1String("{}")) {
            bsoncxx::document::value extraParsed = bsoncxx::from_json("{}");
            QString err;
            if (!MongoJsonUtils::parseSingleDocument(extraOptionsJson, extraParsed, &err)) {
                emit connectionError(uuid, tr("Invalid extra options JSON: %1").arg(err));
                return false;
            }
            for (const auto &el : extraParsed.view()) {
                extraBuilder.append(basic::kvp(
                    std::string(el.key().data(), el.key().size()),
                    el.get_value()));
            }
        }

        db->createCollection(QtUtils::toStdString(collName.trimmed()),
                             static_cast<long long>(sizeBytes),
                             capped,
                             maxDocs,
                             extraBuilder.extract());
        return true;
    }

    void QmlBackend::dropCollection(const QString &uuid, const QString &dbName,
                                    const QString &collName)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || collName.isEmpty())
            return;
        if (auto *db = server->findDatabaseByName(QtUtils::toStdString(dbName)))
            db->dropCollection(QtUtils::toStdString(collName));
    }

    void QmlBackend::renameCollection(const QString &uuid, const QString &dbName,
                                      const QString &collName, const QString &newName)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || collName.isEmpty() || newName.isEmpty())
            return;
        if (auto *db = server->findDatabaseByName(QtUtils::toStdString(dbName)))
            db->renameCollection(QtUtils::toStdString(collName),
                                 QtUtils::toStdString(newName));
    }

    void QmlBackend::duplicateCollection(const QString &uuid, const QString &dbName,
                                         const QString &collName, const QString &newName)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || collName.isEmpty() || newName.isEmpty())
            return;
        if (auto *db = server->findDatabaseByName(QtUtils::toStdString(dbName)))
            db->duplicateCollection(QtUtils::toStdString(collName),
                                    QtUtils::toStdString(newName));
    }

    void QmlBackend::removeAllDocuments(const QString &uuid, const QString &dbName,
                                        const QString &collName)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || dbName.isEmpty() || collName.isEmpty())
            return;
        server->removeDocuments(bsoncxx::document::view_or_value{},
                                MongoNamespace(QtUtils::toStdString(dbName),
                                               QtUtils::toStdString(collName)),
                                RemoveDocumentCount::ALL);
    }

    void QmlBackend::saveIndex(const QString &uuid, const QString &dbName,
                               const QString &collName, const QString &oldIndexName,
                               const QVariantMap &spec)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || !server->worker() || dbName.isEmpty() || collName.isEmpty())
            return;

        MongoCollectionInfo collInfo(
            MongoNamespace(QtUtils::toStdString(dbName), QtUtils::toStdString(collName)).toString());

        const QString newName = spec.value(QStringLiteral("name")).toString().trimmed();
        const QString keys = spec.value(QStringLiteral("keys")).toString().trimmed();
        if (newName.isEmpty() || keys.isEmpty()) {
            emit connectionError(uuid, tr("Index name and keys JSON are required."));
            return;
        }

        IndexInfo oldInfo(collInfo, QtUtils::toStdString(oldIndexName));
        if (!oldIndexName.isEmpty()) {
            // Populate existing spec so MongoClient can restore on error
            oldInfo._keys = QtUtils::toStdString(spec.value(QStringLiteral("oldKeys"), keys).toString());
            oldInfo._unique = spec.value(QStringLiteral("oldUnique"), false).toBool();
            oldInfo._backGround = spec.value(QStringLiteral("oldBackground"), false).toBool();
            oldInfo._sparse = spec.value(QStringLiteral("oldSparse"), false).toBool();
            oldInfo._hidden = spec.value(QStringLiteral("oldHidden"), false).toBool();
            oldInfo._ttl = spec.value(QStringLiteral("oldTtl"), -1).toInt();
            oldInfo._defaultLanguage = QtUtils::toStdString(spec.value(QStringLiteral("oldDefaultLanguage")).toString());
            oldInfo._languageOverride = QtUtils::toStdString(spec.value(QStringLiteral("oldLanguageOverride")).toString());
            oldInfo._textWeights = QtUtils::toStdString(spec.value(QStringLiteral("oldTextWeights")).toString());
            oldInfo._partialFilterExpression = QtUtils::toStdString(spec.value(QStringLiteral("oldPartialFilterExpression")).toString());
            oldInfo._collation = QtUtils::toStdString(spec.value(QStringLiteral("oldCollation")).toString());
            oldInfo._wildcardProjection = QtUtils::toStdString(spec.value(QStringLiteral("oldWildcardProjection")).toString());
        }

        IndexInfo newInfo(
            collInfo,
            QtUtils::toStdString(newName),
            QtUtils::toStdString(keys),
            spec.value(QStringLiteral("unique"), false).toBool(),
            spec.value(QStringLiteral("background"), false).toBool(),
            spec.value(QStringLiteral("sparse"), false).toBool(),
            spec.value(QStringLiteral("ttl"), -1).toInt(),
            QtUtils::toStdString(spec.value(QStringLiteral("defaultLanguage")).toString()),
            QtUtils::toStdString(spec.value(QStringLiteral("languageOverride")).toString()),
            QtUtils::toStdString(spec.value(QStringLiteral("textWeights")).toString()));
        newInfo._hidden = spec.value(QStringLiteral("hidden"), false).toBool();
        newInfo._partialFilterExpression = QtUtils::toStdString(spec.value(QStringLiteral("partialFilterExpression")).toString());
        newInfo._collation = QtUtils::toStdString(spec.value(QStringLiteral("collation")).toString());
        newInfo._wildcardProjection = QtUtils::toStdString(spec.value(QStringLiteral("wildcardProjection")).toString());

        _tree->setIndexesLoading(server, dbName, collName, true);
        auto *proxy = new IndexRequestProxy(
            this, server, dbName, collName, IndexRequestProxy::Mode::SaveIndex);
        _bus->send(server->worker(), new AddEditIndexRequest(proxy, oldInfo, newInfo));
    }

    void QmlBackend::setIndexHidden(const QString &uuid, const QString &dbName,
                                    const QString &collName, const QVariantMap &indexMeta,
                                    bool hidden)
    {
        const QString idxName = indexMeta.value(QStringLiteral("name")).toString().trimmed();
        const QString keys = indexMeta.value(QStringLiteral("keys")).toString().trimmed();
        if (idxName.isEmpty() || idxName == QLatin1String("_id_") || keys.isEmpty())
            return;

        QVariantMap spec;
        spec.insert(QStringLiteral("name"), idxName);
        spec.insert(QStringLiteral("keys"), keys);
        spec.insert(QStringLiteral("unique"), indexMeta.value(QStringLiteral("unique"), false));
        spec.insert(QStringLiteral("background"), indexMeta.value(QStringLiteral("background"), false));
        spec.insert(QStringLiteral("sparse"), indexMeta.value(QStringLiteral("sparse"), false));
        spec.insert(QStringLiteral("hidden"), hidden);
        spec.insert(QStringLiteral("ttl"), indexMeta.value(QStringLiteral("ttl"), -1));
        spec.insert(QStringLiteral("defaultLanguage"), indexMeta.value(QStringLiteral("defaultLanguage")));
        spec.insert(QStringLiteral("languageOverride"), indexMeta.value(QStringLiteral("languageOverride")));
        spec.insert(QStringLiteral("textWeights"), indexMeta.value(QStringLiteral("textWeights")));
        spec.insert(QStringLiteral("partialFilterExpression"), indexMeta.value(QStringLiteral("partialFilterExpression")));
        spec.insert(QStringLiteral("collation"), indexMeta.value(QStringLiteral("collation")));
        spec.insert(QStringLiteral("wildcardProjection"), indexMeta.value(QStringLiteral("wildcardProjection")));

        spec.insert(QStringLiteral("oldKeys"), keys);
        spec.insert(QStringLiteral("oldUnique"), indexMeta.value(QStringLiteral("unique"), false));
        spec.insert(QStringLiteral("oldBackground"), indexMeta.value(QStringLiteral("background"), false));
        spec.insert(QStringLiteral("oldSparse"), indexMeta.value(QStringLiteral("sparse"), false));
        spec.insert(QStringLiteral("oldHidden"), indexMeta.value(QStringLiteral("hidden"), false));
        spec.insert(QStringLiteral("oldTtl"), indexMeta.value(QStringLiteral("ttl"), -1));
        spec.insert(QStringLiteral("oldDefaultLanguage"), indexMeta.value(QStringLiteral("defaultLanguage")));
        spec.insert(QStringLiteral("oldLanguageOverride"), indexMeta.value(QStringLiteral("languageOverride")));
        spec.insert(QStringLiteral("oldTextWeights"), indexMeta.value(QStringLiteral("textWeights")));
        spec.insert(QStringLiteral("oldPartialFilterExpression"), indexMeta.value(QStringLiteral("partialFilterExpression")));
        spec.insert(QStringLiteral("oldCollation"), indexMeta.value(QStringLiteral("collation")));
        spec.insert(QStringLiteral("oldWildcardProjection"), indexMeta.value(QStringLiteral("wildcardProjection")));

        saveIndex(uuid, dbName, collName, idxName, spec);
    }

    void QmlBackend::dropIndex(const QString &uuid, const QString &dbName,
                               const QString &collName, const QString &indexName)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || !server->worker() || dbName.isEmpty() || collName.isEmpty() || indexName.isEmpty())
            return;

        MongoCollectionInfo collInfo(
            MongoNamespace(QtUtils::toStdString(dbName), QtUtils::toStdString(collName)).toString());

        _tree->setIndexesLoading(server, dbName, collName, true);
        auto *proxy = new IndexRequestProxy(
            this, server, dbName, collName, IndexRequestProxy::Mode::DropIndex);
        _bus->send(server->worker(),
                   new DropCollectionIndexRequest(proxy, collInfo, QtUtils::toStdString(indexName)));
    }

    bool QmlBackend::createOrUpdateFunction(const QString &uuid, const QString &dbName,
                                            const QString &oldName, const QString &funcName,
                                            const QString &code)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || funcName.trimmed().isEmpty()) {
            emit connectionError(uuid, tr("Function name is required."));
            return false;
        }
        auto *db = server->findDatabaseByName(QtUtils::toStdString(dbName));
        if (!db) {
            emit connectionError(uuid, tr("Database '%1' not found.").arg(dbName));
            return false;
        }

        MongoFunction fn;
        fn.setName(QtUtils::toStdString(funcName.trimmed()));
        fn.setCode(QtUtils::toStdString(code));

        if (!oldName.trimmed().isEmpty())
            db->updateFunction(QtUtils::toStdString(oldName.trimmed()), fn);
        else
            db->createFunction(fn);
        return true;
    }

    void QmlBackend::dropFunction(const QString &uuid, const QString &dbName,
                                  const QString &funcName)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || funcName.isEmpty())
            return;
        if (auto *db = server->findDatabaseByName(QtUtils::toStdString(dbName)))
            db->dropFunction(QtUtils::toStdString(funcName));
    }

    bool QmlBackend::createOrUpdateUser(const QString &uuid, const QString &dbName,
                                        const QString &userName, const QString &password,
                                        const QString &roleDb, const QStringList &roles,
                                        bool isUpdate, const QStringList &mechanisms)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || userName.trimmed().isEmpty()) {
            emit connectionError(uuid, tr("Username is required."));
            return false;
        }
        const bool isExternalDb = (dbName.trimmed() == QLatin1String("$external"));
        if (!isUpdate && !isExternalDb && password.isEmpty()) {
            emit connectionError(uuid, tr("Password is required when creating a new user."));
            return false;
        }
        const std::string stdDbName = QtUtils::toStdString(dbName.trimmed());
        auto *db = server->findDatabaseByName(stdDbName);
        if (!db && !stdDbName.empty()) {
            db = new MongoDatabase(server, stdDbName);
            server->addDatabase(db);
        }
        if (!db) {
            emit connectionError(uuid, tr("Database '%1' not found.").arg(dbName));
            return false;
        }

        MongoUser user(server->version());
        user.setName(QtUtils::toStdString(userName.trimmed()));
        if (!isExternalDb)
            user.setPassword(QtUtils::toStdString(password));
        const QString defaultRoleTarget = isExternalDb ? QStringLiteral("admin") : dbName.trimmed();
        user.setUserSource(QtUtils::toStdString(roleDb.trimmed().isEmpty() ? defaultRoleTarget : roleDb.trimmed()));
        user.setOverwrite(isUpdate);

        std::vector<std::string> stdRoles;
        for (const QString &r : roles) {
            if (!r.trimmed().isEmpty())
                stdRoles.push_back(QtUtils::toStdString(r.trimmed()));
        }
        user.setRoles(stdRoles);

        if (!isExternalDb && !mechanisms.isEmpty()) {
            std::vector<std::string> stdMechs;
            for (const QString &m : mechanisms) {
                if (!m.trimmed().isEmpty())
                    stdMechs.push_back(QtUtils::toStdString(m.trimmed()));
            }
            user.setMechanisms(stdMechs);
        }

        db->createUser(user);
        return true;
    }

    void QmlBackend::dropUser(const QString &uuid, const QString &dbName,
                              const QString &userName)
    {
        MongoServer *server = findServerByUuid(uuid);
        if (!server || userName.isEmpty())
            return;
        const std::string stdDbName = QtUtils::toStdString(dbName.trimmed());
        auto *db = server->findDatabaseByName(stdDbName);
        if (!db && !stdDbName.empty()) {
            db = new MongoDatabase(server, stdDbName);
            server->addDatabase(db);
        }
        if (db)
            db->dropUser(QtUtils::toStdString(userName));
    }

    void QmlBackend::startExportCollection(const QString &uuid, const QString &dbName,
                                           const QString &collName, const QString &filePath,
                                           int format, const QString &filterJson, int limit)
    {
        if (_transferBusy)
            return;

        MongoServer *server = findServerByUuid(uuid);
        if (!server || !server->connectionRecord()) {
            emit transferFinished(false, 0, tr("Server is not connected."));
            return;
        }
        if (server->connectionRecord()->hasTransport()) {
            emit transferFinished(
                false, 0,
                tr("Collection export is not supported over gateway or demo connection channels."));
            return;
        }

        if (_transferThread && _transferThread->joinable())
            _transferThread->join();

        ensureLegacyStoragePermission(filePath, true);
        _transferCancel.store(false);
        _transferBusy = true;
        _transferProgress = 0;
        _transferStatus = tr("Exporting %1.%2…").arg(iso(dbName), iso(collName));
        emit transferStateChanged();

        std::shared_ptr<ConnectionSettings> connCopy(server->connectionRecord()->clone());
        const auto fmt = static_cast<CollectionTransfer::Format>(std::clamp(format, 0, 2));
        QPointer<QmlBackend> self(this);

        _transferThread = std::make_unique<std::thread>(
            [self, connCopy, dbName, collName, filePath, fmt, filterJson, limit]() {
                QString err;
                const int exported = CollectionTransfer::exportCollectionToFile(
                    *connCopy, dbName, collName, filePath, fmt, filterJson, limit,
                    self ? &self->_transferCancel : nullptr,
                    [self](int count) {
                        if (!self)
                            return;
                        QMetaObject::invokeMethod(self, [self, count]() {
                            if (!self)
                                return;
                            self->_transferProgress = count;
                            self->_transferStatus = tr("Exported %n document(s)…", "", count);
                            emit self->transferStateChanged();
                        }, Qt::QueuedConnection);
                    },
                    &err);

                if (!self)
                    return;

                QMetaObject::invokeMethod(self, [self, exported, err, dbName, collName, filePath]() {
                    if (!self)
                        return;
                    self->_transferBusy = false;
                    if (exported >= 0) {
                        self->_transferProgress = exported;
                        self->_transferStatus = tr("Exported %n document(s) from %1.%2.", "", exported)
                                                    .arg(iso(dbName), iso(collName));
                        self->setStatus(self->_transferStatus);
                        emit self->transferStateChanged();
                        emit self->transferFinished(true, exported, self->_transferStatus);
                    } else {
                        self->_transferStatus = err;
                        emit self->transferStateChanged();
                        emit self->transferFinished(false, 0, err);
                    }
                }, Qt::QueuedConnection);
            });
    }

    void QmlBackend::startImportCollection(const QString &uuid, const QString &dbName,
                                           const QString &collName, const QString &filePath,
                                           int format, bool dropBeforeImport, bool stopOnError)
    {
        if (_transferBusy)
            return;

        MongoServer *server = findServerByUuid(uuid);
        if (!server || !server->connectionRecord()) {
            emit transferFinished(false, 0, tr("Server is not connected."));
            return;
        }
        if (server->connectionRecord()->hasTransport()) {
            emit transferFinished(
                false, 0,
                tr("Collection import is not supported over gateway or demo connection channels."));
            return;
        }

        if (_transferThread && _transferThread->joinable())
            _transferThread->join();

        ensureLegacyStoragePermission(filePath, false);
        _transferCancel.store(false);
        _transferBusy = true;
        _transferProgress = 0;
        _transferStatus = tr("Importing into %1.%2…").arg(iso(dbName), iso(collName));
        emit transferStateChanged();

        std::shared_ptr<ConnectionSettings> connCopy(server->connectionRecord()->clone());
        const auto fmt = static_cast<CollectionTransfer::Format>(std::clamp(format, 0, 2));
        QPointer<QmlBackend> self(this);

        _transferThread = std::make_unique<std::thread>(
            [self, uuid, connCopy, dbName, collName, filePath, fmt, dropBeforeImport, stopOnError]() {
                QString err;
                const int imported = CollectionTransfer::importFileToCollection(
                    *connCopy, dbName, collName, filePath, fmt, dropBeforeImport, stopOnError,
                    self ? &self->_transferCancel : nullptr,
                    [self](int count) {
                        if (!self)
                            return;
                        QMetaObject::invokeMethod(self, [self, count]() {
                            if (!self)
                                return;
                            self->_transferProgress = count;
                            self->_transferStatus = tr("Imported %n document(s)…", "", count);
                            emit self->transferStateChanged();
                        }, Qt::QueuedConnection);
                    },
                    &err);

                if (!self)
                    return;

                QMetaObject::invokeMethod(self, [self, uuid, imported, err, dbName, collName]() {
                    if (!self)
                        return;
                    self->_transferBusy = false;
                    if (imported >= 0) {
                        self->_transferProgress = imported;
                        self->_transferStatus = tr("Imported %n document(s) into %1.%2.", "", imported)
                                                    .arg(iso(dbName), iso(collName));
                        self->setStatus(self->_transferStatus);
                        self->refreshCollections(uuid, dbName);
                        emit self->transferStateChanged();
                        emit self->transferFinished(true, imported, self->_transferStatus);
                    } else {
                        self->_transferStatus = err;
                        emit self->transferStateChanged();
                        emit self->transferFinished(false, 0, err);
                    }
                }, Qt::QueuedConnection);
            });
    }

    void QmlBackend::cancelTransfer()
    {
        _transferCancel.store(true);
    }

    void QmlBackend::onIndexesLoaded(MongoServer *server, const QString &dbName,
                                     const QString &collName,
                                     LoadCollectionIndexesResponse *event)
    {
        if (!_serverUuids.contains(server))
            return;
        if (event->isError()) {
            _tree->setCollectionIndexesError(server, dbName, collName);
            const QString msg = tr("Cannot load indexes for '%1.%2': %3")
                                    .arg(dbName, collName,
                                         QtUtils::toQString(event->error().errorMessage()));
            setStatus(msg);
            emit connectionError(_serverUuids.value(server), msg);
            return;
        }
        _tree->setCollectionIndexes(server, dbName, collName, event->indexes());
    }

    void QmlBackend::onIndexSaved(MongoServer *server, const QString &dbName,
                                  const QString &collName, AddEditIndexResponse *event)
    {
        if (!_serverUuids.contains(server))
            return;
        const bool isAdd = event->oldIndex_._name.empty();
        const QString idxName = QString::fromStdString(
            isAdd ? event->newIndex_._name : event->oldIndex_._name);

        if (event->isError()) {
            _tree->setIndexesLoading(server, dbName, collName, false);
            const QString detail = QtUtils::toQString(event->error().errorMessage());
            const QString msg = isAdd ? tr("Failed to create index \"%1\": %2").arg(idxName, detail)
                                      : tr("Failed to edit index \"%1\": %2").arg(idxName, detail);
            LOG_MSG(msg.toStdString(), RagoShell::LogSeverity::Error);
            setStatus(msg);
            emit connectionError(_serverUuids.value(server), msg);
            return;
        }

        const QString newName = QString::fromStdString(event->newIndex_._name);
        const QString okMsg = isAdd ? tr("Index \"%1\" created on %2.%3.").arg(newName, dbName, collName)
                                    : tr("Index \"%1\" updated on %2.%3.").arg(newName, dbName, collName);
        LOG_MSG(okMsg.toStdString(), RagoShell::LogSeverity::Info);
        setStatus(okMsg);
    }

    void QmlBackend::onIndexDropped(MongoServer *server, const QString &dbName,
                                    const QString &collName,
                                    DropCollectionIndexResponse *event)
    {
        if (!_serverUuids.contains(server))
            return;
        if (event->isError()) {
            _tree->setIndexesLoading(server, dbName, collName, false);
            const QString msg = tr("Failed to drop index \"%1\": %2")
                                    .arg(QString::fromStdString(event->index()),
                                         QtUtils::toQString(event->error().errorMessage()));
            LOG_MSG(msg.toStdString(), RagoShell::LogSeverity::Error);
            setStatus(msg);
            emit connectionError(_serverUuids.value(server), msg);
            return;
        }

        const QString okMsg = tr("Index \"%1\" dropped from %2.%3.")
                                  .arg(QString::fromStdString(event->index()), dbName, collName);
        LOG_MSG(okMsg.toStdString(), RagoShell::LogSeverity::Info);
        setStatus(okMsg);
        loadCollectionIndexes(server, dbName, collName);
    }

}
