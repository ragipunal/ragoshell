#pragma once

#include <QAbstractItemModel>
#include <QVariantMap>
#include <QVector>
#include <QString>
#include <vector>

#include "ragoshell/core/events/MongoEventsInfo.h"
#include "ragoshell/core/domain/MongoFunction.h"

namespace RagoShell
{
    class MongoServer;
    class MongoDatabase;
    class MongoCollection;
    class SchemaCatalog;
    class SchemaCollector;

    /**
     * @brief Explorer hierarchy for connected servers (Compass-style direct database layout):
     *
     *   Server ("server")
     *     ├── Replica Set (N nodes) ("rsFolder")        [when server is Replica Set]
     *     │     └── host:port ("rsMember")
     *     ├── System ("systemDbFolder")                 [when system databases exist]
     *     │     └── Database ("database")
     *     └── Database ("database")
     *           ├── Collection / View ("collection")
     *           │     └── Indexes ("indexesFolder")     [omitted on read-only Views]
     *           │           └── Index ("index")
     *           ├── System ("systemCollFolder")         [when system collections exist]
     *           │     └── Collection ("collection")
     *           └── Functions ("functionsFolder")       [only when system.js functions exist]
     *                 └── Function ("function")
     */
    class ServerTreeModel : public QAbstractItemModel
    {
        Q_OBJECT

    public:
        enum Roles {
            NameRole = Qt::UserRole + 1,
            RawNameRole,
            TypeRole,
            LoadingRole,
            ServerUuidRole,
            ParentNameRole,   // kept for backwards compatibility (dbName)
            DbNameRole,
            CollNameRole,
            BadgeTextRole,
            BadgeKindRole,    // "accent" | "info" | "warning" | "danger" | ""
            SubtitleRole,
            OnlineRole,
            IsReplicaSetRole,
            IndexMetaRole,
            ToolTipRole,
            /// Collection: "fresh" / "stale" snapshot in the schema catalog,
            /// "" = none. Database: "running" while schema jobs are pending.
            SchemaStateRole,
            /// Collection: documents sampled. Database: collections analyzed.
            SchemaCountRole,
            /// Database: collections that can be analyzed (-1 = not listed yet).
            SchemaTotalRole,
            /// Collection: when its snapshot was taken (local time).
            SchemaAnalyzedAtRole
        };

        struct Node {
            enum Type {
                Server,
                ReplicaSetFolder,
                ReplicaSetMember,
                SystemDbFolder,
                Database,
                SystemCollFolder,
                Collection,
                IndexesFolder,
                Index,
                FunctionsFolder,
                Function,
                Placeholder
            } type;

            Node *parent = nullptr;
            QVector<Node *> children;

            QString name;        // formatted display name (with counts / ellipsis)
            QString rawName;     // clean identifier (db, collection, index, host:port, etc.)
            QString serverUuid;
            QString dbName;
            QString collName;
            QString badgeText;
            QString badgeKind;
            QString subtitle;
            QString toolTip;
            QString serverVersionStr;
            QVariantMap indexMeta;

            bool loading = false;
            bool loaded = false;
            bool online = true;
            bool isReplicaSet = false;
            int count = -1;

            MongoServer *server = nullptr;
            MongoDatabase *db = nullptr;
            MongoCollection *coll = nullptr;

            ~Node() { qDeleteAll(children); }
        };

        explicit ServerTreeModel(QObject *parent = nullptr);
        ~ServerTreeModel() override;

        QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
        QModelIndex parent(const QModelIndex &child) const override;
        int rowCount(const QModelIndex &parent = QModelIndex()) const override;
        int columnCount(const QModelIndex &parent = QModelIndex()) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        // --- mutation API used by QmlBackend ---
        void addServerNode(MongoServer *server, const QString &uuid,
                           const QString &versionStr = QString());
        void setServerVersionString(MongoServer *server, const QString &versionStr);
        void removeServerNode(MongoServer *server);
        void populateReplicaSetServer(MongoServer *server);
        void updateReplicaSetFolder(MongoServer *server, bool primaryReachable, bool expandFolder);
        void setReplicaSetFolderLoading(MongoServer *server, bool loading);
        void setServerDatabases(MongoServer *server, const QList<MongoDatabase *> &dbs);

        void setDatabaseCollections(MongoDatabase *db, const std::vector<MongoCollection *> &colls);
        void setDatabaseFunctions(MongoDatabase *db, const std::vector<MongoFunction> &funcs);
        void setCollectionIndexes(MongoServer *server, const QString &dbName,
                                  const QString &collName, const std::vector<IndexInfo> &indexes);
        void setCollectionIndexesError(MongoServer *server, const QString &dbName,
                                       const QString &collName);

        void setLoading(MongoServer *server, bool loading);
        void setCollectionsLoading(MongoDatabase *db, bool loading);
        void setFunctionsLoading(MongoDatabase *db, bool loading);
        void setIndexesLoading(MongoServer *server, const QString &dbName,
                               const QString &collName, bool loading);

        Node *findServerNode(MongoServer *server) const;
        Node *findDatabaseNode(MongoDatabase *db) const;
        Node *findDatabaseNode(MongoServer *server, const QString &dbName) const;
        Node *findCollectionNode(MongoServer *server, const QString &dbName, const QString &collName) const;
        bool hasServer(MongoServer *server) const;
        QModelIndex serverIndex(MongoServer *server) const;

        /// Trigger lazy-loading when the QML TreeView expands a node.
        Q_INVOKABLE void expandIndex(const QModelIndex &index);

        /// Where the Schema* roles come from; the tree follows the
        /// collector's progress signal.
        void setSchemaSources(SchemaCatalog *catalog, SchemaCollector *collector);
        /// Refreshes the Schema* roles of every database of a server (its
        /// schema data was forgotten).
        void refreshSchema(const QString &serverUuid);

        /// Re-applies translated folder labels, "Loading…" rows and server
        /// tooltips after a language switch.
        void retranslate();

    Q_SIGNALS:
        void serverPopulated(const QModelIndex &serverIndex);
        void folderExpandRequested(const QModelIndex &folderIndex);
        void indexesLoadRequested(MongoServer *server, const QString &dbName, const QString &collName);

    private:
        Node *findDatabaseNode(const QString &serverUuid, const QString &dbName) const;
        /// Refreshes the Schema* roles of a database and its collections.
        void schemaChanged(const QString &serverUuid, const QString &dbName);
        Node *nodeFromIndex(const QModelIndex &index) const;
        QModelIndex indexForNode(Node *node) const;
        void replaceChildren(Node *parentNode, QVector<Node *> &&newChildren);

        Node *makeReplicaSetFolderNode(MongoServer *server, const QString &uuid) const;
        QVector<Node *> makeReplicaSetMemberNodes(MongoServer *server, const QString &uuid) const;
        Node *makeDatabaseNode(MongoServer *server, MongoDatabase *db, const QString &uuid) const;
        Node *makeCollectionNode(MongoServer *server, MongoDatabase *db,
                                 MongoCollection *coll, const QString &uuid) const;
        Node *makeFunctionsFolderNode(MongoServer *server, MongoDatabase *db, const QString &uuid) const;
        static Node *makePlaceholderNode(const QString &uuid);
        /// Display label of a folder node; plain (no count) when count < 0.
        static QString folderLabel(Node::Type type, int count);

        Node *findChildByType(Node *parentNode, Node::Type type) const;

        Node *_root;
        SchemaCatalog *_schemaCatalog = nullptr;
        SchemaCollector *_schemaCollector = nullptr;
    };
}
