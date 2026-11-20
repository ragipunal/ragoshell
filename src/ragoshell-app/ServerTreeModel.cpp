#include "ragoshell-app/ServerTreeModelInternal.h"

namespace RagoShell
{
    using namespace ServerTreeDetail;

    ServerTreeModel::ServerTreeModel(QObject *parent)
        : QAbstractItemModel(parent), _root(new Node{Node::Server})
    {
    }

    ServerTreeModel::~ServerTreeModel()
    {
        delete _root;
    }

    ServerTreeModel::Node *ServerTreeModel::nodeFromIndex(const QModelIndex &index) const
    {
        return index.isValid() ? static_cast<Node *>(index.internalPointer()) : _root;
    }

    QModelIndex ServerTreeModel::indexForNode(Node *node) const
    {
        if (!node || node == _root || !node->parent)
            return {};
        int row = node->parent->children.indexOf(node);
        return row >= 0 ? createIndex(row, 0, node) : QModelIndex{};
    }

    QModelIndex ServerTreeModel::index(int row, int column, const QModelIndex &parent) const
    {
        if (column != 0)
            return {};
        Node *parentNode = nodeFromIndex(parent);
        if (!parentNode || row < 0 || row >= parentNode->children.size())
            return {};
        return createIndex(row, column, parentNode->children.at(row));
    }

    QModelIndex ServerTreeModel::parent(const QModelIndex &child) const
    {
        if (!child.isValid())
            return {};
        Node *node = nodeFromIndex(child);
        if (!node || node == _root || !node->parent || node->parent == _root)
            return {};
        Node *parentNode = node->parent;
        Node *grand = parentNode->parent ? parentNode->parent : _root;
        int row = grand->children.indexOf(parentNode);
        return row >= 0 ? createIndex(row, 0, parentNode) : QModelIndex{};
    }

    int ServerTreeModel::rowCount(const QModelIndex &parent) const
    {
        if (parent.column() > 0)
            return 0;
        Node *node = nodeFromIndex(parent);
        return node ? node->children.size() : 0;
    }

    int ServerTreeModel::columnCount(const QModelIndex &) const
    {
        return 1;
    }

    QVariant ServerTreeModel::data(const QModelIndex &index, int role) const
    {
        Node *node = nodeFromIndex(index);
        if (!node || node == _root)
            return {};

        switch (role) {
        case NameRole:
            return node->name;
        case RawNameRole:
            return node->rawName.isEmpty() ? node->name : node->rawName;
        case TypeRole:
            switch (node->type) {
            case Node::Server:            return QStringLiteral("server");
            case Node::ReplicaSetFolder:  return QStringLiteral("rsFolder");
            case Node::ReplicaSetMember:  return QStringLiteral("rsMember");
            case Node::SystemDbFolder:    return QStringLiteral("systemDbFolder");
            case Node::Database:          return QStringLiteral("database");
            case Node::SystemCollFolder:  return QStringLiteral("systemCollFolder");
            case Node::Collection:        return QStringLiteral("collection");
            case Node::IndexesFolder:     return QStringLiteral("indexesFolder");
            case Node::Index:             return QStringLiteral("index");
            case Node::FunctionsFolder:   return QStringLiteral("functionsFolder");
            case Node::Function:          return QStringLiteral("function");
            case Node::Placeholder:       return QStringLiteral("placeholder");
            }
            break;
        case LoadingRole:
            return node->loading;
        case ServerUuidRole:
            return node->serverUuid;
        case ParentNameRole:
        case DbNameRole:
            return node->dbName;
        case CollNameRole:
            return node->collName;
        case BadgeTextRole:
            return node->badgeText;
        case BadgeKindRole:
            return node->badgeKind;
        case SubtitleRole:
            return node->subtitle;
        case OnlineRole:
            return node->online;
        case IsReplicaSetRole:
            return node->isReplicaSet;
        case IndexMetaRole:
            return node->indexMeta;
        case ToolTipRole:
            return node->toolTip;
        case SchemaStateRole:
        case SchemaCountRole:
        case SchemaTotalRole:
        case SchemaAnalyzedAtRole:
            if (node->type == Node::Collection) {
                const std::optional<SchemaSnapshot> snapshot = _schemaCatalog
                    ? _schemaCatalog->find(node->serverUuid, node->dbName, node->collName)
                    : std::optional<SchemaSnapshot>();
                if (role == SchemaStateRole)
                    return !snapshot ? QString()
                           : SchemaCatalog::isStale(*snapshot, QDateTime::currentDateTimeUtc())
                               ? QStringLiteral("stale") : QStringLiteral("fresh");
                if (role == SchemaCountRole)
                    return snapshot ? snapshot->result.sampledDocuments : 0;
                if (role == SchemaAnalyzedAtRole)
                    return snapshot ? snapshot->result.analyzedAt.toLocalTime() : QDateTime();
                return -1;
            }
            if (node->type == Node::Database && _schemaCollector) {
                const SchemaCollector::Progress p = _schemaCollector->progress(node->serverUuid, node->rawName);
                if (role == SchemaStateRole)
                    return p.pending > 0 ? QStringLiteral("running") : QString();
                if (role == SchemaCountRole)
                    return p.analyzed;
                if (role == SchemaTotalRole)
                    return p.total;
                return QDateTime();
            }
            if (role == SchemaStateRole)
                return QString();
            if (role == SchemaAnalyzedAtRole)
                return QDateTime();
            return role == SchemaTotalRole ? -1 : 0;
        }
        return {};
    }

    QHash<int, QByteArray> ServerTreeModel::roleNames() const
    {
        return {
            {NameRole,         "name"},
            {RawNameRole,      "rawName"},
            {TypeRole,         "nodeType"},
            {LoadingRole,      "loading"},
            {ServerUuidRole,   "serverUuid"},
            {ParentNameRole,   "parentName"},
            {DbNameRole,       "dbName"},
            {CollNameRole,     "collName"},
            {BadgeTextRole,    "badgeText"},
            {BadgeKindRole,    "badgeKind"},
            {SubtitleRole,     "subtitle"},
            {OnlineRole,       "online"},
            {IsReplicaSetRole, "isReplicaSet"},
            {IndexMetaRole,    "indexMeta"},
            {ToolTipRole,      "toolTipText"},
            {SchemaStateRole,  "schemaState"},
            {SchemaCountRole,  "schemaCount"},
            {SchemaTotalRole,  "schemaTotal"},
            {SchemaAnalyzedAtRole, "schemaAnalyzedAt"}
        };
    }

    void ServerTreeModel::setSchemaSources(SchemaCatalog *catalog, SchemaCollector *collector)
    {
        _schemaCatalog = catalog;
        _schemaCollector = collector;
        if (collector)
            connect(collector, &SchemaCollector::progressChanged, this, &ServerTreeModel::schemaChanged);
    }

    void ServerTreeModel::refreshSchema(const QString &serverUuid)
    {
        for (Node *s : _root->children) {
            if (s->type != Node::Server || s->serverUuid != serverUuid)
                continue;
            for (Node *child : s->children) {
                if (child->type == Node::Database)
                    schemaChanged(serverUuid, child->rawName);
                else if (child->type == Node::SystemDbFolder)
                    for (Node *sysDb : child->children)
                        schemaChanged(serverUuid, sysDb->rawName);
            }
        }
    }

    ServerTreeModel::Node *ServerTreeModel::findDatabaseNode(const QString &serverUuid, const QString &dbName) const
    {
        for (Node *s : _root->children) {
            if (s->type == Node::Server && s->serverUuid == serverUuid)
                return findDatabaseNode(s->server, dbName);
        }
        return nullptr;
    }

    void ServerTreeModel::schemaChanged(const QString &serverUuid, const QString &dbName)
    {
        Node *d = findDatabaseNode(serverUuid, dbName);
        if (!d)
            return;
        const QList<int> roles{SchemaStateRole, SchemaCountRole, SchemaTotalRole, SchemaAnalyzedAtRole};
        const QModelIndex dIdx = indexForNode(d);
        emit dataChanged(dIdx, dIdx, roles);
        if (!d->children.isEmpty())
            emit dataChanged(indexForNode(d->children.first()), indexForNode(d->children.last()), roles);
    }

    void ServerTreeModel::replaceChildren(Node *parentNode, QVector<Node *> &&newChildren)
    {
        if (!parentNode) {
            qDeleteAll(newChildren);
            return;
        }
        const QModelIndex pIdx = indexForNode(parentNode);
        const int oldCount = static_cast<int>(parentNode->children.size());
        const int newCount = static_cast<int>(newChildren.size());

        // Insert new rows before removing old rows (such as the "Loading…"
        // placeholder) so rowCount(pIdx) never transiently drops to 0 while
        // pIdx is expanded; otherwise Qt 6's QQmlTreeModelToTableModel
        // auto-collapses the parent node inside modelRowsRemoved().
        if (newCount > 0) {
            for (Node *c : newChildren)
                c->parent = parentNode;
            beginInsertRows(pIdx, oldCount, oldCount + newCount - 1);
            parentNode->children.reserve(oldCount + newCount);
            for (Node *c : newChildren)
                parentNode->children.append(c);
            newChildren.clear();
            endInsertRows();
        }

        if (oldCount > 0) {
            QVector<Node *> doomed;
            doomed.reserve(oldCount);
            for (int i = 0; i < oldCount; ++i)
                doomed.append(parentNode->children.at(i));

            beginRemoveRows(pIdx, 0, oldCount - 1);
            parentNode->children.remove(0, oldCount);
            endRemoveRows();

            // Delete removed nodes after endRemoveRows() so persistent-index
            // updates never observe freed internalPointer() targets.
            qDeleteAll(doomed);
        }
    }

    ServerTreeModel::Node *ServerTreeModel::findChildByType(Node *parentNode, Node::Type type) const
    {
        if (!parentNode)
            return nullptr;
        for (Node *c : parentNode->children) {
            if (c->type == type)
                return c;
        }
        return nullptr;
    }

    ServerTreeModel::Node *ServerTreeModel::findServerNode(MongoServer *server) const
    {
        for (Node *n : _root->children) {
            if (n->type == Node::Server && n->server == server)
                return n;
        }
        return nullptr;
    }

    ServerTreeModel::Node *ServerTreeModel::findDatabaseNode(MongoDatabase *db) const
    {
        if (!db)
            return nullptr;
        for (Node *s : _root->children) {
            for (Node *child : s->children) {
                if (child->type == Node::Database && child->db == db)
                    return child;
                if (child->type == Node::SystemDbFolder) {
                    for (Node *sysDb : child->children) {
                        if (sysDb->type == Node::Database && sysDb->db == db)
                            return sysDb;
                    }
                }
            }
        }
        return nullptr;
    }

    ServerTreeModel::Node *ServerTreeModel::findDatabaseNode(MongoServer *server, const QString &dbName) const
    {
        Node *s = findServerNode(server);
        if (!s)
            return nullptr;
        for (Node *child : s->children) {
            if (child->type == Node::Database && child->rawName == dbName)
                return child;
            if (child->type == Node::SystemDbFolder) {
                for (Node *sysDb : child->children) {
                    if (sysDb->type == Node::Database && sysDb->rawName == dbName)
                        return sysDb;
                }
            }
        }
        return nullptr;
    }

    ServerTreeModel::Node *ServerTreeModel::findCollectionNode(
        MongoServer *server, const QString &dbName, const QString &collName) const
    {
        Node *d = findDatabaseNode(server, dbName);
        if (!d)
            return nullptr;
        for (Node *c : d->children) {
            if (c->type == Node::Collection && c->rawName == collName)
                return c;
            if (c->type == Node::SystemCollFolder) {
                for (Node *sc : c->children) {
                    if (sc->type == Node::Collection && sc->rawName == collName)
                        return sc;
                }
            }
        }
        return nullptr;
    }

    bool ServerTreeModel::hasServer(MongoServer *server) const
    {
        return findServerNode(server) != nullptr;
    }

    QModelIndex ServerTreeModel::serverIndex(MongoServer *server) const
    {
        return indexForNode(findServerNode(server));
    }

    QString ServerTreeModel::folderLabel(Node::Type type, int count)
    {
        const bool withCount = count >= 0;
        switch (type) {
        case Node::FunctionsFolder:
            return withCount ? tr("Functions (%1)").arg(count) : tr("Functions");
        case Node::IndexesFolder:
            return withCount ? tr("Indexes (%1)").arg(count) : tr("Indexes");
        case Node::SystemDbFolder:
        case Node::SystemCollFolder:
            return withCount ? tr("System (%1)").arg(count) : tr("System");
        case Node::ReplicaSetFolder:
            return withCount ? tr("Replica Set (%n nodes)", "", count) : tr("Replica Set");
        default:
            return QString();
        }
    }

    void ServerTreeModel::retranslate()
    {
        std::function<void(Node *)> walk = [&](Node *node) {
            for (Node *child : node->children) {
                const QString label = child->type == Node::Placeholder
                    ? tr("Loading…")
                    : folderLabel(child->type, child->loading ? -1 : child->count);
                if (!label.isEmpty() && label != child->name) {
                    child->name = label;
                    const QModelIndex idx = indexForNode(child);
                    emit dataChanged(idx, idx, {NameRole});
                }
                if (child->type == Node::Server) {
                    applyServerBadgeAndToolTip(child);
                    const QModelIndex idx = indexForNode(child);
                    emit dataChanged(idx, idx, {ToolTipRole});
                }
                walk(child);
            }
        };
        if (_root)
            walk(_root);
    }

    ServerTreeModel::Node *ServerTreeModel::makePlaceholderNode(const QString &uuid)
    {
        auto *p = new Node{Node::Placeholder};
        p->name = tr("Loading…");
        p->rawName = QString();
        p->serverUuid = uuid;
        p->loading = true;
        return p;
    }

    QVector<ServerTreeModel::Node *> ServerTreeModel::makeReplicaSetMemberNodes(
        MongoServer *server, const QString &uuid) const
    {
        QVector<Node *> members;
        if (!server || !server->replicaSetInfo())
            return members;

        const std::string primaryAddr = server->replicaSetInfo()->primary.toString();
        for (const auto &memberAndHealth : server->replicaSetInfo()->membersAndHealths) {
            const std::string &addr = memberAndHealth.first;
            const bool isUp = memberAndHealth.second;
            const bool isPrimary = isUp && !primaryAddr.empty() && (primaryAddr == addr);

            auto *m = new Node{Node::ReplicaSetMember};
            m->server = server;
            m->serverUuid = uuid;
            m->rawName = QString::fromStdString(addr);
            m->name = m->rawName;
            m->online = isUp;
            if (isUp) {
                m->badgeText = isPrimary ? QStringLiteral("PRIMARY") : QStringLiteral("SECONDARY");
                m->badgeKind = isPrimary ? QStringLiteral("accent") : QStringLiteral("info");
            } else {
                m->badgeText = QStringLiteral("OFFLINE");
                m->badgeKind = QStringLiteral("danger");
            }
            members.append(m);
        }
        return members;
    }

    ServerTreeModel::Node *ServerTreeModel::makeReplicaSetFolderNode(
        MongoServer *server, const QString &uuid) const
    {
        auto *folder = new Node{Node::ReplicaSetFolder};
        folder->server = server;
        folder->serverUuid = uuid;
        folder->rawName = QStringLiteral("Replica Set");
        folder->loaded = true;

        QVector<Node *> members = makeReplicaSetMemberNodes(server, uuid);
        folder->count = members.size();
        folder->name = folderLabel(Node::ReplicaSetFolder, folder->count);
        for (Node *m : members) {
            m->parent = folder;
            folder->children.append(m);
        }
        return folder;
    }

    ServerTreeModel::Node *ServerTreeModel::makeDatabaseNode(
        MongoServer *server, MongoDatabase *db, const QString &uuid) const
    {
        const QString qDbName = QtUtils::toQString(db->name());

        auto *d = new Node{Node::Database};
        d->server = server;
        d->db = db;
        d->serverUuid = uuid;
        d->rawName = qDbName;
        d->name = qDbName;
        d->dbName = qDbName;

        // $external is MongoDB's virtual X.509/LDAP/Kerberos auth database and never holds collections.
        if (qDbName == QLatin1String("$external")) {
            d->loaded = true;
            d->count = 0;
            return d;
        }

        d->loaded = false;
        Node *ph = makePlaceholderNode(uuid);
        ph->parent = d;
        d->children.append(ph);
        return d;
    }

    ServerTreeModel::Node *ServerTreeModel::makeFunctionsFolderNode(
        MongoServer *server, MongoDatabase *db, const QString &uuid) const
    {
        const QString qDbName = QtUtils::toQString(db->name());
        auto *fnFolder = new Node{Node::FunctionsFolder};
        fnFolder->server = server;
        fnFolder->db = db;
        fnFolder->serverUuid = uuid;
        fnFolder->dbName = qDbName;
        fnFolder->rawName = QStringLiteral("Functions");
        fnFolder->name = folderLabel(Node::FunctionsFolder, -1);
        fnFolder->badgeText = QStringLiteral("DEPRECATED");
        fnFolder->badgeKind = QStringLiteral("warning");
        fnFolder->toolTip = tr("Stored JavaScript functions (db.system.js) — deprecated in MongoDB 8.0");
        fnFolder->loaded = false;

        Node *ph = makePlaceholderNode(uuid);
        ph->parent = fnFolder;
        fnFolder->children.append(ph);
        return fnFolder;
    }

    ServerTreeModel::Node *ServerTreeModel::makeCollectionNode(
        MongoServer *server, MongoDatabase *db, MongoCollection *coll, const QString &uuid) const
    {
        const QString qDbName = QtUtils::toQString(db->name());
        const QString qCollName = QtUtils::toQString(coll->name());
        const MongoCollectionInfo info = coll->info();

        auto *c = new Node{Node::Collection};
        c->server = server;
        c->db = db;
        c->coll = coll;
        c->serverUuid = uuid;
        c->dbName = qDbName;
        c->collName = qCollName;
        c->rawName = qCollName;
        c->name = qCollName;
        c->loaded = true;

        if (info.isView()) {
            c->badgeText = QStringLiteral("VIEW");
            c->badgeKind = QStringLiteral("info");
            const QString viewTarget = QtUtils::toQString(info.viewOn());
            c->toolTip = viewTarget.isEmpty()
                ? tr("Read-only view")
                : tr("Read-only view on '%1'").arg(viewTarget);
            c->indexMeta = QVariantMap{
                {QStringLiteral("storageType"), QStringLiteral("view")},
                {QStringLiteral("viewOn"), viewTarget}
            };
            // MongoDB read-only views delegate indexing to their source collection and do not support listIndexes.
            return c;
        }

        if (info.isTimeSeries()) {
            c->badgeText = QStringLiteral("TS");
            c->badgeKind = QStringLiteral("info");
            c->toolTip = tr("Time series collection");
            c->indexMeta = QVariantMap{
                {QStringLiteral("storageType"), QStringLiteral("timeseries")}
            };
        } else if (info.isCapped()) {
            c->badgeText = QStringLiteral("CAPPED");
            c->badgeKind = QStringLiteral("warning");
            c->toolTip = tr("Capped collection");
            c->indexMeta = QVariantMap{
                {QStringLiteral("storageType"), QStringLiteral("collection")},
                {QStringLiteral("capped"), true}
            };
        } else {
            c->indexMeta = QVariantMap{
                {QStringLiteral("storageType"), QStringLiteral("collection")}
            };
        }

        auto *idxFolder = new Node{Node::IndexesFolder};
        idxFolder->parent = c;
        idxFolder->server = server;
        idxFolder->db = db;
        idxFolder->coll = coll;
        idxFolder->serverUuid = uuid;
        idxFolder->dbName = qDbName;
        idxFolder->collName = qCollName;
        idxFolder->rawName = QStringLiteral("Indexes");
        idxFolder->name = folderLabel(Node::IndexesFolder, -1);
        idxFolder->loaded = false;

        Node *ph = makePlaceholderNode(uuid);
        ph->parent = idxFolder;
        idxFolder->children.append(ph);

        c->children.append(idxFolder);
        return c;
    }

}
