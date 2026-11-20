pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

// Shell tab: Context header + Visual QueryBar + IntelliShell (RagoCodeEditor) +
// Multi-statement Result Sub-Tabs + Tree/Table/Colored JSON/StatsSummary views +
// Right-Click / Long-Press / Toolbar CRUD + In-Place Cell Editing + Two-Way Pagination.
Item {
    id: page
    property ShellController shell
    // The command registry (AppActions.qml); the action strip runs its commands.
    property var actions: null
    // Action strip: 2 = buttons with labels, 1 = icons only, 0 = everything in the overflow menu.
    // The thresholds grow with the Touch density, whose buttons are larger.
    readonly property real stripScale: Theme.touch ? 1.2 : 1
    readonly property int stripMode: width >= 900 * stripScale ? 2 : (width >= 620 * stripScale ? 1 : 0)
    readonly property int stripButtonHeight: Theme.touch ? 30 : 24

    readonly property bool isMac: Qt.platform.os === "osx" || Qt.platform.os === "macos" || Qt.platform.os === "ios"

    property bool queryBarOpen: false
    property bool _suppressDirtyTrack: true
    property bool _syncingQueryBar: false
    property double _busyStartedAtMs: 0
    property int _liveBusyElapsedMs: 0
    readonly property bool isShellBusy: page.shell ? page.shell.busy : false

    onIsShellBusyChanged: {
        if (isShellBusy) {
            _busyStartedAtMs = Date.now()
            _liveBusyElapsedMs = 0
        } else {
            _liveBusyElapsedMs = 0
        }
    }

    Timer {
        id: busyElapsedTimer
        interval: 100
        repeat: true
        running: page.visible && page.isShellBusy
        onTriggered: {
            page._liveBusyElapsedMs = Math.max(0, Math.round(Date.now() - page._busyStartedAtMs))
        }
    }

    Timer {
        id: queryBarSyncTimer
        interval: 60
        repeat: false
        onTriggered: page.syncEditorToQueryBar()
    }

    // Unsaved text is kept on disk (Drafts) a moment after typing stops, so a
    // crash or a forced shutdown does not lose it.
    Timer {
        id: draftTimer
        interval: 1000
        repeat: false
        onTriggered: if (page.shell) page.shell.updateDraft(editor.text)
    }

    function syncEditorToQueryBar() {
        if (_syncingQueryBar || !page.shell || editor.textLength > 65536)
            return
        _syncingQueryBar = true
        var parsed = page.shell.parseFindQuery(editor.text)
        if (parsed && parsed.valid) {
            queryBar.syncFromParsed(parsed)
            if (parsed.collection && parsed.collection.length > 0)
                page.shell.previewActiveCollection(parsed.collection)
        }
        _syncingQueryBar = false
    }

    function syncQueryBarToEditor(builtScript, executeNow) {
        if (!builtScript || builtScript.length === 0)
            return
        _syncingQueryBar = true
        if (editor.text !== builtScript)
            editor.replaceAllText(builtScript)
        _syncingQueryBar = false
        if (executeNow && page.shell)
            page.shell.execute(builtScript)
    }

    // Multi-document selection state (sorted unique array of 0-based doc indices)
    property var selectedDocIndices: []
    property int _selectionAnchorDoc: -1
    property int _activeTreeRow: -1

    Connections {
        target: AppSettings
        function onCodeFontChanged() {
            Qt.callLater(function() {
                resultsViewport.forceLayouts()
            })
        }
    }

    // Context menu target metadata
    property int _ctxDocIndex: -1
    property int _ctxTreeRow: -1
    property string _ctxKey: ""
    property string _ctxRawValue: ""
    property string _ctxFieldPath: ""
    property string _ctxTypeName: ""
    property string _ctxOidIso: ""
    property bool _ctxExpandable: false

    // In-place cell editing state
    property int _editingDocIndex: -1
    property string _editingFieldPath: ""
    property string _editingTypeName: ""
    property string _editingInitialValue: ""

    // Pending destructive script confirmation state
    property string _pendingScriptToRun: ""
    property var _pendingDestructiveOps: []

    readonly property string serverColorTag: {
        var _rev = ConnectionModel.revision
        return (page.shell && page.shell.serverUuid)
               ? ConnectionModel.colorTagForUuid(page.shell.serverUuid)
               : ""
    }
    readonly property string serverVerText: {
        var _conns = Backend.connectedUuids
        return (page.shell && page.shell.serverUuid)
               ? Backend.serverVersionString(page.shell.serverUuid)
               : ""
    }

    function _runScriptWithSafetyGuard(scriptToRun) {
        if (!page.shell)
            return
        var trimmed = String(scriptToRun || "").trim()
        if (trimmed.length === 0)
            return
        if (AppSettings.confirmDestructiveQueries) {
            var ops = page.shell.detectDestructiveOperations(trimmed)
            if (ops && ops.length > 0) {
                page._pendingScriptToRun = scriptToRun
                page._pendingDestructiveOps = ops
                confirmDialogs.openWriteConfirm()
                return
            }
        }
        page.shell.execute(scriptToRun)
    }

    function _buildSelectedIdsPreview() {
        if (!page.shell || page.selectedDocIndices.length === 0)
            return ""
        var lines = []
        var maxShow = Math.min(page.selectedDocIndices.length, 5)
        for (var i = 0; i < maxShow; ++i) {
            var idVal = page.shell.documentIdPreviewAt(page.selectedDocIndices[i])
            if (idVal.length > 0)
                lines.push("• _id: " + idVal)
        }
        if (page.selectedDocIndices.length > maxShow)
            lines.push(qsTr("… and %n more document(s)", "", page.selectedDocIndices.length - maxShow))
        return lines.join("\n")
    }

    // ---- Public API called by Main.qml shortcuts ---------------------------
    function executeCurrent() {
        if (!page.shell)
            return
        var sel = editor.hasSelection ? editor.selectedText.trim() : ""
        page._runScriptWithSafetyGuard(sel.length > 0 ? sel : editor.text)
    }
    function executeAll() {
        if (!page.shell)
            return
        page._runScriptWithSafetyGuard(editor.text)
    }
    function formatScript() {
        if (!page.shell)
            return
        var formatted = page.shell.formatJson(editor.text)
        if (formatted.length > 0)
            editor.replaceAllText(formatted)
    }
    function stopCurrent() { if (page.shell) page.shell.stop() }
    function scriptText() { return editor.text }
    function selectedOrAllScript() {
        var sel = editor.hasSelection ? editor.selectedText.trim() : ""
        return sel.length > 0 ? sel : editor.text
    }
    // History "Load": replaces the editor text as an undoable edit (tab becomes modified).
    function loadScript(t) {
        editor.replaceAllText(t)
        editor.focusEditor()
    }
    function setScript(t) {
        _suppressDirtyTrack = true
        editor.text = t
        editor.setSavePoint()
        if (page.shell)
            page.shell.dirty = false
        _suppressDirtyTrack = false
    }
    function markSaved(path) {
        editor.setSavePoint()
        if (page.shell) {
            page.shell.filePath = path || page.shell.filePath
            page.shell.dirty = false
        }
    }
    function toggleOrientation() {
        split.orientation = split.orientation === Qt.Vertical
                            ? Qt.Horizontal : Qt.Vertical
    }
    function toggleQueryBar() {
        queryBarOpen = !queryBarOpen
        if (queryBarOpen) {
            page.syncEditorToQueryBar()
            Qt.callLater(function() { queryBar.focusFilter() })
        }
    }
    function _ensureDocEditor() {
        if (!docEditorLoader.active)
            docEditorLoader.active = true
        return docEditorLoader.item
    }
    function _ensureImportExportDialog() {
        if (!importExportLoader.active)
            importExportLoader.active = true
        return importExportLoader.item
    }
    function openFindInActiveEditor() {
        var je = resultsViewport.activeJsonEditor()
        if (je && (je.activeFocus || (page.shell && page.shell.viewMode === 2 && !editor.hasSelection)))
            je.openFindBar()
        else
            editor.openFindBar()
    }
    function triggerInsertDocument() {
        if (!page.shell || !page.shell.canInsertDocuments)
            return
        var dlg = page._ensureDocEditor()
        if (!dlg)
            return
        dlg.openForInsert(
            page.shell,
            page.shell.activeServer,
            page.shell.serverUuid,
            page.shell.activeDatabase,
            page.shell.activeCollection,
            "{\n    \n}",
            6)
    }
    function triggerEditSelectedDocument() {
        if (!page.shell || !page.shell.canModifyDocuments || selectedDocIndices.length === 0)
            return
        var dlg = page._ensureDocEditor()
        if (!dlg)
            return
        var idx = selectedDocIndices[0]
        // Guard against multi-MB binary documents (e.g. embedded MP4/JPEG files)
        // freezing text-based JSON editing: open safely in truncated View mode with notice.
        var docBytes = page.shell.documentByteSizeAt(idx)
        if (docBytes > 4 * 1024 * 1024) {
            dlg.openForView(
                page.shell.activeServer,
                page.shell.activeDatabase,
                page.shell.activeCollection,
                page.shell.documentJsonForView(idx))
            dlg._statusOk = false
            dlg._statusText = qsTr("Document size (%1 MB) exceeds the 4 MB text editor limit; opened in safe read-only preview.").arg((docBytes / (1024 * 1024)).toFixed(1))
            return
        }
        dlg.openForEdit(
            page.shell,
            idx,
            page.shell.activeServer,
            page.shell.activeDatabase,
            page.shell.activeCollection,
            page.shell.documentJsonAt(idx))
    }
    function triggerViewSelectedDocument() {
        if (!page.shell || selectedDocIndices.length === 0)
            return
        var dlg = page._ensureDocEditor()
        if (!dlg)
            return
        var idx = selectedDocIndices[0]
        dlg.openForView(
            page.shell.activeServer,
            page.shell.activeDatabase,
            page.shell.activeCollection,
            page.shell.documentJsonForView(idx))
    }
    function triggerDuplicateDocument(docIdx) {
        if (!page.shell || !page.shell.canModifyDocuments || docIdx < 0)
            return
        var dlg = page._ensureDocEditor()
        if (!dlg)
            return
        var tpl = page.shell.duplicateTemplateAt(docIdx)
        dlg.openForInsert(
            page.shell,
            page.shell.activeServer,
            page.shell.serverUuid,
            page.shell.activeDatabase,
            page.shell.activeCollection,
            tpl.json,
            tpl.caretOffset)
    }
    function triggerDeleteSelectedDocuments() {
        if (!page.shell || !page.shell.canDeleteDocuments || selectedDocIndices.length === 0)
            return
        confirmDialogs.openDeleteConfirm()
    }
    function copySelectedValueOrDoc(fullDoc) {
        if (!page.shell)
            return
        if (fullDoc && selectedDocIndices.length > 0) {
            page.shell.copyToClipboard(page.shell.documentJsonAt(selectedDocIndices[0]))
        } else if (_ctxRawValue.length > 0) {
            page.shell.copyToClipboard(_ctxRawValue)
        } else if (selectedDocIndices.length > 0) {
            page.shell.copyToClipboard(page.shell.documentJsonAt(selectedDocIndices[0]))
        }
    }

    // ---- Selection helpers -------------------------------------------------
    function isDocSelected(docIdx) {
        return docIdx >= 0 && selectedDocIndices.indexOf(docIdx) !== -1
    }

    function selectDocumentWithModifiers(docIdx, modifiers) {
        if (docIdx < 0)
            return
        var ctrl = (modifiers & Qt.ControlModifier) !== 0
        var shift = (modifiers & Qt.ShiftModifier) !== 0

        if (shift && _selectionAnchorDoc >= 0) {
            var lo = Math.min(_selectionAnchorDoc, docIdx)
            var hi = Math.max(_selectionAnchorDoc, docIdx)
            var range = []
            for (var d = lo; d <= hi; ++d)
                range.push(d)
            selectedDocIndices = range
        } else if (ctrl) {
            var copy = selectedDocIndices.slice()
            var existing = copy.indexOf(docIdx)
            if (existing >= 0)
                copy.splice(existing, 1)
            else
                copy.push(docIdx)
            copy.sort(function(a, b) { return a - b })
            selectedDocIndices = copy
            _selectionAnchorDoc = docIdx
        } else {
            selectedDocIndices = [docIdx]
            _selectionAnchorDoc = docIdx
        }
        if (_ctxDocIndex !== docIdx) {
            _ctxDocIndex = docIdx
            _ctxRawValue = ""
            _ctxKey = ""
            _ctxFieldPath = ""
            _ctxOidIso = ""
        }
    }

    function populateContextFromCell(docIdx, treeRow, keyStr, rawVal, fieldPath, typeName, expandable) {
        _ctxDocIndex = docIdx
        _ctxTreeRow = treeRow
        _ctxKey = keyStr || ""
        _ctxRawValue = rawVal !== undefined && rawVal !== null ? String(rawVal) : ""
        _ctxFieldPath = fieldPath || ""
        _ctxTypeName = typeName || ""
        _ctxExpandable = !!expandable
        _ctxOidIso = (page.shell && (_ctxTypeName === "ObjectId" || _ctxRawValue.indexOf("ObjectId") !== -1))
                     ? page.shell.objectIdTimestamp(_ctxRawValue)
                     : ""
        if (docIdx >= 0 && !isDocSelected(docIdx)) {
            selectedDocIndices = [docIdx]
            _selectionAnchorDoc = docIdx
        }
    }

    function openResultMenuAt(globalParentItem, localX, localY) {
        var pt = globalParentItem.mapToItem(resultsBox, localX, localY)
        resultContextMenu.x = Math.max(4, Math.min(pt.x, resultsBox.width - resultContextMenu.width - 8))
        resultContextMenu.y = Math.max(4, Math.min(pt.y, resultsBox.height - 280))
        resultContextMenu.open()
    }

    function startInPlaceEdit(docIdx, fieldPath, typeName, rawVal) {
        if (!page.shell || !page.shell.canModifyDocuments)
            return
        if (docIdx < 0 || !fieldPath || fieldPath.length === 0)
            return
        var cleanVal = String(rawVal !== undefined && rawVal !== null ? rawVal : "")
        // Replace non-whitespace C0/C1 control codes (e.g. U+009C ST) so the inline
        // TextField never truncates strings with broken Latin-1/UTF-8 bytes.
        cleanVal = cleanVal.replace(/[\x00-\x08\x0B\x0C\x0E-\x1F\x7F-\x9F]/g, "\uFFFD")
        _editingDocIndex = docIdx
        _editingFieldPath = fieldPath
        _editingTypeName = typeName
        _editingInitialValue = cleanVal
    }

    function cancelInPlaceEdit() {
        _editingDocIndex = -1
        _editingFieldPath = ""
        _editingTypeName = ""
        _editingInitialValue = ""
    }

    function commitInPlaceEdit(newVal) {
        if (!page.shell || _editingDocIndex < 0 || _editingFieldPath.length === 0) {
            cancelInPlaceEdit()
            return
        }
        var dIdx = _editingDocIndex
        var fPath = _editingFieldPath
        var tName = _editingTypeName
        cancelInPlaceEdit()
        page.shell.updateScalarCell(dIdx, fPath, newVal, tName)
    }

    function expandTreeSubtree(row, expand) {
        resultsViewport.expandSubtree(row, expand)
    }

    // Dev/CI helper (--calltip): caret to the end of the script, then the tip.
    function debugShowCallTip() {
        editorOverlays.debugShowCallTip()
    }

    function formatShellEditor() {
        if (!page.shell)
            return
        var target = editor.hasSelection ? editor.selectedText : editor.text
        var formatted = page.shell.formatJson(target)
        if (formatted.length > 0 && !editor.hasSelection)
            editor.replaceAllText(formatted)
    }

    Component.onCompleted: {
        if (page.isShellBusy && page._busyStartedAtMs <= 0)
            page._busyStartedAtMs = Date.now()
        var initial = (page.shell && page.shell.script.length > 0)
                      ? page.shell.script
                      : (Platform.shortcutsVisible ? qsTr("// %1 — write a query and press F5") : "// %1").arg(page.shell ? page.shell.title : "") + "\n"
        editor.text = initial
        editor.setSavePoint()
        _suppressDirtyTrack = false
        page.syncEditorToQueryBar()
    }

    Connections {
        target: page.shell
        function onAutocompleteReady() { editorOverlays.showAutocomplete() }
        function onLiveCollectionsUpdated() {
            if (editorOverlays.awaitingLive || editorOverlays.popupVisible)
                editorOverlays.triggerAutocomplete(editorOverlays.lastExplicit, false)
        }
        function onDirtyChanged() {
            // Saved (or undone to the saved text): the draft goes at once.
            if (page.shell.dirty)
                draftTimer.restart()
            else
                page.shell.updateDraft("")
        }
        function onResultChanged() {
            page.cancelInPlaceEdit()
            page.selectedDocIndices = page.shell && page.shell.resultCount > 0 ? [0] : []
            page._selectionAnchorDoc = page.selectedDocIndices.length > 0 ? 0 : -1
            page._ctxDocIndex = page.selectedDocIndices.length > 0 ? 0 : -1
            page._ctxTreeRow = -1
            page._ctxKey = ""
            page._ctxRawValue = ""
            page._ctxFieldPath = ""
            page._ctxTypeName = ""
            page._ctxOidIso = ""
            page._ctxExpandable = false
            resultsViewport.syncPagingInputs()
            if (page.shell && !page.shell.busy && AppSettings.autoExpand && page.shell.resultCount > 0) {
                Qt.callLater(function() {
                    resultsViewport.expandFirstRow()
                })
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Active Server / DB / Collection Context Strip -----------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.touch ? 34 : 32
            color: Theme.surfaceColor
            border.width: 1
            border.color: Theme.glassBorder

            HScrollRow {
                anchors.fill: parent
                margin: 8
                spacing: 5

                RagoBadge {
                    visible: page.shell && page.shell.activeServer.length > 0
                    text: "⛁ " + (page.shell ? page.shell.activeServer : "")
                    badgeColor: page.serverColorTag.length > 0
                                ? page.serverColorTag
                                : Theme.disabledTextColor
                }

                RagoBadge {
                    visible: page.serverVerText.length > 0 && page.width >= 520 * page.stripScale
                    text: "v" + page.serverVerText
                    badgeColor: "#60a5fa"
                }

                RagoBadge {
                    visible: page.shell && page.shell.activeDatabase.length > 0
                    text: "DB: " + (page.shell ? page.shell.activeDatabase : "")
                    badgeColor: "#3b9eff"
                }

                RagoBadge {
                    visible: page.shell && page.shell.activeCollection.length > 0
                    text: "▤ " + (page.shell ? page.shell.activeCollection : "")
                    badgeColor: Theme.accentColor
                }

                Item { Layout.fillWidth: true }

                // ---- Action strip: the commands of the former Shell menu, next to the work ----
                StripButton {
                    visible: page.actions !== null && page.stripMode > 0
                    actions: page.actions
                    commandId: "shell.format"
                    iconText: "❖"
                    //: Button: format the script in the editor.
                    label: qsTr("Format")
                    showLabel: page.stripMode === 2
                }
                StripButton {
                    visible: page.actions !== null && page.stripMode > 0
                    actions: page.actions
                    commandId: "shell.history"
                    iconText: "◷"
                    //: Button: open the query history.
                    label: qsTr("History")
                    showLabel: page.stripMode === 2
                }
                StripButton {
                    visible: page.actions !== null && page.stripMode > 0
                    actions: page.actions
                    commandId: "shell.snippets"
                    iconText: "☆"
                    label: qsTr("Snippets")
                    showLabel: page.stripMode === 2
                }
                StripButton {
                    visible: page.actions !== null && page.stripMode > 0
                    actions: page.actions
                    commandId: "shell.exportQuery"
                    iconText: "⇧"
                    //: Button: export the query as code in another language.
                    label: qsTr("Export")
                    showLabel: page.stripMode === 2
                }

                RagoPillButton {
                    visible: page.stripMode > 0
                    compact: true
                    text: qsTr("Query Bar")
                    iconText: "▤"
                    implicitHeight: page.stripButtonHeight
                    checked: page.queryBarOpen
                    onClicked: page.toggleQueryBar()
                }
                RagoIconButton {
                    visible: page.stripMode === 0
                    glyph: "▤"
                    size: 24
                    implicitWidth: Theme.touch ? 30 : 24
                    implicitHeight: implicitWidth
                    accent: page.queryBarOpen
                    toolTipText: qsTr("Query Bar")
                    onClicked: page.toggleQueryBar()
                }

                RagoPillButton {
                    visible: page.stripMode > 0
                    compact: true
                    //: Button that runs a MongoDB find() query. Keep "find" recognisable; it is not a text search.
                    text: qsTr("Find")
                    iconText: "⚲"
                    implicitHeight: page.stripButtonHeight
                    onClicked: editor.openFindBar()
                }
                RagoIconButton {
                    visible: page.stripMode === 0
                    glyph: "⚲"
                    size: 24
                    implicitWidth: Theme.touch ? 30 : 24
                    implicitHeight: implicitWidth
                    toolTipText: qsTr("Find")
                    onClicked: editor.openFindBar()
                }

                RagoIconButton {
                    id: stripMoreButton
                    visible: page.actions !== null
                    glyph: "⋮"
                    size: 24
                    implicitWidth: Theme.touch ? 30 : 24
                    implicitHeight: implicitWidth
                    toolTipText: qsTr("More")
                    onClicked: {
                        page.actions.refresh()
                        stripMenu.popup(stripMoreButton, stripMoreButton.width - stripMenu.width, stripMoreButton.height + 4)
                    }

                    RagoMenu {
                        id: stripMenu
                        CommandMenuItem { actions: page.actions; commandId: "shell.executeAll" }
                        CommandMenuItem { actions: page.actions; commandId: "shell.format"; visible: page.stripMode === 0 }
                        CommandMenuItem { actions: page.actions; commandId: "shell.history"; visible: page.stripMode === 0 }
                        CommandMenuItem { actions: page.actions; commandId: "shell.snippets"; visible: page.stripMode === 0 }
                        CommandMenuItem { actions: page.actions; commandId: "shell.exportQuery"; visible: page.stripMode === 0 }
                        RagoMenuSeparator {}
                        CommandMenuItem { actions: page.actions; commandId: "shell.saveSnippet" }
                        CommandMenuItem { actions: page.actions; commandId: "shell.duplicate" }
                    }
                }
            }
        }

        // ---- Collapsible Visual Query Bar ----------------------------------
        QueryBar {
            id: queryBar
            Layout.fillWidth: true
            visible: page.queryBarOpen
            shell: page.shell
            onQueryEdited: function(builtScript) {
                page.syncQueryBarToEditor(builtScript, false)
            }
            onExecuteScriptRequested: function(builtScript) {
                page.syncQueryBarToEditor(builtScript, true)
            }
        }

        // ---- Main Editor / Results SplitView -------------------------------
        SplitView {
            id: split
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Vertical

            handle: Rectangle {
                implicitWidth: Theme.splitHandle
                implicitHeight: Theme.splitHandle
                color: SplitHandle.pressed || SplitHandle.hovered
                       ? Theme.accentColor : Theme.surfaceColor
                border.width: 1
                border.color: Theme.glassBorder

                Rectangle {
                    visible: Theme.touch
                    anchors.centerIn: parent
                    width: split.orientation === Qt.Vertical ? 28 : 2
                    height: split.orientation === Qt.Vertical ? 2 : 28
                    radius: 1
                    color: Theme.disabledTextColor
                    opacity: 0.45
                }
            }

            // ---- IntelliShell Editor Pane ----------------------------------
            Item {
                id: editorBox
                implicitHeight: split.height > 0 && split.height < 360
                                ? Math.max(68, Math.round(split.height * 0.36))
                                : (Platform.formFactor === "phone" ? 132 : 210)
                implicitWidth: split.width > 0 && split.width < 680
                               ? Math.max(160, Math.round(split.width * 0.42))
                               : 420
                SplitView.minimumHeight: 68
                SplitView.minimumWidth: 160

                RagoCodeEditor {
                    id: editor
                    anchors.fill: parent
                    framed: false
                    lineNumbers: AppSettings.lineNumbers
                    folding: true
                    showFormatAction: true
                    overlayActive: editorOverlays.popupVisible
                    onAutocompleteRequested: editorOverlays.triggerAutocomplete(true, false)
                    onExecuteRequested: page.executeCurrent()
                    onFormatRequested: page.formatShellEditor()
                    // Save-point based: no full-document read per keystroke.
                    onModifiedChanged: {
                        if (!page._suppressDirtyTrack && page.shell)
                            page.shell.dirty = editor.modified
                    }
                    onTextChanged: {
                        queryBarSyncTimer.restart()
                        draftTimer.restart()
                        if ((editorOverlays.popupVisible || editorOverlays.awaitingLive) && !editor.composing)
                            editorOverlays.triggerAutocomplete(false, false)
                    }
                    // Uncommitted IME text is not a completion prefix.
                    onComposingChanged: {
                        if (editor.composing)
                            editorOverlays.hidePopup()
                    }
                    onCharAdded: function(ch) { editorOverlays.handleCharAdded(ch) }
                    onCaretPositionChanged: editorOverlays.handleCaretPositionChanged()
                    onEditorHasFocusChanged: {
                        if (!editor.editorHasFocus)
                            editorOverlays.hideAll()
                    }
                    onOverlayKeyPressed: function(key) { editorOverlays.handleOverlayKey(key) }
                    onEditorKeyPressed: function(event) {
                        if (event.key === Qt.Key_Escape && editorOverlays.callTipVisible) {
                            editorOverlays.hideCallTip()
                            event.accepted = true
                            return
                        }
                        if (event.key === Qt.Key_F5) {
                            page.executeCurrent()
                            event.accepted = true
                        } else if (event.key === Qt.Key_F6) {
                            page.stopCurrent()
                            event.accepted = true
                        }
                    }
                }

                ShellEditorOverlays {
                    id: editorOverlays
                    page: page
                    editor: editor
                }
            }

            // ---- Results Pane ----------------------------------------------
            ColumnLayout {
                id: resultsBox
                SplitView.fillHeight: true
                SplitView.fillWidth: true
                spacing: 0

                ShellResultsHeader {
                    Layout.fillWidth: true
                    page: page
                    editorHasSelection: editor.hasSelection
                    splitOrientation: split.orientation
                }

                ShellResultsViewport {
                    id: resultsViewport
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    page: page
                }

                ShellResultContextMenu {
                    id: resultContextMenu
                    page: page
                    filterText: queryBar.filterText
                }
            }
        }
    }

    // ---- Lazy Modal In-App Document Editor & Import/Export -----------------
    Loader {
        id: docEditorLoader
        active: false
        sourceComponent: Component {
            DocumentEditorDialog {}
        }
    }

    Loader {
        id: importExportLoader
        active: false
        sourceComponent: Component {
            ImportExportDialog {}
        }
    }


    ShellConfirmDialogs {
        id: confirmDialogs
        page: page
    }
}
