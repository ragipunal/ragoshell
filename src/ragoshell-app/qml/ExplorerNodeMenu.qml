pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

RagoMenu {
    id: nodeMenu
    required property var pane
    readonly property bool _isViewCollection:
        Boolean(nodeMenu.pane._ctxIndexMeta && nodeMenu.pane._ctxIndexMeta.storageType === "view")

    component ActionItem: RagoMenuItem {}
    component ActionSeparator: RagoMenuSeparator {}
    width: 262

    // ---- SERVER NODE ------------------------------------------------
    ActionItem {
        text: qsTr("Open Shell")
        visible: nodeMenu.pane._menuNodeType === "server"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: Backend.openShell(nodeMenu.pane._ctxUuid, "", "")
    }
    ActionItem {
        text: qsTr("Server Monitor")
        visible: nodeMenu.pane._menuNodeType === "server"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: Backend.openMonitor(nodeMenu.pane._ctxUuid)
    }
    ActionItem {
        text: qsTr("Refresh")
        visible: nodeMenu.pane._menuNodeType === "server"
        onTriggered: Backend.refreshServer(nodeMenu.pane._ctxUuid)
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "server" }
    ActionItem {
        text: qsTr("Create Database…")
        visible: nodeMenu.pane._menuNodeType === "server"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: {
            nodeMenu.pane._ctxAction = "db"
            nameDialog.title = qsTr("Create Database")
            nameDialog.label = qsTr("Database name:")
            nameDialog.initialText = ""
            nameDialog.open()
        }
    }
    ActionItem {
        text: qsTr("Users & Roles…")
        visible: nodeMenu.pane._menuNodeType === "server"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: nodeMenu.pane.openManageUsers(nodeMenu.pane._ctxUuid, "admin")
    }
    ActionItem {
        text: qsTr("Server Status")
        visible: nodeMenu.pane._menuNodeType === "server"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: Backend.openShellWithScript(nodeMenu.pane._ctxUuid, "", "db.serverStatus()", true)
    }
    ActionItem {
        text: qsTr("Host Info")
        visible: nodeMenu.pane._menuNodeType === "server"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: Backend.openShellWithScript(nodeMenu.pane._ctxUuid, "", "db.hostInfo()", true)
    }
    ActionItem {
        text: qsTr("Build Info & Engine")
        visible: nodeMenu.pane._menuNodeType === "server"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: Backend.openShellWithScript(nodeMenu.pane._ctxUuid, "", "db.serverBuildInfo()", true)
    }
    ActionItem {
        text: qsTr("Feature Compatibility (FCV)")
        visible: nodeMenu.pane._menuNodeType === "server" && (nodeMenu.pane._ctxServerVer <= 0.0 || nodeMenu.pane._ctxServerVer >= 3.4)
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: Backend.openShellWithScript(nodeMenu.pane._ctxUuid, "", "db.getFeatureCompatibilityVersion()", true)
    }
    ActionItem {
        text: qsTr("Connection Pool Stats")
        visible: nodeMenu.pane._menuNodeType === "server"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: Backend.openShellWithScript(nodeMenu.pane._ctxUuid, "", "db.connPoolStats()", true)
    }
    ActionItem {
        text: qsTr("Show Log")
        visible: nodeMenu.pane._menuNodeType === "server"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: Backend.openShellWithScript(nodeMenu.pane._ctxUuid, "", "show log", true)
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "server" }
    ActionItem {
        text: qsTr("Disconnect")
        visible: nodeMenu.pane._menuNodeType === "server"
        danger: true
        onTriggered: nodeMenu.pane.requestDisconnectServer(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxRawName)
    }

    // ---- REPLICA SET FOLDER -----------------------------------------
    ActionItem {
        text: qsTr("Status of Replica Set")
        visible: nodeMenu.pane._menuNodeType === "rsFolder"
        onTriggered: Backend.replicaSetStatus(nodeMenu.pane._ctxUuid)
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "rsFolder" }
    ActionItem {
        text: qsTr("Refresh")
        visible: nodeMenu.pane._menuNodeType === "rsFolder"
        onTriggered: Backend.refreshReplicaSetFolder(nodeMenu.pane._ctxUuid)
    }

    // ---- REPLICA SET MEMBER -----------------------------------------
    ActionItem {
        text: qsTr("Open Shell")
        visible: nodeMenu.pane._menuNodeType === "rsMember"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: Backend.openMemberShell(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxRawName, "", false)
    }
    ActionItem {
        text: qsTr("Member Monitor")
        visible: nodeMenu.pane._menuNodeType === "rsMember"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: Backend.openMemberMonitor(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxRawName)
    }
    ActionItem {
        text: qsTr("Open Direct Connection")
        visible: nodeMenu.pane._menuNodeType === "rsMember"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: Backend.openMemberDirectConnection(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxRawName)
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "rsMember" }
    ActionItem {
        text: qsTr("Server Status")
        visible: nodeMenu.pane._menuNodeType === "rsMember"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: Backend.openMemberShell(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxRawName, "db.serverStatus()", true)
    }
    ActionItem {
        text: qsTr("Host Info")
        visible: nodeMenu.pane._menuNodeType === "rsMember"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: Backend.openMemberShell(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxRawName, "db.hostInfo()", true)
    }
    ActionItem {
        text: qsTr("MongoDB Version")
        visible: nodeMenu.pane._menuNodeType === "rsMember"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: Backend.openMemberShell(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxRawName, "db.version()", true)
    }
    ActionItem {
        text: qsTr("Show Log")
        visible: nodeMenu.pane._menuNodeType === "rsMember"
        enabled: nodeMenu.pane._ctxOnline
        onTriggered: Backend.openMemberShell(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxRawName, "show log", true)
    }

    // ---- DATABASE NODE ----------------------------------------------
    ActionItem {
        text: qsTr("Open Shell")
        visible: nodeMenu.pane._menuNodeType === "database"
        onTriggered: Backend.openShell(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, "")
    }
    ActionItem {
        text: qsTr("Refresh")
        visible: nodeMenu.pane._menuNodeType === "database"
        onTriggered: Backend.refreshDatabase(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb)
    }
    ActionItem {
        text: qsTr("Copy Database Name")
        visible: nodeMenu.pane._menuNodeType === "database"
        onTriggered: Backend.copyToClipboard(nodeMenu.pane._ctxDb)
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "database" }
    ActionItem {
        text: qsTr("Create Collection…")
        visible: nodeMenu.pane._menuNodeType === "database"
        onTriggered: nodeMenu.pane.openCreateCollection(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, "standard")
    }
    ActionItem {
        text: qsTr("Create Read-Only View… (3.4+)")
        visible: nodeMenu.pane._menuNodeType === "database" && (nodeMenu.pane._ctxServerVer <= 0.0 || nodeMenu.pane._ctxServerVer >= 3.4)
        onTriggered: nodeMenu.pane.openCreateCollection(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, "view")
    }
    ActionItem {
        text: qsTr("Create Time Series… (5.0+)")
        visible: nodeMenu.pane._menuNodeType === "database" && (nodeMenu.pane._ctxServerVer <= 0.0 || nodeMenu.pane._ctxServerVer >= 5.0)
        onTriggered: nodeMenu.pane.openCreateCollection(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, "timeseries")
    }
    ActionItem {
        text: qsTr("Create Clustered Collection… (5.3+)")
        visible: nodeMenu.pane._menuNodeType === "database" && (nodeMenu.pane._ctxServerVer <= 0.0 || nodeMenu.pane._ctxServerVer >= 5.3)
        onTriggered: nodeMenu.pane.openCreateCollection(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, "clustered")
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "database" }
    ActionItem {
        text: qsTr("Users & Roles…")
        visible: nodeMenu.pane._menuNodeType === "database"
        onTriggered: nodeMenu.pane.openManageUsers(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb)
    }
    ActionItem {
        text: qsTr("Add Stored Function (system.js — Deprecated)…")
        visible: nodeMenu.pane._menuNodeType === "database"
        onTriggered: nodeMenu.pane.openAddFunction(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb)
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "database" }
    ActionItem {
        text: qsTr("Database Statistics")
        visible: nodeMenu.pane._menuNodeType === "database"
        onTriggered: Backend.openShellWithScript(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, "db.stats()", true)
    }
    ActionItem {
        text: qsTr("Collection Metadata (Infos)")
        visible: nodeMenu.pane._menuNodeType === "database"
        onTriggered: Backend.openShellWithScript(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, "db.getCollectionInfos()", true)
    }
    ActionItem {
        text: qsTr("Collections Statistics")
        visible: nodeMenu.pane._menuNodeType === "database"
        onTriggered: Backend.openShellWithScript(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, "db.printCollectionStats()", true)
    }
    ActionItem {
        text: qsTr("Analyze Schema of All Collections")
        visible: nodeMenu.pane._menuNodeType === "database"
        onTriggered: Backend.analyzeDatabaseSchema(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb)
    }
    ActionItem {
        text: qsTr("Profiling Status")
        visible: nodeMenu.pane._menuNodeType === "database"
        onTriggered: Backend.openShellWithScript(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, "db.getProfilingStatus()", true)
    }
    ActionItem {
        text: qsTr("Server Monitor & Operations")
        visible: nodeMenu.pane._menuNodeType === "database"
        onTriggered: Backend.openMonitor(nodeMenu.pane._ctxUuid)
    }
    ActionItem {
        text: qsTr("Current Operations")
        visible: nodeMenu.pane._menuNodeType === "database"
        onTriggered: Backend.openShellWithScript(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, "db.currentOp()", true)
    }
    // Removed in MongoDB 4.2+: only expose on legacy < 4.2 servers with explicit warning
    ActionItem {
        text: qsTr("Repair Database… (< 4.2)")
        visible: nodeMenu.pane._menuNodeType === "database" && nodeMenu.pane._ctxServerVer > 0.0 && nodeMenu.pane._ctxServerVer < 4.2
        onTriggered: {
            nodeMenu.pane.openDestructiveConfirm({
                action: "repairDb",
                uuid: nodeMenu.pane._ctxUuid,
                ns: nodeMenu.pane._ctxDb,
                title: qsTr("Repair Legacy Database"),
                acceptText: qsTr("Run repairDatabase()"),
                msg: qsTr("Run 'db.repairDatabase()' on '%1'?").arg(Language.isolate(nodeMenu.pane._ctxDb)),
                consequence: qsTr("repairDatabase() acquires an exclusive global/database write lock, rewrites data files on disk, and requires free disk space equal to the current dataset size.")
            })
        }
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "database" }
    ActionItem {
        //: Permanently deletes it from the server (MongoDB drop). Not drag and drop.
        text: qsTr("Drop Database…")
        danger: true
        visible: nodeMenu.pane._menuNodeType === "database"
        onTriggered: {
            nodeMenu.pane.openDestructiveConfirm({
                action: "dropDb",
                uuid: nodeMenu.pane._ctxUuid,
                ns: nodeMenu.pane._ctxDb,
                //: Permanently deletes it from the server (MongoDB drop). Not drag and drop.
                title: qsTr("Drop Database"),
                //: Permanently deletes it from the server (MongoDB drop). Not drag and drop.
                acceptText: qsTr("Drop Database"),
                msg: qsTr("Permanently drop database '%1'?").arg(Language.isolate(nodeMenu.pane._ctxDb)),
                consequence: qsTr("ALL collections, views, time series buckets, documents, and indexes inside '%1' will be permanently deleted from the server. This action cannot be undone.")
                             .arg(Language.isolate(nodeMenu.pane._ctxDb)),
                typeToConfirm: nodeMenu.pane._ctxDb
            })
        }
    }

    // ---- COLLECTION NODE --------------------------------------------
    ActionItem {
        text: qsTr("View Documents")
        visible: nodeMenu.pane._menuNodeType === "collection"
        onTriggered: Backend.openShell(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, nodeMenu.pane._ctxColl)
    }
    ActionItem {
        text: qsTr("Aggregation Pipeline…")
        visible: nodeMenu.pane._menuNodeType === "collection"
        onTriggered: Backend.openShellWithScript(
            nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb,
            "db.getCollection('" + nodeMenu.pane.escapeSingleQuotes(nodeMenu.pane._ctxColl) + "').aggregate([\n" +
            "    { \"$match\": {} },\n" +
            "    { \"$limit\": 50 }\n" +
            "]);\n",
            false)
    }
    ActionItem {
        text: qsTr("Copy Collection Name")
        visible: nodeMenu.pane._menuNodeType === "collection"
        onTriggered: Backend.copyToClipboard(nodeMenu.pane._ctxColl)
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection }
    ActionItem {
        text: qsTr("Insert Document…")
        visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection
        onTriggered: {
            docEditorDialog.get().openForInsert(
                null, Backend.serverAddress(nodeMenu.pane._ctxUuid),
                nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, nodeMenu.pane._ctxColl, "{\n    \n}")
        }
    }
    ActionItem {
        text: qsTr("Update Documents…")
        visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection
        onTriggered: Backend.openShellWithScript(
            nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb,
            "db.getCollection('" + nodeMenu.pane.escapeSingleQuotes(nodeMenu.pane._ctxColl) + "').updateMany(\n" +
            "    { \"key\": \"value\" },\n" +
            "    { \"$set\": { } }\n" +
            ");\n",
            false)
    }
    ActionItem {
        text: qsTr("Remove Documents…")
        visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection
        onTriggered: Backend.openShellWithScript(
            nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb,
            "db.getCollection('" + nodeMenu.pane.escapeSingleQuotes(nodeMenu.pane._ctxColl) + "').deleteMany({ \"key\": \"value\" });\n",
            false)
    }
    ActionItem {
        text: qsTr("Remove All Documents…")
        danger: true
        visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection
        onTriggered: {
            nodeMenu.pane.openDestructiveConfirm({
                action: "removeAllDocs",
                uuid: nodeMenu.pane._ctxUuid,
                ns: nodeMenu.pane._ctxDb + "." + nodeMenu.pane._ctxColl,
                title: qsTr("Remove All Documents"),
                acceptText: qsTr("Delete All Documents"),
                msg: qsTr("Delete ALL documents from '%1.%2'?").arg(Language.isolate(nodeMenu.pane._ctxDb)).arg(Language.isolate(nodeMenu.pane._ctxColl)),
                consequence: qsTr("Every document in '%1.%2' will be permanently deleted (collection structure and indexes are preserved). This action cannot be undone.")
                             .arg(Language.isolate(nodeMenu.pane._ctxDb)).arg(Language.isolate(nodeMenu.pane._ctxColl)),
                typeToConfirm: nodeMenu.pane._ctxColl
            })
        }
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "collection" }
    ActionItem {
        text: qsTr("Aggregation Builder…")
        visible: nodeMenu.pane._menuNodeType === "collection"
        onTriggered: nodeMenu.pane.pipelineBuilderRequested(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, nodeMenu.pane._ctxColl)
    }
    ActionItem {
        text: qsTr("Analyze Schema…")
        visible: nodeMenu.pane._menuNodeType === "collection"
        onTriggered: nodeMenu.pane.schemaAnalysisRequested(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, nodeMenu.pane._ctxColl)
    }
    ActionItem {
        text: qsTr("Ask AI for a Query…")
        visible: nodeMenu.pane._menuNodeType === "collection"
        onTriggered: nodeMenu.pane.aiAssistantRequested(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, nodeMenu.pane._ctxColl)
    }
    ActionItem {
        text: qsTr("Export Collection…")
        visible: nodeMenu.pane._menuNodeType === "collection"
        onTriggered: nodeMenu.pane.openExportDialog(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, nodeMenu.pane._ctxColl, "{}")
    }
    ActionItem {
        text: qsTr("Import Data…")
        visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection
        onTriggered: nodeMenu.pane.openImportDialog(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, nodeMenu.pane._ctxColl)
    }
    ActionItem {
        text: qsTr("Add Index…")
        visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection
        onTriggered: nodeMenu.pane.openAddIndex(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, nodeMenu.pane._ctxColl)
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection }
    ActionItem {
        text: qsTr("Rename Collection…")
        visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection
        onTriggered: {
            nodeMenu.pane._ctxAction = "renameColl"
            nameDialog.title = qsTr("Rename Collection")
            nameDialog.label = qsTr("New collection name:")
            nameDialog.initialText = nodeMenu.pane._ctxColl
            nameDialog.open()
        }
    }
    ActionItem {
        text: qsTr("Duplicate Collection…")
        visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection
        onTriggered: {
            nodeMenu.pane._ctxAction = "dupColl"
            nameDialog.title = qsTr("Duplicate Collection")
            nameDialog.label = qsTr("Target collection name:")
            nameDialog.initialText = nodeMenu.pane._ctxColl + "_copy"
            nameDialog.open()
        }
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection }
    ActionItem {
        text: qsTr("Statistics")
        visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection
        onTriggered: Backend.openShellWithScript(
            nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb,
            "db.getCollection('" + nodeMenu.pane.escapeSingleQuotes(nodeMenu.pane._ctxColl) + "').stats()",
            true)
    }
    ActionItem {
        text: qsTr("Latency Histogram ($collStats)")
        visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection && (nodeMenu.pane._ctxServerVer <= 0.0 || nodeMenu.pane._ctxServerVer >= 3.2)
        onTriggered: Backend.openShellWithScript(
            nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb,
            "db.getCollection('" + nodeMenu.pane.escapeSingleQuotes(nodeMenu.pane._ctxColl) + "').latencyStats()",
            true)
    }
    ActionItem {
        text: qsTr("Validate Collection")
        visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection
        onTriggered: Backend.openShellWithScript(
            nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb,
            "db.getCollection('" + nodeMenu.pane.escapeSingleQuotes(nodeMenu.pane._ctxColl) + "').validate()",
            true)
    }
    ActionItem {
        text: qsTr("Storage Size")
        visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection
        onTriggered: Backend.openShellWithScript(
            nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb,
            "db.getCollection('" + nodeMenu.pane.escapeSingleQuotes(nodeMenu.pane._ctxColl) + "').storageSize()",
            true)
    }
    ActionItem {
        text: qsTr("Total Index Size")
        visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection
        onTriggered: Backend.openShellWithScript(
            nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb,
            "db.getCollection('" + nodeMenu.pane.escapeSingleQuotes(nodeMenu.pane._ctxColl) + "').totalIndexSize()",
            true)
    }
    ActionItem {
        text: qsTr("Shard Distribution")
        visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection
        onTriggered: Backend.openShellWithScript(
            nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb,
            "db.getCollection('" + nodeMenu.pane.escapeSingleQuotes(nodeMenu.pane._ctxColl) + "').getShardDistribution()",
            true)
    }
    ActionItem {
        text: qsTr("Compact Collection…")
        visible: nodeMenu.pane._menuNodeType === "collection" && !nodeMenu._isViewCollection
        onTriggered: {
            nodeMenu.pane.openDestructiveConfirm({
                action: "compactColl",
                uuid: nodeMenu.pane._ctxUuid,
                ns: nodeMenu.pane._ctxDb + "." + nodeMenu.pane._ctxColl,
                title: qsTr("Compact Collection"),
                acceptText: qsTr("Run Compact"),
                msg: qsTr("Compact storage pages for '%1.%2'?").arg(Language.isolate(nodeMenu.pane._ctxDb)).arg(Language.isolate(nodeMenu.pane._ctxColl)),
                consequence: qsTr("WiredTiger rewrites and defragments data and index blocks to release unused disk space to the OS. Intensive disk I/O may impact active workloads.")
            })
        }
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "collection" }
    ActionItem {
        //: Permanently deletes it from the server (MongoDB drop). Not drag and drop.
        text: nodeMenu._isViewCollection ? qsTr("Drop View…") : qsTr("Drop Collection…")
        danger: true
        visible: nodeMenu.pane._menuNodeType === "collection"
        onTriggered: {
            nodeMenu.pane.openDestructiveConfirm({
                action: "dropColl",
                uuid: nodeMenu.pane._ctxUuid,
                ns: nodeMenu.pane._ctxDb + "." + nodeMenu.pane._ctxColl,
                //: Permanently deletes it from the server (MongoDB drop). Not drag and drop.
                title: nodeMenu._isViewCollection ? qsTr("Drop View") : qsTr("Drop Collection"),
                //: Permanently deletes it from the server (MongoDB drop). Not drag and drop.
                acceptText: nodeMenu._isViewCollection ? qsTr("Drop View") : qsTr("Drop Collection"),
                msg: nodeMenu._isViewCollection
                     ? qsTr("Permanently drop read-only view '%1.%2'?").arg(Language.isolate(nodeMenu.pane._ctxDb)).arg(Language.isolate(nodeMenu.pane._ctxColl))
                     : qsTr("Permanently drop collection '%1.%2'?").arg(Language.isolate(nodeMenu.pane._ctxDb)).arg(Language.isolate(nodeMenu.pane._ctxColl)),
                consequence: nodeMenu._isViewCollection
                             ? qsTr("View definition '%1.%2' will be permanently removed (the underlying source collection is unaffected). This action cannot be undone.")
                               .arg(Language.isolate(nodeMenu.pane._ctxDb)).arg(Language.isolate(nodeMenu.pane._ctxColl))
                             : qsTr("All documents and custom indexes in '%1.%2' will be permanently erased from the server. This action cannot be undone.")
                               .arg(Language.isolate(nodeMenu.pane._ctxDb)).arg(Language.isolate(nodeMenu.pane._ctxColl)),
                typeToConfirm: nodeMenu.pane._ctxColl
            })
        }
    }

    // ---- INDEXES FOLDER ---------------------------------------------
    ActionItem {
        text: qsTr("View Indexes")
        visible: nodeMenu.pane._menuNodeType === "indexesFolder"
        onTriggered: Backend.openShellWithScript(
            nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb,
            "db.getCollection('" + nodeMenu.pane.escapeSingleQuotes(nodeMenu.pane._ctxColl) + "').getIndexes()",
            true)
    }
    ActionItem {
        text: qsTr("Add Index…")
        visible: nodeMenu.pane._menuNodeType === "indexesFolder"
        onTriggered: nodeMenu.pane.openAddIndex(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, nodeMenu.pane._ctxColl)
    }
    // Removed on mongod in MongoDB 6.0+: only expose on < 6.0 with explicit warning
    ActionItem {
        text: qsTr("Rebuild Indexes (reIndex)… (< 6.0)")
        visible: nodeMenu.pane._menuNodeType === "indexesFolder" && (nodeMenu.pane._ctxServerVer <= 0.0 || nodeMenu.pane._ctxServerVer < 6.0)
        onTriggered: {
            nodeMenu.pane.openDestructiveConfirm({
                action: "reIndex",
                uuid: nodeMenu.pane._ctxUuid,
                ns: nodeMenu.pane._ctxDb + "." + nodeMenu.pane._ctxColl,
                title: qsTr("Rebuild All Indexes (reIndex)"),
                acceptText: qsTr("Rebuild Indexes"),
                msg: qsTr("Drop and rebuild all indexes on '%1.%2'?").arg(Language.isolate(nodeMenu.pane._ctxDb)).arg(Language.isolate(nodeMenu.pane._ctxColl)),
                consequence: qsTr("reIndex() drops all existing indexes on the collection and rebuilds them from scratch, blocking collection operations on standalone servers (not supported on MongoDB 6.0+).")
            })
        }
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "indexesFolder" }
    ActionItem {
        text: qsTr("Refresh")
        visible: nodeMenu.pane._menuNodeType === "indexesFolder"
        onTriggered: Backend.refreshIndexes(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, nodeMenu.pane._ctxColl)
    }

    // ---- INDEX ITEM -------------------------------------------------
    ActionItem {
        text: qsTr("View Indexes")
        visible: nodeMenu.pane._menuNodeType === "index"
        onTriggered: Backend.openShellWithScript(
            nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb,
            "db.getCollection('" + nodeMenu.pane.escapeSingleQuotes(nodeMenu.pane._ctxColl) + "').getIndexes()",
            true)
    }
    ActionItem {
        text: qsTr("Edit Index…")
        visible: nodeMenu.pane._menuNodeType === "index"
        enabled: nodeMenu.pane._ctxRawName !== "_id_"
        onTriggered: nodeMenu.pane.openEditIndex(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb,
                                             nodeMenu.pane._ctxColl, nodeMenu.pane._ctxIndexMeta)
    }
    // MongoDB 4.4+ Hidden Index instant toggle via collMod
    ActionItem {
        readonly property bool isCurrentlyHidden: Boolean(nodeMenu.pane._ctxIndexMeta && nodeMenu.pane._ctxIndexMeta.hidden)
        text: isCurrentlyHidden
              ? qsTr("Unhide Index (4.4+)")
              : qsTr("Hide Index from Planner… (4.4+)")
        visible: nodeMenu.pane._menuNodeType === "index" &&
                 nodeMenu.pane._ctxRawName !== "_id_" &&
                 (nodeMenu.pane._ctxServerVer <= 0.0 || nodeMenu.pane._ctxServerVer >= 4.4)
        onTriggered: {
            if (isCurrentlyHidden) {
                Backend.setIndexHidden(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, nodeMenu.pane._ctxColl, nodeMenu.pane._ctxIndexMeta, false)
            } else {
                nodeMenu.pane.openDestructiveConfirm({
                    action: "hideIndex",
                    uuid: nodeMenu.pane._ctxUuid,
                    ns: nodeMenu.pane._ctxDb + "." + nodeMenu.pane._ctxColl + " [" + nodeMenu.pane._ctxRawName + "]",
                    title: qsTr("Hide Index from Query Planner"),
                    acceptText: qsTr("Hide Index"),
                    msg: qsTr("Hide index '%1' on '%2.%3'?").arg(Language.isolate(nodeMenu.pane._ctxRawName)).arg(Language.isolate(nodeMenu.pane._ctxDb)).arg(Language.isolate(nodeMenu.pane._ctxColl)),
                    consequence: qsTr("MongoDB 4.4+ will keep maintaining this index on writes, but queries will immediately stop using it — useful to verify impact before dropping. You can unhide it instantly anytime.")
                })
            }
        }
    }
    ActionItem {
        text: qsTr("Copy Index Name")
        visible: nodeMenu.pane._menuNodeType === "index"
        onTriggered: Backend.copyToClipboard(nodeMenu.pane._ctxRawName)
    }
    ActionItem {
        text: qsTr("Copy Keys JSON")
        visible: nodeMenu.pane._menuNodeType === "index" && Boolean(nodeMenu.pane._ctxIndexMeta && nodeMenu.pane._ctxIndexMeta.keys)
        onTriggered: Backend.copyToClipboard(String(nodeMenu.pane._ctxIndexMeta.keys || ""))
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "index" }
    ActionItem {
        //: Permanently deletes it from the server (MongoDB drop). Not drag and drop.
        text: qsTr("Drop Index…")
        danger: true
        visible: nodeMenu.pane._menuNodeType === "index"
        enabled: nodeMenu.pane._ctxRawName !== "_id_"
        onTriggered: {
            var kJson = (nodeMenu.pane._ctxIndexMeta && nodeMenu.pane._ctxIndexMeta.keys)
                        ? String(nodeMenu.pane._ctxIndexMeta.keys).replace(/\s+/g, " ").trim()
                        : ""
            nodeMenu.pane.openDestructiveConfirm({
                action: "dropIndex",
                uuid: nodeMenu.pane._ctxUuid,
                ns: nodeMenu.pane._ctxDb + "." + nodeMenu.pane._ctxColl,
                //: Permanently deletes it from the server (MongoDB drop). Not drag and drop.
                title: qsTr("Drop Index"),
                //: Permanently deletes it from the server (MongoDB drop). Not drag and drop.
                acceptText: qsTr("Drop Index"),
                msg: qsTr("Permanently drop index '%1' from '%2.%3'?")
                     .arg(Language.isolate(nodeMenu.pane._ctxRawName)).arg(Language.isolate(nodeMenu.pane._ctxDb)).arg(Language.isolate(nodeMenu.pane._ctxColl)),
                consequence: (kJson.length > 0 ? qsTr("Keys: %1\n").arg(kJson) : "") +
                             qsTr("Queries relying on this index may fall back to full collection scans (COLLSCAN), and rebuilding it later on large collections can take time.")
            })
        }
    }

    // ---- FUNCTIONS FOLDER -------------------------------------------
    ActionItem {
        text: qsTr("View Functions")
        visible: nodeMenu.pane._menuNodeType === "functionsFolder"
        onTriggered: Backend.openShellWithScript(
            nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, "db.system.js.find()", true)
    }
    ActionItem {
        text: qsTr("Add Function…")
        visible: nodeMenu.pane._menuNodeType === "functionsFolder"
        onTriggered: nodeMenu.pane.openAddFunction(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb)
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "functionsFolder" }
    ActionItem {
        text: qsTr("Refresh")
        visible: nodeMenu.pane._menuNodeType === "functionsFolder"
        onTriggered: Backend.refreshFunctions(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb)
    }

    // ---- FUNCTION ITEM ----------------------------------------------
    ActionItem {
        text: qsTr("Edit Function…")
        visible: nodeMenu.pane._menuNodeType === "function"
        onTriggered: nodeMenu.pane.openEditFunction(nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb, nodeMenu.pane._ctxIndexMeta)
    }
    ActionItem {
        text: qsTr("View in Shell")
        visible: nodeMenu.pane._menuNodeType === "function"
        onTriggered: Backend.openShellWithScript(
            nodeMenu.pane._ctxUuid, nodeMenu.pane._ctxDb,
            "db.system.js.find({ _id: \"" + nodeMenu.pane.escapeDoubleQuotes(nodeMenu.pane._ctxRawName) + "\" })",
            true)
    }
    ActionItem {
        text: qsTr("Copy Function Name")
        visible: nodeMenu.pane._menuNodeType === "function"
        onTriggered: Backend.copyToClipboard(nodeMenu.pane._ctxRawName)
    }
    ActionSeparator { visible: nodeMenu.pane._menuNodeType === "function" }
    ActionItem {
        text: qsTr("Remove Function…")
        danger: true
        visible: nodeMenu.pane._menuNodeType === "function"
        onTriggered: {
            nodeMenu.pane.openDestructiveConfirm({
                action: "dropFunc",
                uuid: nodeMenu.pane._ctxUuid,
                ns: nodeMenu.pane._ctxDb + ".system.js",
                title: qsTr("Remove Stored Function"),
                acceptText: qsTr("Remove Function"),
                msg: qsTr("Permanently remove stored JavaScript function '%1' from '%2'?")
                     .arg(Language.isolate(nodeMenu.pane._ctxRawName)).arg(Language.isolate(nodeMenu.pane._ctxDb)),
                consequence: qsTr("The function document will be deleted from '%1.system.js'. This action cannot be undone.")
                             .arg(Language.isolate(nodeMenu.pane._ctxDb))
            })
        }
    }
}
