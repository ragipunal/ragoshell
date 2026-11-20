#include "ragoshell-app/ServerTreeModelInternal.h"

namespace RagoShell
{
    using namespace ServerTreeDetail;

    void ServerTreeModel::addServerNode(MongoServer *server, const QString &uuid,
                                        const QString &versionStr)
    {
        if (!server || findServerNode(server))
            return;

        int row = _root->children.size();
        beginInsertRows({}, row, row);
        auto *node = new Node{Node::Server};
        node->parent = _root;
        node->server = server;
        node->serverUuid = uuid;
        node->serverVersionStr = versionStr;
        node->rawName = QtUtils::toQString(server->connectionRecord()->getReadableName());
        node->name = node->rawName;
        node->online = true;
        applyServerBadgeAndToolTip(node);
        _root->children.append(node);
        endInsertRows();
    }

    void ServerTreeModel::setServerVersionString(MongoServer *server, const QString &versionStr)
    {
        Node *s = findServerNode(server);
        if (!s)
            return;
        s->serverVersionStr = versionStr;
        applyServerBadgeAndToolTip(s);
        const QModelIndex sIdx = indexForNode(s);
        emit dataChanged(sIdx, sIdx, {BadgeTextRole, BadgeKindRole, ToolTipRole});
    }

    void ServerTreeModel::removeServerNode(MongoServer *server)
    {
        Node *node = findServerNode(server);
        if (!node)
            return;
        int row = _root->children.indexOf(node);
        beginRemoveRows({}, row, row);
        _root->children.remove(row);
        endRemoveRows();
        delete node;
    }

    void ServerTreeModel::populateReplicaSetServer(MongoServer *server)
    {
        if (!server)
            return;
        setServerDatabases(server, server->databases());
    }

    void ServerTreeModel::setServerDatabases(MongoServer *server,
                                             const QList<MongoDatabase *> &dbs)
    {
        Node *s = findServerNode(server);
        if (!s)
            return;

        QVector<Node *> newChildren;

        // 1. Replica Set folder (if Replica Set connection)
        s->isReplicaSet = server->connectionRecord()->isReplicaSet();
        if (s->isReplicaSet)
            newChildren.append(makeReplicaSetFolderNode(server, s->serverUuid));

        // 2. Partition databases into System vs User databases (skip virtual '$external' auth namespace)
        QVector<Node *> systemDbNodes;
        QVector<Node *> userDbNodes;
        for (MongoDatabase *db : dbs) {
            if (!db || db->name() == "$external")
                continue;
            Node *dbNode = makeDatabaseNode(server, db, s->serverUuid);
            if (db->isSystem() || db->name() == "config")
                systemDbNodes.append(dbNode);
            else
                userDbNodes.append(dbNode);
        }

        if (!systemDbNodes.isEmpty()) {
            auto *sysFolder = new Node{Node::SystemDbFolder};
            sysFolder->server = server;
            sysFolder->serverUuid = s->serverUuid;
            sysFolder->rawName = QStringLiteral("System");
            sysFolder->count = systemDbNodes.size();
            sysFolder->name = folderLabel(Node::SystemDbFolder, sysFolder->count);
            sysFolder->loaded = true;
            for (Node *sysDb : systemDbNodes) {
                sysDb->parent = sysFolder;
                sysFolder->children.append(sysDb);
            }
            newChildren.append(sysFolder);
        }

        for (Node *userDb : userDbNodes)
            newChildren.append(userDb);

        replaceChildren(s, std::move(newChildren));

        s->rawName = QtUtils::toQString(server->connectionRecord()->getReadableName());
        s->count = static_cast<int>(userDbNodes.size() + systemDbNodes.size());
        s->name = QString("%1 (%2)").arg(s->rawName).arg(s->count);
        s->loading = false;
        s->loaded = true;
        s->online = true;
        applyServerBadgeAndToolTip(s);

        QModelIndex sIdx = indexForNode(s);
        emit dataChanged(sIdx, sIdx);
        emit serverPopulated(sIdx);
    }

    void ServerTreeModel::setReplicaSetFolderLoading(MongoServer *server, bool loading)
    {
        Node *s = findServerNode(server);
        if (!s)
            return;
        Node *rsFolder = findChildByType(s, Node::ReplicaSetFolder);
        if (!rsFolder)
            return;
        rsFolder->loading = loading;
        rsFolder->name = folderLabel(Node::ReplicaSetFolder,
                                     loading ? -1 : static_cast<int>(rsFolder->children.size()));
        QModelIndex rsIdx = indexForNode(rsFolder);
        emit dataChanged(rsIdx, rsIdx, {NameRole, LoadingRole});
    }

    void ServerTreeModel::updateReplicaSetFolder(MongoServer *server,
                                                 bool primaryReachable,
                                                 bool expandFolder)
    {
        Node *s = findServerNode(server);
        if (!s)
            return;

        Node *rsFolder = findChildByType(s, Node::ReplicaSetFolder);
        if (!rsFolder) {
            QModelIndex sIdx = indexForNode(s);
            beginInsertRows(sIdx, 0, 0);
            rsFolder = makeReplicaSetFolderNode(server, s->serverUuid);
            rsFolder->parent = s;
            s->children.prepend(rsFolder);
            endInsertRows();
        } else {
            QVector<Node *> members = makeReplicaSetMemberNodes(server, s->serverUuid);
            rsFolder->count = members.size();
            rsFolder->name = folderLabel(Node::ReplicaSetFolder, rsFolder->count);
            rsFolder->loading = false;
            rsFolder->loaded = true;
            replaceChildren(rsFolder, std::move(members));
            QModelIndex rsIdx = indexForNode(rsFolder);
            emit dataChanged(rsIdx, rsIdx);
        }

        if (!primaryReachable) {
            s->online = false;
            s->loading = false;
            s->name = s->rawName;
        } else {
            s->online = true;
            s->loading = false;
            s->name = s->count >= 0
                ? QString("%1 (%2)").arg(s->rawName).arg(s->count)
                : s->rawName;
        }
        applyServerBadgeAndToolTip(s);

        QModelIndex sIdx = indexForNode(s);
        emit dataChanged(sIdx, sIdx);

        if (expandFolder)
            emit folderExpandRequested(indexForNode(rsFolder));
    }

    void ServerTreeModel::setDatabaseCollections(
        MongoDatabase *db, const std::vector<MongoCollection *> &colls)
    {
        Node *d = findDatabaseNode(db);
        if (!d)
            return;

        QVector<Node *> sysColls;
        QVector<Node *> userColls;
        bool hasSystemJs = false;
        for (MongoCollection *coll : colls) {
            if (!coll)
                continue;
            if (coll->name() == "system.js")
                hasSystemJs = true;
            Node *cNode = makeCollectionNode(d->server, db, coll, d->serverUuid);
            if (coll->isSystem())
                sysColls.append(cNode);
            else
                userColls.append(cNode);
        }

        // Preserve any already-populated FunctionsFolder when refreshing collections on a DB with system.js
        // by cloning its state into newChildren so replaceChildren() swaps children without an extra remove pass.
        Node *preservedFnFolder = nullptr;
        if (hasSystemJs) {
            if (const Node *existingFn = findChildByType(d, Node::FunctionsFolder)) {
                auto *fnClone = new Node{Node::FunctionsFolder};
                fnClone->server = existingFn->server;
                fnClone->db = existingFn->db;
                fnClone->serverUuid = existingFn->serverUuid;
                fnClone->dbName = existingFn->dbName;
                fnClone->rawName = existingFn->rawName;
                fnClone->name = existingFn->name;
                fnClone->badgeText = existingFn->badgeText;
                fnClone->badgeKind = existingFn->badgeKind;
                fnClone->toolTip = existingFn->toolTip;
                fnClone->loading = existingFn->loading;
                fnClone->loaded = existingFn->loaded;
                fnClone->count = existingFn->count;
                for (const Node *existingChild : existingFn->children) {
                    auto *childClone = new Node{existingChild->type};
                    childClone->parent = fnClone;
                    childClone->server = existingChild->server;
                    childClone->db = existingChild->db;
                    childClone->serverUuid = existingChild->serverUuid;
                    childClone->dbName = existingChild->dbName;
                    childClone->rawName = existingChild->rawName;
                    childClone->name = existingChild->name;
                    childClone->toolTip = existingChild->toolTip;
                    childClone->indexMeta = existingChild->indexMeta;
                    childClone->loading = existingChild->loading;
                    childClone->loaded = existingChild->loaded;
                    fnClone->children.append(childClone);
                }
                preservedFnFolder = fnClone;
            }
        }

        QVector<Node *> newChildren;
        for (Node *uc : userColls)
            newChildren.append(uc);

        if (!sysColls.isEmpty()) {
            auto *sysFolder = new Node{Node::SystemCollFolder};
            sysFolder->server = d->server;
            sysFolder->db = db;
            sysFolder->serverUuid = d->serverUuid;
            sysFolder->dbName = d->dbName;
            sysFolder->rawName = QStringLiteral("System");
            sysFolder->count = sysColls.size();
            sysFolder->name = folderLabel(Node::SystemCollFolder, sysFolder->count);
            sysFolder->loaded = true;
            for (Node *sc : sysColls) {
                sc->parent = sysFolder;
                sysFolder->children.append(sc);
            }
            newChildren.append(sysFolder);
        }

        bool autoLoadFunctions = false;
        if (preservedFnFolder) {
            newChildren.append(preservedFnFolder);
        } else if (hasSystemJs) {
            newChildren.append(makeFunctionsFolderNode(d->server, db, d->serverUuid));
            autoLoadFunctions = true;
        }

        replaceChildren(d, std::move(newChildren));

        d->count = static_cast<int>(userColls.size());
        d->name = QStringLiteral("%1 (%2)").arg(d->rawName).arg(d->count);
        d->loading = false;
        d->loaded = true;

        const QModelIndex dIdx = indexForNode(d);
        emit dataChanged(dIdx, dIdx);

        if (autoLoadFunctions)
            db->loadFunctions();
    }

    void ServerTreeModel::setDatabaseFunctions(
        MongoDatabase *db, const std::vector<MongoFunction> &funcs)
    {
        Node *d = findDatabaseNode(db);
        if (!d)
            return;

        Node *folder = findChildByType(d, Node::FunctionsFolder);
        if (funcs.empty()) {
            if (folder) {
                const int row = d->children.indexOf(folder);
                if (row >= 0) {
                    const QModelIndex dIdx = indexForNode(d);
                    beginRemoveRows(dIdx, row, row);
                    d->children.remove(row);
                    endRemoveRows();
                    delete folder;
                }
            }
            return;
        }

        if (!folder) {
            if (!d->loaded) {
                db->loadCollections();
                return;
            }
            const int row = d->children.size();
            const QModelIndex dIdx = indexForNode(d);
            beginInsertRows(dIdx, row, row);
            folder = makeFunctionsFolderNode(d->server, db, d->serverUuid);
            folder->parent = d;
            d->children.append(folder);
            endInsertRows();
        }

        QVector<Node *> newChildren;
        for (const MongoFunction &fn : funcs) {
            auto *fnNode = new Node{Node::Function};
            fnNode->server = d->server;
            fnNode->db = db;
            fnNode->serverUuid = d->serverUuid;
            fnNode->dbName = d->dbName;
            fnNode->rawName = QtUtils::toQString(fn.name());
            fnNode->name = fnNode->rawName;
            fnNode->loaded = true;
            const QString qCode = QtUtils::toQString(fn.code());
            fnNode->indexMeta = QVariantMap{
                {QStringLiteral("name"), fnNode->rawName},
                {QStringLiteral("code"), qCode}
            };
            if (!qCode.isEmpty()) {
                QString preview = qCode.left(260);
                if (qCode.size() > 260)
                    preview += QStringLiteral("…");
                fnNode->toolTip = tr("Function: %1\n\n%2").arg(fnNode->rawName, preview);
            }
            newChildren.append(fnNode);
        }

        replaceChildren(folder, std::move(newChildren));

        folder->count = static_cast<int>(funcs.size());
        folder->name = folderLabel(Node::FunctionsFolder, folder->count);
        folder->loading = false;
        folder->loaded = true;

        QModelIndex fIdx = indexForNode(folder);
        emit dataChanged(fIdx, fIdx);
    }

    void ServerTreeModel::setCollectionIndexes(
        MongoServer *server, const QString &dbName,
        const QString &collName, const std::vector<IndexInfo> &indexes)
    {
        Node *c = findCollectionNode(server, dbName, collName);
        if (!c)
            return;
        Node *folder = findChildByType(c, Node::IndexesFolder);
        if (!folder)
            return;

        QVector<Node *> newChildren;
        for (const IndexInfo &idx : indexes) {
            auto *iNode = new Node{Node::Index};
            iNode->server = server;
            iNode->db = c->db;
            iNode->coll = c->coll;
            iNode->serverUuid = c->serverUuid;
            iNode->dbName = dbName;
            iNode->collName = collName;
            iNode->rawName = QtUtils::toQString(idx._name);
            iNode->name = iNode->rawName;
            iNode->subtitle.clear();
            iNode->loaded = true;

            const QString rawKeys = QtUtils::toQString(idx._keys);
            const QString compactKeys = rawKeys.simplified();

            if (idx._hidden) {
                iNode->badgeText = QStringLiteral("HIDDEN");
                iNode->badgeKind = QStringLiteral("danger");
            } else if (idx._unique && idx._name != "_id_") {
                iNode->badgeText = QStringLiteral("UNIQUE");
                iNode->badgeKind = QStringLiteral("accent");
            } else if (idx._ttl >= 0) {
                iNode->badgeText = QStringLiteral("TTL");
                iNode->badgeKind = QStringLiteral("warning");
            } else if (!idx._partialFilterExpression.empty()) {
                iNode->badgeText = QStringLiteral("PARTIAL");
                iNode->badgeKind = QStringLiteral("info");
            } else if (idx._sparse) {
                iNode->badgeText = QStringLiteral("SPARSE");
                iNode->badgeKind = QStringLiteral("info");
            }

            QStringList props;
            if (idx._unique)
                props << tr("Unique");
            if (idx._sparse)
                props << tr("Sparse");
            if (idx._hidden)
                props << tr("Hidden");
            if (idx._backGround)
                props << tr("Background");
            if (idx._ttl >= 0)
                props << tr("TTL (%1s)").arg(idx._ttl);
            if (!idx._partialFilterExpression.empty())
                props << tr("Partial Filter");
            if (!idx._collation.empty())
                props << tr("Collation");
            if (!idx._wildcardProjection.empty())
                props << tr("Wildcard Projection");

            QStringList tipLines;
            tipLines << tr("Index: %1").arg(iNode->rawName);
            if (!compactKeys.isEmpty())
                tipLines << tr("Keys:  %1").arg(compactKeys);
            if (!props.isEmpty())
                tipLines << tr("Props: %1").arg(props.join(QStringLiteral(", ")));
            if (!idx._partialFilterExpression.empty())
                tipLines << tr("Partial: %1")
                                .arg(QtUtils::toQString(idx._partialFilterExpression).simplified());
            iNode->toolTip = tipLines.join(QLatin1Char('\n'));

            iNode->indexMeta = QVariantMap{
                {QStringLiteral("name"),                    iNode->rawName},
                {QStringLiteral("keys"),                    rawKeys},
                {QStringLiteral("unique"),                  idx._unique},
                {QStringLiteral("background"),              idx._backGround},
                {QStringLiteral("sparse"),                  idx._sparse},
                {QStringLiteral("hidden"),                  idx._hidden},
                {QStringLiteral("ttl"),                     idx._ttl},
                {QStringLiteral("defaultLanguage"),         QtUtils::toQString(idx._defaultLanguage)},
                {QStringLiteral("languageOverride"),        QtUtils::toQString(idx._languageOverride)},
                {QStringLiteral("textWeights"),             QtUtils::toQString(idx._textWeights)},
                {QStringLiteral("partialFilterExpression"), QtUtils::toQString(idx._partialFilterExpression)},
                {QStringLiteral("collation"),               QtUtils::toQString(idx._collation)},
                {QStringLiteral("wildcardProjection"),      QtUtils::toQString(idx._wildcardProjection)}
            };

            newChildren.append(iNode);
        }

        replaceChildren(folder, std::move(newChildren));

        folder->count = static_cast<int>(indexes.size());
        folder->name = folderLabel(Node::IndexesFolder, folder->count);
        folder->loading = false;
        folder->loaded = true;

        QModelIndex fIdx = indexForNode(folder);
        emit dataChanged(fIdx, fIdx);
    }

    void ServerTreeModel::setCollectionIndexesError(
        MongoServer *server, const QString &dbName, const QString &collName)
    {
        Node *c = findCollectionNode(server, dbName, collName);
        if (!c)
            return;
        Node *folder = findChildByType(c, Node::IndexesFolder);
        if (!folder)
            return;

        folder->loading = false;
        folder->name = folderLabel(Node::IndexesFolder, -1);
        QModelIndex fIdx = indexForNode(folder);
        emit dataChanged(fIdx, fIdx, {NameRole, LoadingRole});
    }

    void ServerTreeModel::setLoading(MongoServer *server, bool loading)
    {
        Node *s = findServerNode(server);
        if (!s)
            return;
        s->loading = loading;
        if (loading)
            s->name = s->rawName;
        else if (s->count >= 0)
            s->name = QString("%1 (%2)").arg(s->rawName).arg(s->count);
        QModelIndex sIdx = indexForNode(s);
        emit dataChanged(sIdx, sIdx, {NameRole, LoadingRole});
    }

    void ServerTreeModel::setCollectionsLoading(MongoDatabase *db, bool loading)
    {
        Node *d = findDatabaseNode(db);
        if (!d)
            return;
        d->loading = loading;
        if (loading)
            d->name = d->rawName;
        else if (d->count >= 0)
            d->name = QStringLiteral("%1 (%2)").arg(d->rawName).arg(d->count);
        const QModelIndex dIdx = indexForNode(d);
        emit dataChanged(dIdx, dIdx, {NameRole, LoadingRole});
    }

    void ServerTreeModel::setFunctionsLoading(MongoDatabase *db, bool loading)
    {
        Node *d = findDatabaseNode(db);
        if (!d)
            return;
        Node *folder = findChildByType(d, Node::FunctionsFolder);
        if (!folder)
            return;
        folder->loading = loading;
        if (loading)
            folder->name = folderLabel(Node::FunctionsFolder, -1);
        else if (folder->count >= 0)
            folder->name = folderLabel(Node::FunctionsFolder, folder->count);
        QModelIndex fIdx = indexForNode(folder);
        emit dataChanged(fIdx, fIdx, {NameRole, LoadingRole});
    }

    void ServerTreeModel::setIndexesLoading(
        MongoServer *server, const QString &dbName, const QString &collName, bool loading)
    {
        Node *c = findCollectionNode(server, dbName, collName);
        if (!c)
            return;
        Node *folder = findChildByType(c, Node::IndexesFolder);
        if (!folder)
            return;
        folder->loading = loading;
        if (loading)
            folder->name = folderLabel(Node::IndexesFolder, -1);
        else if (folder->count >= 0)
            folder->name = folderLabel(Node::IndexesFolder, folder->count);
        QModelIndex fIdx = indexForNode(folder);
        emit dataChanged(fIdx, fIdx, {NameRole, LoadingRole});
    }

    void ServerTreeModel::expandIndex(const QModelIndex &index)
    {
        Node *node = nodeFromIndex(index);
        if (!node || node == _root)
            return;

        switch (node->type) {
        case Node::Database:
            if (node->db && !node->loaded && !node->loading) {
                setCollectionsLoading(node->db, true);
                node->db->loadCollections();
            }
            break;

        case Node::FunctionsFolder:
            if (node->db && !node->loaded && !node->loading) {
                setFunctionsLoading(node->db, true);
                node->db->loadFunctions();
            }
            break;

        case Node::Collection: {
            Node *idxFolder = findChildByType(node, Node::IndexesFolder);
            if (idxFolder && node->server && !idxFolder->loaded && !idxFolder->loading) {
                setIndexesLoading(node->server, node->dbName, node->collName, true);
                emit indexesLoadRequested(node->server, node->dbName, node->collName);
            }
            break;
        }

        case Node::IndexesFolder:
            if (node->server && !node->loaded && !node->loading) {
                setIndexesLoading(node->server, node->dbName, node->collName, true);
                emit indexesLoadRequested(node->server, node->dbName, node->collName);
            }
            break;

        default:
            break;
        }
    }
}
