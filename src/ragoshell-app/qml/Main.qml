pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs as QtDialogs

ApplicationWindow {
    id: root
    width: 1280
    height: 800
    // Narrow windows are fine (a desktop split screen, an iPad Slide Over): the explorer folds away below
    // narrowWidth and the toolbar buttons hide by width. Touch devices start from an even smaller minimum.
    minimumWidth: Theme.touch ? 320 : 480
    minimumHeight: Theme.touch ? 320 : 360
    readonly property int narrowWidth: Theme.touch ? 900 : 720
    readonly property bool narrow: width < narrowWidth
    property bool _explorerFoldedByWidth: false
    onNarrowChanged: {
        // Fold the explorer when the window becomes narrow and bring it back when it is wide again
        // (unless the user closed it themselves).
        if (narrow) {
            _explorerFoldedByWidth = explorerPane.visible
            explorerPane.visible = false
        } else {
            explorerDrawer.close()
            if (_explorerFoldedByWidth) {
                _explorerFoldedByWidth = false
                explorerPane.visible = true
            }
        }
    }
    visible: false // Shown by main.cpp after positioning on the active screen
    title: Brand.productName + " " + Backend.appVersion

    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos" || Qt.platform.os === "ios"

    color: Theme.windowColor

    // Right-to-left languages (Arabic) mirror anchors, rows and text alignment of
    // the whole window. Code-like areas opt out with RagoLtr (docs/Localization.md).
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    // Propagate live theme colors to Qt Quick Controls (ScrollBars, BusyIndicators, ToolTips, etc.)
    palette.window: Theme.windowColor
    palette.windowText: Theme.textColor
    palette.base: Theme.baseColor
    palette.alternateBase: Theme.alternateColor
    palette.text: Theme.textColor
    palette.button: Theme.buttonColor
    palette.buttonText: Theme.textColor
    palette.brightText: Theme.onAccentColor
    palette.highlight: Theme.accentColor
    palette.highlightedText: Theme.onAccentColor
    palette.placeholderText: Theme.disabledTextColor
    palette.toolTipBase: Theme.surfaceColor
    palette.toolTipText: Theme.textColor
    palette.mid: Theme.borderColor
    palette.dark: Theme.surfaceColor
    palette.light: Theme.cardBackgroundHover

    // Tab strip: Backend.tabs (C++ TabModel) holds the optional Welcome tab and
    // the shell / server-monitor tabs in opening order, plus the current tab.
    readonly property TabModel tabs: Backend.tabs
    property int _ctxTabIndex: -1

    // Unsaved script ("dirty") close confirmation state
    property string _pendingCloseAction: ""
    property int _pendingCloseTabIndex: -1
    property var _pendingDirtyTitles: []
    property bool _forceClosingWindow: false

    function _srgbToLinear(c: real): real {
        return c <= 0.03928 ? (c / 12.92) : Math.pow((c + 0.055) / 1.055, 2.4)
    }

    function colorLuminance(col: color): real {
        return 0.2126 * _srgbToLinear(col.r)
             + 0.7152 * _srgbToLinear(col.g)
             + 0.0722 * _srgbToLinear(col.b)
    }

    function blendColors(bg: color, fg: color, alpha: real): color {
        var a = Math.max(0.0, Math.min(1.0, alpha))
        return Qt.rgba(
            bg.r * (1.0 - a) + fg.r * a,
            bg.g * (1.0 - a) + fg.g * a,
            bg.b * (1.0 - a) + fg.b * a,
            1.0
        )
    }

    function invertedTextColorFor(bgCol: color): color {
        // Perceptual brightness (W3C YIQ + WCAG relative luminance) so saturated blues,
        // greens, purples, teals, and reds always get crisp white (#ffffff) while bright
        // yellows, limes, and sky tones get crisp dark ink (#0f172a).
        var yiq = 0.299 * bgCol.r + 0.587 * bgCol.g + 0.114 * bgCol.b
        var lum = colorLuminance(bgCol)
        return (yiq >= 0.60 || lum >= 0.36) ? Qt.color("#0f172a") : Qt.color("#ffffff")
    }

    function activeTabColor() {
        return tabs.currentColor.length > 0 ? Qt.color(tabs.currentColor) : Theme.accentColor
    }

    // Closes the tab in 'row' (the Welcome tab has no object).
    function _closeTabRow(row: int) {
        if (tabs.kindAt(row) === "welcome")
            tabs.welcomeOpen = false
        else
            Backend.closeTabObject(tabs.objectAt(row))
    }

    // Rows whose tabs "others" / "right" closes, relative to 'tabIdx'.
    function _closeTargets(action, tabIdx) {
        var rows = []
        for (var i = 0; i < tabs.count; ++i) {
            if ((action === "others" && i !== tabIdx) || (action === "right" && i > tabIdx))
                rows.push(i)
        }
        return rows
    }

    function _dirtyShellTitles(rows) {
        var names = []
        for (var i = 0; i < rows.length; ++i) {
            if (tabs.kindAt(rows[i]) !== "shell")
                continue
            var sh = tabs.objectAt(rows[i]) as ShellController
            if (sh && sh.dirty)
                names.push(sh.title || qsTr("Untitled Script"))
        }
        return names
    }

    function _performCloseActionNow(action: string, tabIdx: int) {
        if (action === "window") {
            // Leaving with unsaved tabs was confirmed: their drafts are not wanted either.
            Backend.discardOpenDrafts()
            _forceClosingWindow = true
            root.close()
            return
        }
        if (action === "single") {
            if (tabIdx >= 0 && tabIdx < tabs.count)
                _closeTabRow(tabIdx)
            return
        }
        // Right to left, so the rows still to close keep their positions.
        var rows = _closeTargets(action, tabIdx)
        for (var r = rows.length - 1; r >= 0; --r)
            _closeTabRow(rows[r])
        if (action === "others" && tabs.count > 0)
            tabs.currentIndex = 0
    }

    function _confirmOrClose(action, tabIdx, rows) {
        var dirtyNames = _dirtyShellTitles(rows)
        if (dirtyNames.length > 0) {
            _pendingCloseAction = action
            _pendingCloseTabIndex = tabIdx
            _pendingDirtyTitles = dirtyNames
            serviceDialogs.openUnsavedConfirm()
            return
        }
        _performCloseActionNow(action, tabIdx)
    }

    function closeTabAt(tabIdx: int) {
        if (tabIdx >= 0 && tabIdx < tabs.count)
            _confirmOrClose("single", tabIdx, [tabIdx])
    }

    function closeOtherTabsAt(tabIdx: int) {
        if (tabIdx >= 0 && tabIdx < tabs.count)
            _confirmOrClose("others", tabIdx, _closeTargets("others", tabIdx))
    }

    function closeTabsToRightAt(tabIdx: int) {
        if (tabIdx >= 0 && tabIdx < tabs.count - 1)
            _confirmOrClose("right", tabIdx, _closeTargets("right", tabIdx))
    }

    onClosing: function(close) {
        root._saveLayout()
        if (root._forceClosingWindow) {
            logWindow.close()   // or the app keeps running for the log window
            return
        }
        var dirtyNames = []
        for (var i = 0; i < Backend.shells.length; ++i) {
            var sh = Backend.shells[i]
            if (sh && sh.dirty)
                dirtyNames.push(sh.title || qsTr("Untitled Script"))
        }
        if (dirtyNames.length > 0) {
            close.accepted = false
            root._pendingCloseAction = "window"
            root._pendingCloseTabIndex = -1
            root._pendingDirtyTitles = dirtyNames
            serviceDialogs.openUnsavedConfirm()
        } else {
            logWindow.close()
        }
    }

    function reopenWelcome() {
        tabs.welcomeOpen = true
        tabs.currentIndex = 0
    }

    function triggerSaveCurrentScript(forceSaveAs: bool) {
        var p = currentShellPage()
        var s = currentShell()
        if (!p || !s)
            return
        if (!forceSaveAs && s.filePath && s.filePath.length > 0) {
            if (Backend.saveFile(s.filePath, p.scriptText()))
                p.markSaved(s.filePath)
        } else if (Qt.platform.os === "wasm") {
            // Browser save = download (docs/WebStorage.md §3, roadmap 8.2).
            var fp = String(s.filePath || "")
            var hint = fp.length > 0 ? fp.split('/').pop() : "script.js"
            if (Backend.saveFileAs(hint, p.scriptText()))
                p.markSaved(hint)
        } else {
            saveScriptDialog.open()
        }
    }

    Component.onCompleted: {
        workArea.syncCurrentTab()
        _restoreLayout()
        // Changes made while restoring are not user changes.
        Qt.callLater(function() { root._layoutReady = true })
        // Unsaved scripts of a session that ended badly: offer them once, after the
        // window is up. Nothing is connected until the user opens one.
        Qt.callLater(function() {
            if (Drafts.takeStartupPrompt())
                root.openRecoverDrafts()
        })
    }

    // Dev/CI visual verification & geometry checks (--open-view / --check-geometry).
    function openDebugView(viewName) { debugHarness.openDebugView(viewName) }
    function geometryReport() { return debugHarness.geometryReport() }

    // Current shell / monitor tab helpers (used by shortcuts + file dialogs + toolbar).
    function currentShell() {
        return tabs.currentKind === "shell" ? tabs.currentObject as ShellController : null
    }
    function currentShellIndex() {
        var s = currentShell()
        return s ? Backend.shellModel.indexOf(s) : -1
    }
    function currentShellPage() {
        var i = currentShellIndex()
        return i >= 0 ? workArea.shellPageAt(i) : null
    }
    function currentMonitor() {
        return tabs.currentKind === "monitor" ? tabs.currentObject as ServerMonitorController : null
    }
    function activeServerUuid() {
        var s = currentShell()
        if (s && s.serverUuid)
            return s.serverUuid
        var mon = currentMonitor()
        if (mon && mon.serverUuid) {
            var atIdx = String(mon.serverUuid).indexOf("@")
            return atIdx > 0 ? String(mon.serverUuid).substring(0, atIdx) : String(mon.serverUuid)
        }
        return Backend.firstConnectedUuid()
    }

    // ---- Top bar ------------------------------------------------------------
    // One bar for every platform (docs/DesignSystem.md, section 6); every command is also in the command palette.
    header: AppBar { id: appBar; app: root; actions: appActions }

    // ---- Narrow / Phone & Tablet Portrait Explorer Drawer ------------------
    Drawer {
        id: explorerDrawer
        edge: Language.rtl ? Qt.RightEdge : Qt.LeftEdge
        width: Math.min(340, Math.round(root.width * 0.85))
        height: root.height
        modal: true
        interactive: root.narrow
        topPadding: Overlay.overlay ? Overlay.overlay.SafeArea.margins.top : 0
        bottomPadding: Overlay.overlay ? Overlay.overlay.SafeArea.margins.bottom : 0
        leftPadding: edge === Qt.LeftEdge && Overlay.overlay ? Overlay.overlay.SafeArea.margins.left : 0
        rightPadding: edge === Qt.RightEdge && Overlay.overlay ? Overlay.overlay.SafeArea.margins.right : 0
        background: Rectangle {
            color: Theme.surfaceColor
            border.width: 1
            border.color: Theme.glassBorder
        }
    }

    Connections {
        target: tabs
        function onCurrentIndexChanged() {
            if (root.narrow && explorerDrawer.visible)
                explorerDrawer.close()
        }
    }

    // ---- Body -------------------------------------------------------------
    // Explorer | work area on top, the log panel below; both splitters and
    // the panels' visibility are remembered (Backend.uiState).
    SplitView {
        id: bodySplit
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: parent.SafeArea.margins.left
        anchors.rightMargin: parent.SafeArea.margins.right
        anchors.bottomMargin: parent.SafeArea.margins.bottom
        orientation: Qt.Vertical

        handle: Rectangle {
            implicitHeight: 4
            color: SplitHandle.pressed || SplitHandle.hovered
                   ? Theme.accentColor : Theme.glassBorder
        }

        SplitView {
            id: mainSplit
            SplitView.fillHeight: true
            SplitView.minimumHeight: 160
            orientation: Qt.Horizontal

            handle: Rectangle {
                implicitWidth: 4
                color: SplitHandle.pressed || SplitHandle.hovered
                       ? Theme.accentColor : Theme.glassBorder
            }

            Rectangle {
                id: explorerPane
                visible: !root.narrow && (Backend.uiState("explorerVisible") !== false)
                SplitView.preferredWidth: Math.min(320, Math.round(root.width * 0.36))
                SplitView.minimumWidth: 220
                SplitView.maximumWidth: 520
                color: Theme.surfaceColor

                ConnectionsPane {
                    id: connectionsPane
                    parent: root.narrow ? explorerDrawer.contentItem : explorerPane
                    anchors.fill: parent
                    onNewConnectionRequested: {
                        if (root.narrow) explorerDrawer.close()
                        connDialog.get().openNew()
                    }
                    onEditConnectionRequested: function(row) {
                        if (root.narrow) explorerDrawer.close()
                        connDialog.get().openEdit(row)
                    }
                    onPipelineBuilderRequested: function(uuid, database, collection) {
                        if (root.narrow) explorerDrawer.close()
                        root.openPipelineBuilder(uuid, database, collection)
                    }
                    onSchemaAnalysisRequested: function(uuid, database, collection) {
                        if (root.narrow) explorerDrawer.close()
                        root.openSchemaAnalysis(uuid, database, collection)
                    }
                    onAiAssistantRequested: function(uuid, database, collection) {
                        if (root.narrow) explorerDrawer.close()
                        root.openAiAssistant(uuid, database, collection)
                    }
                }
            }

            // Work area: tab strip + content
            MainWorkspaceArea {
                id: workArea
                SplitView.fillWidth: true
                app: root
                actions: appActions
            }
        }

        // Bottom log panel (toggleable via the Log toolbar button or Ctrl+L).
        LogPanel {
            id: logPanel
            SplitView.preferredHeight: 170
            SplitView.minimumHeight: 80
            visible: false
            onDetachToggled: root.setLogDetached(true)
            onCloseRequested: logPanel.visible = false
        }
    }

    // The log in its own window (LogPanel's ❐ button); same model, so
    // nothing is moved between windows.
    property bool logDetached: false
    readonly property bool logShown: logDetached ? logWindow.visible : logPanel.visible

    Window {
        id: logWindow
        title: qsTr("%1 — Diagnostic Log").arg(Brand.productName)
        width: 900
        height: 360
        minimumWidth: 420
        minimumHeight: 160
        color: Theme.baseColor
        visible: false

        LogPanel {
            anchors.fill: parent
            detached: true
            onDetachToggled: root.setLogDetached(false)
            onCloseRequested: logWindow.close()
        }
    }

    function setLogDetached(detached: bool) {
        logDetached = detached
        if (detached) {
            logPanel.visible = false
            logWindow.show()
            logWindow.raise()
            logWindow.requestActivate()
        } else {
            logWindow.hide()
            logPanel.visible = true
        }
    }

    function showLog() {
        if (logDetached) {
            logWindow.show()
            logWindow.raise()
        } else {
            logPanel.visible = true
        }
    }

    // SplitView keeps declaration order and does not mirror, so in right-to-left
    // languages the explorer is moved behind the work area (it ends up on the right).
    property bool _explorerOnRight: false
    function _placeExplorer() {
        if (Language.rtl === _explorerOnRight)
            return
        _explorerOnRight = Language.rtl
        const moving = _explorerOnRight ? explorerPane : workArea
        moving.parent = null
        moving.parent = mainSplit
    }
    Connections {
        target: Language
        function onLanguageChanged() { root._placeExplorer() }
    }

    function _restoreLayout() {
        _placeExplorer()
        const main = Backend.uiState("mainSplit")
        if (main)
            mainSplit.restoreState(main)
        const body = Backend.uiState("bodySplit")
        if (body)
            bodySplit.restoreState(body)
        const logWin = Backend.uiState("logWindow")
        if (logWin && logWin.length === 4) {
            logWindow.x = logWin[0]
            logWindow.y = logWin[1]
            logWindow.width = logWin[2]
            logWindow.height = logWin[3]
        }
        logDetached = Backend.uiState("logDetached") === true
        if (Backend.uiState("logVisible") === true)
            showLog()
        explorerPane.visible = Backend.uiState("explorerVisible") !== false
    }

    // The layout is saved shortly after it changes, not only when the window
    // closes, so a crash or a forced shutdown (which skips onClosing) keeps it.
    property bool _layoutReady: false
    function _scheduleLayoutSave() {
        if (_layoutReady)
            layoutSaveTimer.restart()
    }
    Timer {
        id: layoutSaveTimer
        interval: 1000
        onTriggered: root._saveLayout()
    }
    Connections {
        target: mainSplit
        function onResizingChanged() { root._scheduleLayoutSave() }
    }
    Connections {
        target: bodySplit
        function onResizingChanged() { root._scheduleLayoutSave() }
    }
    Connections {
        target: explorerPane
        function onVisibleChanged() { root._scheduleLayoutSave() }
    }
    Connections {
        target: logWindow
        function onXChanged() { root._scheduleLayoutSave() }
        function onYChanged() { root._scheduleLayoutSave() }
        function onWidthChanged() { root._scheduleLayoutSave() }
        function onHeightChanged() { root._scheduleLayoutSave() }
    }
    // Android back button / gesture: a dialog or menu closes first (Popup handles Key_Back by itself when
    // it has focus); on the main page it does not end the app while connections are open.
    Shortcut {
        enabled: Qt.platform.os === "android"
        sequences: [StandardKey.Back]
        onActivated: {
            if (Backend.connectedUuids.length === 0)
                root.close()
        }
    }

    // Leaving the foreground (phone, tablet, or just another window): save the layout now.
    Connections {
        target: Qt.application
        function onStateChanged() {
            if (root._layoutReady && Qt.application.state !== Qt.ApplicationActive)
                root._saveLayout()
        }
    }
    onLogShownChanged: _scheduleLayoutSave()
    onLogDetachedChanged: _scheduleLayoutSave()

    function _saveLayout() {
        Backend.setUiState("mainSplit", mainSplit.saveState())
        Backend.setUiState("bodySplit", bodySplit.saveState())
        Backend.setUiState("logVisible", root.logShown)
        Backend.setUiState("logDetached", root.logDetached)
        if (root.logDetached)
            Backend.setUiState("logWindow", [logWindow.x, logWindow.y, logWindow.width, logWindow.height])
        // Folded away only because the window is narrow: not "closed by the user".
        Backend.setUiState("explorerVisible", explorerPane.visible || root._explorerFoldedByWidth)
    }


    // ---- Dialogs (created on first use) -------------------------------------
    LazyDialog { id: connDialog; sourceComponent: Component { ConnectionDialog {} } }
    LazyDialog { id: prefsDialog; sourceComponent: Component { PreferencesDialog {} } }
    LazyDialog { id: cloudSyncDialog; sourceComponent: Component { CloudSyncDialog {} } }
    LazyDialog {
        id: aboutDialog
        sourceComponent: Component {
            AboutDialog {
                onOpenLicensesRequested: root.openLicenses()
            }
        }
    }
    LazyDialog { id: licensesDialog; sourceComponent: Component { LicensesDialog {} } }
    LazyDialog { id: shortcutsDialog; sourceComponent: Component { ShortcutsDialog {} } }
    LazyDialog { id: historyDialog; sourceComponent: Component { HistoryDialog { app: root } } }
    LazyDialog { id: recoverDraftsDialog; sourceComponent: Component { RecoverDraftsDialog {} } }
    LazyDialog { id: hostKeyDialog; sourceComponent: Component { HostKeyDialog {} } }
    Connections {
        target: Backend
        function onHostKeyRequested(id, host, port, keyType, fingerprint, changed) {
            hostKeyDialog.get().ask(id, host, port, keyType, fingerprint, changed)
        }
    }
    LazyDialog { id: pipelineDialog; sourceComponent: Component { PipelineBuilderDialog {} } }
    Connections {
        target: pipelineDialog.item
        function onExportRequested(script, database, serverUuid) {
            root.openCodeExport(script, database, serverUuid)
        }
    }
    LazyDialog { id: codeExportDialog; sourceComponent: Component { ExportCodeDialog {} } }
    LazyDialog { id: schemaDialog; sourceComponent: Component { SchemaDialog {} } }
    LazyDialog { id: aiDialog; sourceComponent: Component { AiAssistantDialog {} } }
    Connections {
        target: aiDialog.item
        function onPreferencesRequested() { root.openPreferences() }
        function onExportRequested(script, database, serverUuid) {
            root.openCodeExport(script, database, serverUuid)
        }
    }

    InputDialog {
        id: snippetNameDialog
        title: qsTr("Save Script as Snippet")
        label: qsTr("Snippet name (optional)")
        onSubmitted: function(text) {
            var p = root.currentShellPage()
            var s = root.currentShell()
            if (p && s)
                s.saveAsSnippet(p.scriptText(), text)
        }
    }

    QtDialogs.FileDialog {
        id: openScriptDialog
        fileMode: QtDialogs.FileDialog.OpenFile
        nameFilters: [qsTr("JavaScript files (*.js)"), qsTr("All files (*)")]
        onAccepted: {
            var localPath = Backend.urlToLocalFile(selectedFile)
            var p = root.currentShellPage()
            if (p) {
                p.setScript(Backend.readFile(selectedFile))
                p.markSaved(localPath)
            }
        }
    }
    QtDialogs.FileDialog {
        id: saveScriptDialog
        fileMode: QtDialogs.FileDialog.SaveFile
        defaultSuffix: "js"
        nameFilters: [qsTr("JavaScript files (*.js)"), qsTr("All files (*)")]
        onAccepted: {
            var localPath = Backend.urlToLocalFile(selectedFile)
            var p = root.currentShellPage()
            if (p && Backend.saveFile(selectedFile, p.scriptText()))
                p.markSaved(localPath)
        }
    }

    // ---- Actions shared by the command registry and the shortcuts below ----
    function openNewConnection() { connDialog.get().openNew() }
    function openCloudSync(startSetup) { cloudSyncDialog.get().openDialog(startSetup === true) }
    function openPreferences(section) {
        var dialog = prefsDialog.get()
        dialog.revealSection = section || ""
        dialog.open()
    }
    function openShortcuts() { shortcutsDialog.get().open() }
    function openRecoverDrafts() {
        if (Drafts.count > 0)
            recoverDraftsDialog.get().open()
    }
    function openAbout() { aboutDialog.get().open() }
    function openLicenses() { licensesDialog.get().open() }
    function openHistory(snippets) { historyDialog.get().openHistory(snippets) }
    function openPipelineBuilder(uuid: string, database: string, collection: string) {
        pipelineDialog.get().openFor(uuid, database, collection)
    }
    // Dev/CI helper invoked by --pipeline <db> <coll> [stagesJson].
    function openDebugPipeline(uuid, database, collection, stagesJson) {
        var d = pipelineDialog.get()
        if (d.openFor(uuid, database, collection) && stagesJson.length > 0)
            d.addDebugStages(JSON.parse(stagesJson))
    }
    // Builder for the current shell tab's collection (menu / Ctrl+Shift+A).
    function openPipelineBuilderForCurrent() {
        var s = currentShell()
        if (s && s.activeCollection.length > 0)
            openPipelineBuilder(String(s.serverUuid).split("@")[0], s.activeDatabase, s.activeCollection)
    }
    function openCodeExport(script: string, database: string, serverUuid: string) {
        codeExportDialog.get().openFor(script, database, serverUuid)
    }
    // Current tab's selection (or whole script) as driver code (menu / Ctrl+Shift+E).
    function exportCurrentQuery() {
        var p = currentShellPage()
        var s = currentShell()
        if (p && s)
            openCodeExport(p.selectedOrAllScript(), s.activeDatabase, String(s.serverUuid).split("@")[0])
    }
    function openSchemaAnalysis(uuid, database, collection) {
        schemaDialog.get().openFor(uuid, database, collection)
    }
    function openSchemaAnalysisForCurrent() {
        var s = currentShell()
        if (s && s.activeCollection.length > 0)
            openSchemaAnalysis(String(s.serverUuid).split("@")[0], s.activeDatabase, s.activeCollection)
    }
    function openAiAssistant(uuid: string, database: string, collection: string) {
        aiDialog.get().openFor(uuid, database, collection)
    }
    // Current shell tab's collection (menu / Ctrl+Shift+G; Ctrl+Shift+I is the
    // editor's Format).
    function openAiAssistantForCurrent() {
        var s = currentShell()
        if (s && s.activeCollection.length > 0)
            openAiAssistant(String(s.serverUuid).split("@")[0], s.activeDatabase, s.activeCollection)
    }
    // Dev/CI helper invoked by --ai <db> <coll> [question].
    function openDebugAiAssistant(uuid, database, collection, question) {
        var d = aiDialog.get()
        if (d.openFor(uuid, database, collection) && question.length > 0)
            d.askDebug(question)
    }
    function saveScriptAsSnippet() {
        if (currentShellPage())
            snippetNameDialog.open()
    }
    function closeCurrentTab() { root.closeTabAt(root.tabs.currentIndex) }
    function toggleExplorer() {
        if (root.narrow) {
            if (explorerDrawer.visible)
                explorerDrawer.close()
            else
                explorerDrawer.open()
        } else {
            explorerPane.visible = !explorerPane.visible
        }
    }
    function toggleLogPanel() {
        if (logDetached) {
            if (logWindow.visible)
                logWindow.close()
            else
                showLog()
        } else {
            logPanel.visible = !logPanel.visible
        }
    }
    function openScript() {
        if (currentShellPage())
            openScriptDialog.open()
    }
    function openNewShell() {
        var s = currentShell()
        if (s) {
            Backend.openShell(s.serverUuid, s.activeDatabase, "")
            return
        }
        var uuid = Backend.firstConnectedUuid()
        if (uuid.length > 0)
            Backend.openShell(uuid, "", "")
    }
    function duplicateCurrentShell() {
        var idx = currentShellIndex()
        var p = currentShellPage()
        if (idx >= 0)
            Backend.duplicateShell(idx, p ? p.scriptText() : "")
    }
    function openMonitorForActiveServer() {
        var uuid = root.activeServerUuid()
        if (uuid.length > 0)
            Backend.openMonitor(uuid)
    }
    function executeCurrent() {
        var p = currentShellPage()
        if (p) {
            p.executeCurrent()
            return
        }
        var mon = currentMonitor()
        if (mon)
            mon.refreshNow()
    }
    function executeAll() {
        var p = currentShellPage()
        if (p) p.executeAll()
    }
    function stopCurrent() {
        var s = currentShell()
        if (s && s.busy) {
            s.stop()
            return
        }
        var p = currentShellPage()
        if (p) p.stopCurrent()
    }
    function withShellPage(fn) {
        var p = currentShellPage()
        if (p) fn(p)
    }
    function setCurrentViewMode(mode: int) {
        var s = currentShell()
        if (s) s.viewMode = mode
    }
    function toggleFullScreen() {
        root.visibility = root.visibility === ApplicationWindow.FullScreen
                          ? ApplicationWindow.Windowed
                          : ApplicationWindow.FullScreen
    }

    // The command registry (menus, command palette); see docs/DesignSystem.md, section 6.
    AppActions { id: appActions; app: root }
    CommandPalette { id: commandPalette; app: root; actions: appActions }
    function openPalette() { commandPalette.openPalette() }

    MainShortcuts {
        app: root
        explorerPane: explorerPane
    }

    MainServiceDialogs {
        id: serviceDialogs
        app: root
        explorerDrawer: explorerDrawer
    }

    MainDebugHarness {
        id: debugHarness
        app: root
        appBar: appBar
        connectionsPane: connectionsPane
        commandPalette: commandPalette
        hostKeyDialog: hostKeyDialog
        codeExportDialog: codeExportDialog
    }
}
