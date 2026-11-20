pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Left pane: saved-connections list + connected servers explorer tree.
Item {
    id: pane

    signal newConnectionRequested()
    signal editConnectionRequested(int row)
    signal pipelineBuilderRequested(string uuid, string database, string collection)
    signal schemaAnalysisRequested(string uuid, string database, string collection)
    signal aiAssistantRequested(string uuid, string database, string collection)

    // Context of the tree node or connection a menu/dialog action targets.
    property string _menuNodeType: ""
    property string _ctxUuid: ""
    property double _ctxServerVer: 0.0
    property string _ctxServerVerText: ""
    property string _ctxDb: ""
    property string _ctxColl: ""
    property string _ctxRawName: ""
    property bool _ctxOnline: true
    property var _ctxIndexMeta: ({})
    property int _ctxConnRow: -1
    property string _ctxConnUuid: ""
    property string _ctxConnName: ""
    property bool _ctxConnOnline: false
    property string _ctxAction: ""


    function openDestructiveConfirm(opts) {
        confirmDialog.openWithOptions(opts)
    }

    function promptName(action, titleText, labelText, initial) {
        pane._ctxAction = action
        nameDialog.title = titleText
        nameDialog.label = labelText
        nameDialog.initialText = initial
        nameDialog.open()
    }

    function openCreateCollection(uuid, db, mode) {
        createCollDialog.get().openForDatabase(uuid, db, mode)
    }

    function openInsertDocument(uuid, db, coll) {
        docEditorDialog.get().openForInsert(
            null, Backend.serverAddress(uuid), uuid, db, coll, "{\n    \n}")
    }

    function openAddIndex(uuid, db, coll) { indexDialog.get().openForAdd(uuid, db, coll) }
    function openEditIndex(uuid, db, coll, meta) { indexDialog.get().openForEdit(uuid, db, coll, meta) }
    function openAddFunction(uuid, db) { funcEditorDialog.get().openForAdd(uuid, db) }
    function openEditFunction(uuid, db, meta) { funcEditorDialog.get().openForEdit(uuid, db, meta) }
    function openManageUsers(uuid, db) { usersManagerDialog.get().openForDatabase(uuid, db) }
    function openAddUser(uuid, db) { userEditorDialog.get().openForAdd(uuid, db) }
    function openEditUser(uuid, db, meta) { userEditorDialog.get().openForEdit(uuid, db, meta) }
    function requestDisconnectServer(uuid, fallbackName) {
        if (!uuid || uuid.length === 0)
            return
        var openCnt = Backend.openTabsCountForServer(uuid)
        if (openCnt <= 0) {
            Backend.disconnectServer(uuid, false)
            return
        }
        var dirtyCnt = Backend.dirtyShellsCountForServer(uuid)
        pane._ctxUuid = uuid
        openDestructiveConfirm({
            action: "disconnectConn",
            uuid: uuid,
            serverLabel: fallbackName || Backend.serverAddress(uuid),
            title: qsTr("Disconnect Server"),
            acceptText: qsTr("Disconnect"),
            msg: qsTr("Disconnect from '%1'?").arg(Language.isolate(fallbackName || Backend.serverAddress(uuid))),
            consequence: dirtyCnt > 0
                ? qsTr("This server currently has %n open tab(s), including %1 Shell tab(s) with UNSAVED script edits. Live Monitor tabs will close immediately.", "", openCnt)
                      .arg(dirtyCnt)
                : qsTr("This server currently has %n open Shell / Monitor tab(s). Live Monitor tabs will close immediately.", "", openCnt),
            showCloseShellsOption: true,
            defaultCloseShells: dirtyCnt === 0
        })
    }

    function openExportDialog(uuid, db, coll, filterJson) {
        importExportDialog.get().openExport(uuid, db, coll, filterJson || "{}")
    }

    function openImportDialog(uuid, db, coll) {
        importExportDialog.get().openImport(uuid, db, coll)
    }

    function escapeSingleQuotes(s) {
        return String(s).replace(/\\/g, "\\\\").replace(/'/g, "\\'")
    }

    function escapeDoubleQuotes(s) {
        return String(s).replace(/\\/g, "\\\\").replace(/"/g, "\\\"")
    }

    function badgeColorForKind(kind) {
        if (kind === "danger")
            return "#ff6b6b"
        if (kind === "warning")
            return "#f59e0b"
        if (kind === "info")
            return "#60a5fa"
        return Theme.accentColor
    }

    function iconGlyphForType(t, isRs, badgeText) {
        switch (t) {
        case "server":           return isRs ? "❖" : "◈"
        case "rsFolder":         return "⬡"
        case "rsMember":         return "●"
        case "systemDbFolder":
        case "systemCollFolder": return "⚙\uFE0E"
        case "database":         return "⛁"
        case "collection":       return badgeText === "VIEW" ? "◫" : "▤"
        case "indexesFolder":
        case "index":            return "≣"
        case "functionsFolder":
        case "function":         return "ƒ"
        default:                 return "·"
        }
    }

    function iconColorForNode(t, online, badgeKind, badgeText) {
        if (!online)
            return "#ff6b6b"
        switch (t) {
        case "server":          return Theme.accentColor
        case "rsFolder":        return Theme.accentColor
        case "rsMember":        return badgeKind === "accent" ? Theme.accentColor : "#60a5fa"
        case "database":        return "#34d399"
        case "collection":      return badgeText === "VIEW" ? "#60a5fa" : Theme.accentColor
        case "indexesFolder":
        case "index":           return "#f59e0b"
        case "functionsFolder":
        case "function":        return "#a78bfa"
        default:                return Theme.disabledTextColor
        }
    }

    function selectTreeRow(row) {
        if (row === undefined || row < 0 || row >= serverTree.rows)
            return
        serverTree.forceActiveFocus()
        treeSelectionModel.setCurrentIndex(
            serverTree.index(row, 0),
            ItemSelectionModel.Current | ItemSelectionModel.ClearAndSelect)
    }

    function openNodeContextMenu(delegateItem, targetItem, x, y) {
        if (!delegateItem)
            return
        pane.selectTreeRow(delegateItem.row)
        if (delegateItem.nodeType === "placeholder" ||
            delegateItem.nodeType === "systemDbFolder" ||
            delegateItem.nodeType === "systemCollFolder")
            return

        pane._menuNodeType = delegateItem.nodeType
        pane._ctxUuid = delegateItem.serverUuid
        pane._ctxServerVer = Backend.serverVersion(delegateItem.serverUuid)
        pane._ctxServerVerText = Backend.serverVersionString(delegateItem.serverUuid)
        pane._ctxRawName = delegateItem.rawName
        pane._ctxDb = delegateItem.dbName
        pane._ctxColl = delegateItem.collName
        pane._ctxOnline = delegateItem.online
        pane._ctxIndexMeta = delegateItem.indexMeta || ({})

        if (targetItem) {
            if (targetItem === delegateItem) {
                // Opened on the row delegate itself (right-click or long-press).
                // Position at the cursor/touch point (x, y), adjusted so it stays within window bounds.
                let posX = (x !== undefined) ? x : 16
                let posY = (y !== undefined) ? y : targetItem.height
                let rootPt = targetItem.mapToItem(null, posX, posY)
                let windowW = (Window.width > 0) ? Window.width : 800
                if (rootPt.x + nodeMenu.width > windowW - 8) {
                    let overflow = (rootPt.x + nodeMenu.width) - (windowW - 8)
                    posX = Math.max(8, posX - overflow)
                }
                nodeMenu.popup(targetItem, Math.max(8, posX), posY)
            } else {
                // Opened from an action button (e.g. the 3-dots button).
                // Align right edge of menu with right edge of button.
                let menuX = targetItem.width - nodeMenu.width
                nodeMenu.popup(targetItem, menuX, targetItem.height + 4)
            }
        } else {
            nodeMenu.popup(delegateItem, 16, delegateItem.height)
        }
    }

    function activateTreeRow(delegateItem) {
        if (!delegateItem || delegateItem.nodeType === "placeholder")
            return

        if (delegateItem.nodeType === "collection") {
            Backend.openShell(delegateItem.serverUuid,
                              delegateItem.dbName,
                              delegateItem.rawName)
        } else if (delegateItem.nodeType === "rsMember") {
            if (delegateItem.online)
                Backend.openMemberShell(delegateItem.serverUuid,
                                        delegateItem.rawName, "", false)
        } else if (delegateItem.nodeType === "index") {
            if (delegateItem.rawName !== "_id_") {
                indexDialog.get().openForEdit(delegateItem.serverUuid,
                                        delegateItem.dbName,
                                        delegateItem.collName,
                                        delegateItem.indexMeta)
            } else {
                Backend.openShellWithScript(
                    delegateItem.serverUuid, delegateItem.dbName,
                    "db.getCollection('" + pane.escapeSingleQuotes(delegateItem.collName) + "').getIndexes()",
                    true)
            }
        } else if (delegateItem.nodeType === "function") {
            funcEditorDialog.get().openForEdit(delegateItem.serverUuid,
                                         delegateItem.dbName,
                                         delegateItem.indexMeta)
        } else {
            serverTree.toggleExpanded(delegateItem.row)
            if (serverTree.isExpanded(delegateItem.row) && delegateItem.nodeType === "rsFolder")
                Backend.refreshReplicaSetFolder(delegateItem.serverUuid)
        }
    }

    Connections {
        target: Backend.serverTree

        function onServerPopulated(serverIndex) {
            Qt.callLater(function() {
                var cell = serverTree.cellAtIndex(serverIndex)
                if (cell.y >= 0 && !serverTree.isExpanded(cell.y))
                    serverTree.expand(cell.y)
            })
        }

        function onFolderExpandRequested(folderIndex) {
            Qt.callLater(function() {
                var cell = serverTree.cellAtIndex(folderIndex)
                if (cell.y >= 0 && !serverTree.isExpanded(cell.y))
                    serverTree.expand(cell.y)
            })
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        SavedConnectionsSection {
            Layout.fillWidth: true
            pane: pane
        }

        // ---- Connected servers tree -------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.bottomMargin: 6

            Label {
                text: qsTr("EXPLORER / CONNECTED")
                font.bold: true
                font.pixelSize: 11
                font.letterSpacing: Language.rtl ? 0 : 1.1
                color: Theme.disabledTextColor
                Layout.fillWidth: true
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            TreeView {
                id: serverTree
                anchors.fill: parent
                clip: true
                // Disable QQuickTreeView's built-in pointer navigation so Qt 6.12's
                // TreeViewDelegate does not swallow MouseButtonDblClick before onDoubleClicked.
                pointerNavigationEnabled: false
                model: Backend.serverTree
                selectionModel: ItemSelectionModel {
                    id: treeSelectionModel
                }

                columnWidthProvider: function(col) {
                    var sbw = treeVScroll.visible ? treeVScroll.width : 0
                    return Math.max(120, serverTree.width - sbw)
                }
                onWidthChanged: forceLayout()

                onExpanded: function(row, depth) {
                    Backend.serverTree.expandIndex(serverTree.index(row, 0))
                }

                Keys.onReturnPressed: {
                    var item = serverTree.itemAtCell(0, serverTree.currentRow)
                    if (item)
                        pane.activateTreeRow(item)
                }
                Keys.onEnterPressed: {
                    var item = serverTree.itemAtCell(0, serverTree.currentRow)
                    if (item)
                        pane.activateTreeRow(item)
                }

                delegate: TreeViewDelegate {
                    id: treeDelegate
                    width: serverTree.width - (treeVScroll.visible ? treeVScroll.width : 0)
                    implicitWidth: width
                    implicitHeight: Platform.isMobile ? 36 : 30
                    indentation: 14
                    leftMargin: 10
                    rightMargin: 8
                    // Mirrored (right-to-left): the indent moves to the right side, like
                    // the Basic style's own delegate.
                    leftPadding: mirrored ? 14 : leftMargin + __contentIndent
                    rightPadding: mirrored ? leftMargin + __contentIndent : 14
                    topPadding: 3
                    bottomPadding: 3

                    required property string name
                    required property string rawName
                    required property string nodeType
                    required property bool loading
                    required property string serverUuid
                    required property string dbName
                    required property string collName
                    required property string badgeText
                    required property string badgeKind
                    required property string subtitle
                    required property bool online
                    required property bool isReplicaSet
                    required property var indexMeta
                    required property string toolTipText
                    // Schema catalog / background collector (ServerTreeModel Schema* roles).
                    required property string schemaState
                    required property int schemaCount
                    required property int schemaTotal
                    required property var schemaAnalyzedAt
                    readonly property string schemaTip: {
                        if (nodeType === "collection" && schemaState.length > 0)
                            return qsTr("Schema: %1 documents sampled, %2")
                                .arg(schemaCount)
                                .arg(schemaAnalyzedAt ? schemaAnalyzedAt.toLocaleString(Qt.locale(), Locale.ShortFormat) : "")
                        if (nodeType === "database" && schemaState === "running" && schemaTotal >= 0)
                            return qsTr("Schema analysis: %1 of %2 collections").arg(schemaCount).arg(schemaTotal)
                        return ""
                    }

                    readonly property bool hasActions:
                        nodeType !== "placeholder" &&
                        nodeType !== "systemDbFolder" &&
                        nodeType !== "systemCollFolder"
                    readonly property string serverColorTag:
                        nodeType === "server"
                        ? ConnectionModel.colorTagForUuid(serverUuid, ConnectionModel.revision)
                        : ""
                    readonly property bool hasServerTint: serverColorTag.length > 0
                    readonly property color parsedServerTint:
                        hasServerTint ? Qt.color(serverColorTag) : "transparent"

                    indicator: Item {
                        readonly property real indent: treeDelegate.leftMargin + (treeDelegate.depth * treeDelegate.indentation)
                        x: treeDelegate.mirrored ? treeDelegate.width - indent - width : indent
                        y: 0
                        width: 16
                        height: treeDelegate.height
                        visible: treeDelegate.hasChildren

                        Label {
                            anchors.centerIn: parent
                            text: treeDelegate.expanded ? "▾" : (Language.rtl ? "◂" : "▸")
                            font.pixelSize: 11
                            color: Theme.disabledTextColor
                        }

                        MouseArea {
                            anchors.fill: parent
                            anchors.leftMargin: -3
                            anchors.rightMargin: -3
                            acceptedButtons: Qt.LeftButton
                            cursorShape: Qt.PointingHandCursor
                            onPressed: {
                                treeDelegate._longPressTriggered = false
                                pane.selectTreeRow(treeDelegate.row)
                            }
                            onClicked: {
                                if (treeDelegate._longPressTriggered) {
                                    treeDelegate._longPressTriggered = false
                                    return
                                }
                                serverTree.toggleExpanded(treeDelegate.row)
                                if (serverTree.isExpanded(treeDelegate.row) && treeDelegate.nodeType === "rsFolder")
                                    Backend.refreshReplicaSetFolder(treeDelegate.serverUuid)
                            }
                            onDoubleClicked: {
                                // Absorb fast second click on the arrow so the first click's toggle stays intact.
                            }
                        }
                    }

                    contentItem: RowLayout {
                        spacing: 6
                        clip: true
                        opacity: treeDelegate.online ? 1.0 : 0.55

                        Label {
                            text: pane.iconGlyphForType(treeDelegate.nodeType,
                                                        treeDelegate.isReplicaSet,
                                                        treeDelegate.badgeText)
                            font.pixelSize: treeDelegate.nodeType === "rsMember" ? 9 : 12
                            font.bold: treeDelegate.nodeType === "function"
                            color: treeDelegate.hasServerTint && treeDelegate.online
                                   ? treeDelegate.parsedServerTint
                                   : pane.iconColorForNode(treeDelegate.nodeType,
                                                           treeDelegate.online,
                                                           treeDelegate.badgeKind,
                                                           treeDelegate.badgeText)
                            Layout.alignment: Qt.AlignVCenter
                        }

                        Label {
                            text: treeDelegate.name + (treeDelegate.loading && treeDelegate.nodeType !== "placeholder" ? " …" : "")
                            font.pixelSize: 13
                            font.bold: treeDelegate.nodeType === "server" || treeDelegate.nodeType === "database"
                            font.italic: treeDelegate.nodeType === "placeholder"
                            color: treeDelegate.nodeType === "placeholder"
                                   ? Theme.disabledTextColor
                                   : Theme.textColor
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            // Explicit "left" = the start side: it flips to the right in
                            // right-to-left layouts, also for Latin names.
                            horizontalAlignment: Text.AlignLeft
                            Layout.fillWidth: treeDelegate.subtitle.length === 0
                            Layout.minimumWidth: 28
                            Layout.maximumWidth: treeDelegate.subtitle.length > 0 ? 140 : -1
                        }

                        // Collection with a schema snapshot: a dot (dim when stale).
                        Rectangle {
                            visible: treeDelegate.nodeType === "collection" && treeDelegate.schemaState.length > 0
                            Layout.preferredWidth: 6
                            Layout.preferredHeight: 6
                            Layout.alignment: Qt.AlignVCenter
                            radius: 3
                            color: treeDelegate.schemaState === "fresh" ? Theme.accentColor : Theme.disabledTextColor
                        }

                        // Database being analyzed in the background: analyzed / total.
                        Label {
                            visible: treeDelegate.nodeType === "database" && treeDelegate.schemaState === "running"
                            text: treeDelegate.schemaTotal >= 0
                                  ? "◷ " + treeDelegate.schemaCount + "/" + treeDelegate.schemaTotal : "◷"
                            font.pixelSize: 11
                            color: Theme.disabledTextColor
                            Layout.alignment: Qt.AlignVCenter
                        }

                        Label {
                            visible: treeDelegate.subtitle.length > 0
                            text: treeDelegate.subtitle
                            font.family: AppSettings.codeFontFamily
                            font.pixelSize: 11
                            color: Theme.disabledTextColor
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            horizontalAlignment: Text.AlignLeft
                            Layout.fillWidth: true
                            Layout.minimumWidth: 20
                        }

                        RagoBadge {
                            visible: treeDelegate.badgeText.length > 0
                            text: treeDelegate.badgeText
                            badgeColor: pane.badgeColorForKind(treeDelegate.badgeKind)
                            Layout.alignment: Qt.AlignVCenter
                        }

                        Rectangle {
                            id: rowActionBtn
                            visible: treeDelegate.hasActions &&
                                     (Platform.isMobile || treeDelegate.hovered || treeDelegate.current || treeDelegate.selected)
                            Layout.preferredWidth: Platform.isMobile ? 26 : 20
                            Layout.preferredHeight: Platform.isMobile ? 26 : 20
                            Layout.alignment: Qt.AlignVCenter
                            radius: Platform.isMobile ? 7 : 6
                            color: rowActionMouse.containsMouse
                                   ? (Theme.dark ? Qt.rgba(1, 1, 1, 0.20) : Qt.rgba(0, 0, 0, 0.12))
                                   : (Theme.dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.05))
                            border.width: 1
                            border.color: rowActionMouse.containsMouse
                                          ? (treeDelegate.hasServerTint ? treeDelegate.parsedServerTint : Theme.accentColor)
                                          : (Theme.dark ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(0, 0, 0, 0.10))

                            Behavior on color { ColorAnimation { duration: 100 } }
                            Behavior on border.color { ColorAnimation { duration: 100 } }

                            Label {
                                anchors.centerIn: parent
                                anchors.verticalCenterOffset: -1
                                text: "⋮"
                                font.pixelSize: Platform.isMobile ? 15 : 13
                                font.bold: true
                                color: rowActionMouse.containsMouse
                                       ? (treeDelegate.hasServerTint ? treeDelegate.parsedServerTint : Theme.accentColor)
                                       : Theme.textColor
                                opacity: rowActionMouse.containsMouse ? 1.0 : 0.78
                            }

                            MouseArea {
                                id: rowActionMouse
                                anchors.fill: parent
                                hoverEnabled: !Platform.isMobile
                                cursorShape: Qt.PointingHandCursor
                                onClicked: pane.openNodeContextMenu(treeDelegate, rowActionBtn)
                                ToolTip.visible: containsMouse
                                ToolTip.delay: 350
                                ToolTip.text: qsTr("Actions")
                            }
                        }
                    }

                    ToolTip {
                        id: nodeToolTip
                        visible: treeDelegate.hovered && text.length > 0
                        delay: 350
                        timeout: 12000
                        padding: 10
                        text: treeDelegate.toolTipText.length > 0 && treeDelegate.schemaTip.length > 0
                              ? treeDelegate.toolTipText + "\n" + treeDelegate.schemaTip
                              : treeDelegate.toolTipText + treeDelegate.schemaTip

                        contentItem: Label {
                            text: nodeToolTip.text
                            font.family: AppSettings.codeFontFamily
                            font.pixelSize: 12
                            color: Theme.textColor
                        }

                        background: Rectangle {
                            radius: 8
                            color: Theme.surfaceColor
                            border.width: 1
                            border.color: Theme.glassBorderHover
                        }
                    }

                    background: Rectangle {
                        radius: 7
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 8
                        anchors.topMargin: 1
                        anchors.bottomMargin: 1
                        color: treeDelegate.hasServerTint
                               ? Qt.rgba(treeDelegate.parsedServerTint.r,
                                         treeDelegate.parsedServerTint.g,
                                         treeDelegate.parsedServerTint.b,
                                         (treeDelegate.current || treeDelegate.selected)
                                         ? (Theme.dark ? 0.22 : 0.16)
                                         : (treeDelegate.hovered
                                            ? (Theme.dark ? 0.16 : 0.12)
                                            : (Theme.dark ? 0.10 : 0.08)))
                               : (treeDelegate.current || treeDelegate.selected
                                  ? Theme.selectedColor
                                  : (treeDelegate.hovered ? Theme.cardBackgroundHover : "transparent"))
                        border.width: treeDelegate.hasServerTint ? 1 : 0
                        border.color: treeDelegate.hasServerTint
                                      ? Qt.rgba(treeDelegate.parsedServerTint.r,
                                                treeDelegate.parsedServerTint.g,
                                                treeDelegate.parsedServerTint.b,
                                                (treeDelegate.current || treeDelegate.selected || treeDelegate.hovered)
                                                ? 0.85 : 0.55)
                                      : "transparent"
                    }

                    onExpandedChanged: {
                        if (treeDelegate.expanded)
                            Backend.serverTree.expandIndex(serverTree.index(treeDelegate.row, 0))
                    }

                    property bool _longPressTriggered: false

                    onPressed: {
                        treeDelegate._longPressTriggered = false
                        pane.selectTreeRow(treeDelegate.row)
                    }

                    onPressedChanged: {
                        if (treeDelegate.pressed)
                            treeDelegate._longPressTriggered = false
                    }

                    // Desktop double click activates row (opens shell or toggles expand).
                    onDoubleClicked: pane.activateTreeRow(treeDelegate)

                    // Mobile single tap activates row (opens collection shell or toggles expand).
                    onClicked: {
                        if (treeDelegate._longPressTriggered) {
                            treeDelegate._longPressTriggered = false
                            return
                        }
                        if (Platform.isMobile)
                            pane.activateTreeRow(treeDelegate)
                    }

                    // Desktop right click opens context menu at cursor position.
                    TapHandler {
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.Stylus
                        acceptedButtons: Qt.RightButton
                        onTapped: (point) => pane.openNodeContextMenu(treeDelegate, treeDelegate, point.position.x, point.position.y)
                    }

                    // Mobile long press opens context menu at touched position.
                    // Restricted to TouchScreen/Stylus so desktop mouse double-click is never interrupted.
                    TapHandler {
                        id: treeLongPress
                        acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
                        acceptedButtons: Qt.LeftButton
                        longPressThreshold: 0.8
                        onPressedChanged: {
                            if (pressed)
                                treeDelegate._longPressTriggered = false
                        }
                        onLongPressed: {
                            treeDelegate._longPressTriggered = true
                            pane.openNodeContextMenu(treeDelegate, treeDelegate, treeLongPress.point.position.x, treeLongPress.point.position.y)
                        }
                    }
                }

                ScrollBar.vertical: ScrollBar {
                    id: treeVScroll
                    policy: ScrollBar.AsNeeded
                    onVisibleChanged: serverTree.forceLayout()
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 36
                visible: Backend.connectedUuids.length === 0
                text: qsTr("Connect to a server above (▶ or double-click) to browse Replica Sets, Databases, Collections, and Indexes.")
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 12
                color: Theme.disabledTextColor
            }
        }

        // ---- Status bar ---------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.glassBorder
        }
        Label {
            id: statusBarLabel
            // Errors arrive multi-line; elide only works on one line, so
            // flatten it here and keep the full text in the tooltip.
            text: Backend.statusText.length > 0
                  ? Backend.statusText.replace(/\s*\n\s*/g, " ")
                  : qsTr("Ready")
            textFormat: Text.PlainText
            color: Theme.disabledTextColor
            font.pixelSize: 11
            maximumLineCount: 1
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 7
            Layout.bottomMargin: 7

            ToolTip.visible: statusBarHover.hovered && statusBarLabel.truncated
            ToolTip.delay: 400
            ToolTip.text: Backend.statusText

            HoverHandler { id: statusBarHover }
        }
    }


    ExplorerNodeMenu {
        id: nodeMenu
        pane: pane
    }

    // In-app modal dialogs for Explorer actions (created on first use)
    LazyDialog { id: docEditorDialog; sourceComponent: Component { DocumentEditorDialog {} } }

    // Dev/CI visual verification helper (--open-view doc); synthetic sample data only.
    function openDebugDocEditor() {
        var specsLines = []
        for (var i = 1; i <= 18; ++i) {
            var k = "spec" + (i < 10 ? "0" + i : String(i))
            specsLines.push("        \"" + k + "\" : " + (i % 3 === 0 ? String(i * 5) : (i % 2 === 0 ? "true" : "\"v" + i + "\"")))
        }
        docEditorDialog.get().openForEdit(
            null, 0, "localhost:27017", "catalog", "products",
            "{\n"
            + "    \"_id\" : ObjectId(\"ae6d52ae3767a24c0b7cdfde\"),\n"
            + "    \"sku\" : \"CAM-00005\",\n"
            + "    \"name\" : \"Zen Camera 13\",\n"
            + "    \"category\" : \"camera\",\n"
            + "    \"brand\" : \"Sony\",\n"
            + "    \"price\" : 1070.54,\n"
            + "    \"stock\" : 5,\n"
            + "    \"specs\" : {\n"
            + specsLines.join(",\n") + "\n"
            + "    }\n"
            + "}")
    }
    // Dev/CI helper for --open-view create-collection | index | user | users.
    function openDebugDialog(name) {
        var uuid = Backend.firstConnectedUuid()
        if (name === "create-collection")
            createCollDialog.get().openForDatabase(uuid, "samples", "view")
        else if (name === "index")
            indexDialog.get().openForAdd(uuid, "samples", "documents")
        else if (name === "user")
            userEditorDialog.get().openForAdd(uuid, "admin")
        else if (name === "users") {
            var uDlg = usersManagerDialog.get()
            uDlg.openForDatabase(uuid, "admin")
            if (uuid.length === 0) {
                uDlg.serverUuid = "debug-preview"
                uDlg.serverLabel = "localhost:27017"
                uDlg.serverVerText = "8.0.4"
                uDlg.users = [
                    { name: "ops_admin", mechanisms: ["SCRAM-SHA-256"], roles: ["root", "clusterMonitor"] },
                    { name: "catalog_service", mechanisms: ["SCRAM-SHA-256"], roles: ["readWrite@catalog", "read@analytics"] }
                ]
            }
        }
    }
    LazyDialog { id: createCollDialog; sourceComponent: Component { CreateCollectionDialog {} } }
    LazyDialog { id: usersManagerDialog; sourceComponent: Component { UsersManagerDialog { pane: pane } } }
    LazyDialog { id: userEditorDialog; sourceComponent: Component { UserEditorDialog {} } }
    LazyDialog { id: funcEditorDialog; sourceComponent: Component { FunctionEditorDialog {} } }
    LazyDialog { id: importExportDialog; sourceComponent: Component { ImportExportDialog {} } }
    LazyDialog { id: indexDialog; sourceComponent: Component { IndexDialog {} } }

    // Name prompt for create / rename / duplicate operations.
    InputDialog {
        id: nameDialog
        onSubmitted: function(text) {
            if (text.length === 0)
                return
            if (pane._ctxAction === "db")
                Backend.createDatabase(pane._ctxUuid, text)
            else if (pane._ctxAction === "renameColl")
                Backend.renameCollection(pane._ctxUuid, pane._ctxDb,
                                         pane._ctxColl, text)
            else if (pane._ctxAction === "dupColl")
                Backend.duplicateCollection(pane._ctxUuid, pane._ctxDb,
                                             pane._ctxColl, text)
        }
    }


    ExplorerConfirmDialog {
        id: confirmDialog
        pane: pane
    }
}
