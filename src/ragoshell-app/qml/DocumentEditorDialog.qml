import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// In-app modal JSON/BSON Document Editor (Insert / Edit / Duplicate / View).
// Supports Mongo Shell literals (ObjectId, ISODate, NumberLong, NumberInt,
// NumberDecimal, UUID, Timestamp, BinData) and Extended JSON v2, multi-document
// inserts, live validation, pretty-formatting, and responsive sizing.
Dialog {
    id: dlg
    parent: Overlay.overlay
    // Sized and centered in what safe areas and the on-screen keyboard leave free (see VisibleArea.qml).
    readonly property real visibleWidth: visibleArea.width
    readonly property real visibleHeight: visibleArea.height
    readonly property bool cramped: visibleHeight < 380
    VisibleArea { id: visibleArea; reference: dlg.parent }
    x: Math.round(visibleArea.left + Math.max(0, (visibleWidth - width) / 2))
    y: Math.round(visibleArea.top + Math.max(0, (visibleHeight - height) / 2))
    modal: true
    focus: true
    width: Math.min(visibleWidth - 20, 780)
    height: Math.min(visibleHeight - 24, 620)
    padding: 0
    topPadding: 0
    bottomPadding: 0
    leftPadding: 0
    rightPadding: 0
    topInset: 0
    bottomInset: 0
    leftInset: 0
    rightInset: 0
    closePolicy: Popup.NoAutoClose

    // Modes: "insert", "edit", "view"
    property string mode: "insert"
    property string serverLabel: ""
    property string serverUuid: ""
    property string databaseName: ""
    property string collectionName: ""
    property int editDocIndex: -1
    property var targetShell: null

    property string _initialText: ""
    property string _statusText: ""
    property bool _statusOk: false
    property bool _saving: false
    property bool _savingViaBackend: false
    property bool _editWillInsert: false

    signal submittedToBackend(string uuid, string dbName, string collName, string jsonText)

    Timer {
        id: idCheckTimer
        interval: 120
        repeat: false
        onTriggered: dlg._refreshEditIdState()
    }

    Connections {
        target: (dlg.opened && dlg._saving && !dlg._savingViaBackend) ? dlg.targetShell : null
        function onCrudCompleted(ok, message) {
            dlg._saving = false
            if (ok) {
                dlg.accept()
            } else {
                dlg._statusOk = false
                dlg._statusText = message
            }
        }
    }

    Connections {
        target: (dlg.opened && dlg._saving && dlg._savingViaBackend) ? Backend : null
        function onDocumentInsertFinished(success, message) {
            dlg._saving = false
            dlg._savingViaBackend = false
            if (success) {
                dlg.submittedToBackend(dlg.serverUuid, dlg.databaseName, dlg.collectionName, codeEditor.text)
                dlg.accept()
            } else {
                dlg._statusOk = false
                dlg._statusText = message
            }
        }
    }

    function _refreshEditIdState() {
        if (mode !== "edit" || !targetShell || editDocIndex < 0 || codeEditor.textLength > 65536) {
            _editWillInsert = false
            return
        }
        _editWillInsert = targetShell.isEditIdChanged(editDocIndex, codeEditor.text)
    }

    function openForInsert(shellObj, srvLabel, uuid, dbName, collName, initialJson, initialCaretOffset) {
        mode = "insert"
        targetShell = shellObj
        serverLabel = srvLabel || ""
        serverUuid = uuid || ""
        databaseName = dbName || ""
        collectionName = collName || ""
        editDocIndex = -1
        _saving = false
        _savingViaBackend = false
        _editWillInsert = false
        var hasCustomJson = (initialJson !== undefined && initialJson !== null && initialJson.length > 0)
        var txt = hasCustomJson ? initialJson : "{\n    \n}"
        var caretPos = (initialCaretOffset !== undefined && initialCaretOffset !== null)
                       ? initialCaretOffset
                       : (hasCustomJson ? -1 : 6)
        _initialText = txt
        codeEditor.text = txt
        codeEditor.setSavePoint()
        if (caretPos >= 0)
            codeEditor.setCaretPosition(caretPos)
        _statusText = ""
        _statusOk = false
        open()
        Qt.callLater(function() {
            if (caretPos >= 0)
                codeEditor.setCaretPosition(caretPos)
            codeEditor.focusEditor()
        })
    }

    function openForEdit(shellObj, docIdx, srvLabel, dbName, collName, docJson) {
        mode = "edit"
        targetShell = shellObj
        editDocIndex = docIdx
        serverLabel = srvLabel || ""
        serverUuid = shellObj ? shellObj.serverUuid : ""
        databaseName = dbName || ""
        collectionName = collName || ""
        _saving = false
        _savingViaBackend = false
        _editWillInsert = false
        _initialText = docJson || "{\n}"
        codeEditor.text = _initialText
        codeEditor.setSavePoint()
        _statusText = ""
        _statusOk = false
        open()
        Qt.callLater(function() { codeEditor.focusEditor() })
    }

    function openForView(srvLabel, dbName, collName, docJson) {
        mode = "view"
        targetShell = null
        editDocIndex = -1
        serverLabel = srvLabel || ""
        serverUuid = ""
        databaseName = dbName || ""
        collectionName = collName || ""
        _saving = false
        _savingViaBackend = false
        _editWillInsert = false
        _initialText = docJson || "{}"
        codeEditor.text = _initialText
        codeEditor.setSavePoint()
        _statusText = ""
        _statusOk = false
        open()
        Qt.callLater(function() { codeEditor.focusEditor() })
    }

    function runValidate() {
        _refreshEditIdState()
        var single = (mode === "edit")
        var err = targetShell
                  ? targetShell.validateJson(codeEditor.text, single)
                  : Backend.validateJson(codeEditor.text, single)
        if (err.length === 0) {
            _statusOk = true
            _statusText = qsTr("Valid BSON / JSON.")
            return true
        }
        _statusOk = false
        _statusText = err
        return false
    }

    function runFormat() {
        if (!runValidate())
            return
        var formatted = targetShell
                        ? targetShell.formatJson(codeEditor.text)
                        : Backend.formatJson(codeEditor.text)
        if (formatted.length > 0) {
            codeEditor.replaceAllText(formatted)
            _refreshEditIdState()
            _statusOk = true
            _statusText = qsTr("Formatted JSON.")
        }
    }

    function tryCancel() {
        _saving = false
        _savingViaBackend = false
        if (mode !== "view" && codeEditor.text !== _initialText) {
            discardConfirm.open()
            return
        }
        dlg.reject()
    }

    function trySave() {
        if (_saving)
            return
        if (mode === "view") {
            dlg.accept()
            return
        }
        _refreshEditIdState()
        if (!runValidate())
            return

        if (targetShell) {
            _saving = true
            _savingViaBackend = false
            let err = (mode === "edit")
                      ? targetShell.submitEditJson(editDocIndex, codeEditor.text)
                      : targetShell.submitInsertJson(codeEditor.text)
            if (err.length > 0) {
                _saving = false
                _statusOk = false
                _statusText = err
            }
            return
        }

        // Invoked from Explorer pane (no open shell required)
        _saving = true
        _savingViaBackend = true
        var bErr = Backend.insertDocumentsJson(serverUuid, databaseName, collectionName, codeEditor.text)
        if (bErr.length > 0) {
            _saving = false
            _savingViaBackend = false
            _statusOk = false
            _statusText = bErr
        }
    }

    Overlay.modal: Rectangle {
        color: Theme.dark ? Qt.rgba(0, 0, 0, 0.62) : Qt.rgba(0.05, 0.1, 0.08, 0.35)
    }

    background: Rectangle {
        radius: 14
        color: Theme.surfaceColor
        border.width: 1
        border.color: Theme.glassBorder
    }

    Shortcut {
        sequence: "Escape"
        enabled: dlg.opened && !discardConfirm.opened
        onActivated: dlg.tryCancel()
    }

    contentItem: ColumnLayout {
        spacing: 0
        clip: true

        // Header with context badges (sized from its children so touch-sized buttons never cross the divider)
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: headerCol.implicitHeight + (dlg.cramped ? 12 : 20)
            Layout.preferredHeight: implicitHeight
            color: "transparent"

            ColumnLayout {
                id: headerCol
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                anchors.topMargin: dlg.cramped ? 6 : 10
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Label {
                        text: (dlg.mode === "insert" || (dlg.mode === "edit" && dlg._editWillInsert))
                              ? qsTr("Insert Document")
                              : (dlg.mode === "edit" ? qsTr("Edit Document") : qsTr("View Document"))
                        font.pixelSize: 15
                        font.bold: true
                        color: Theme.textColor
                    }

                    Item { Layout.fillWidth: true }

                    RagoIconButton {
                        glyph: "✕"
                        size: 26
                        toolTipText: qsTr("Close")
                        onClicked: dlg.tryCancel()
                    }
                }

                // Server / Database / Collection context badges
                HScrollRow {
                    visible: !dlg.cramped
                    Layout.fillWidth: true
                    margin: 0
                    spacing: 6

                    RagoBadge {
                        readonly property string srvTag: dlg.serverUuid.length > 0
                                                         ? ConnectionModel.colorTagForUuid(dlg.serverUuid)
                                                         : ""
                        visible: dlg.serverLabel.length > 0
                        text: "⛁ " + dlg.serverLabel
                        badgeColor: srvTag.length > 0 ? srvTag : Theme.disabledTextColor
                    }
                    RagoBadge {
                        visible: dlg.databaseName.length > 0
                        text: "DB: " + dlg.databaseName
                        badgeColor: "#3b9eff"
                    }
                    RagoBadge {
                        visible: dlg.collectionName.length > 0
                        text: "▤ " + dlg.collectionName
                        badgeColor: Theme.accentColor
                    }
                    Label {
                        visible: dlg.mode === "insert" && dlg.width >= 520
                        text: qsTr("Tip: Insert a single { … } object or a [ { … }, { … } ] array.")
                        font.pixelSize: 11
                        color: Theme.disabledTextColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Item {
                        visible: !(dlg.mode === "insert" && dlg.width >= 520)
                        Layout.fillWidth: true
                    }
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: Theme.glassBorder
            }
        }

        // Code editor body (inset cleanly inside dialog borders)
        RagoCodeEditor {
            id: codeEditor
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            Layout.topMargin: 12
            Layout.bottomMargin: 12
            framed: true
            readOnly: dlg.mode === "view" || dlg._saving
            lineNumbers: true
            folding: true
            showFormatAction: dlg.mode !== "view" && !dlg._saving
            onTextChanged: {
                if (dlg.opened && dlg.mode === "edit")
                    idCheckTimer.restart()
            }
            onFormatRequested: dlg.runFormat()
            onExecuteRequested: if (dlg.mode !== "view") dlg.trySave()
        }

        // Validation status banner
        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            Layout.bottomMargin: 10
            Layout.preferredHeight: statusLbl.implicitHeight + 14
            radius: 7
            visible: dlg._statusText.length > 0
            color: dlg._statusOk
                   ? (Theme.dark ? "#133323" : "#e3f8ec")
                   : (Theme.dark ? "#471818" : "#fde8e8")
            border.width: 1
            border.color: dlg._statusOk ? Theme.accentColor : "#e05252"

            Label {
                id: statusLbl
                anchors.fill: parent
                anchors.margins: 7
                wrapMode: Text.WordWrap
                font.family: AppSettings.codeFontFamily
                font.pixelSize: 12
                color: dlg._statusOk
                       ? (Theme.dark ? "#7cf2b0" : "#0b6e36")
                       : (Theme.dark ? "#ffb4b4" : "#9b1c1c")
                text: (dlg._statusOk ? "✓ " : "✕ ") + dlg._statusText
            }
        }

        // Footer action buttons
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Math.max(54, footerRow.implicitHeight + 16)
            Layout.preferredHeight: implicitHeight
            color: "transparent"

            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: Theme.glassBorder
            }

            HScrollRow {
                id: footerRow
                anchors.fill: parent
                margin: 14
                spacing: 8

                RagoPillButton {
                    visible: dlg.mode !== "view"
                    text: qsTr("Validate JSON")
                    iconText: "✓"
                    onClicked: dlg.runValidate()
                }

                RagoPillButton {
                    visible: dlg.mode !== "view"
                    text: qsTr("Format")
                    iconText: "{}"
                    onClicked: dlg.runFormat()
                }

                RagoPillButton {
                    visible: dlg.mode === "view"
                    text: qsTr("Copy JSON")
                    iconText: "❐"
                    onClicked: codeEditor.copyAllOrSelection()
                }

                Item { Layout.fillWidth: true }

                RagoPillButton {
                    text: dlg.mode === "view" ? qsTr("Close") : qsTr("Cancel")
                    onClicked: dlg.tryCancel()
                }

                RagoPillButton {
                    visible: dlg.mode !== "view"
                    enabled: !dlg._saving
                    text: (dlg.mode === "insert" || dlg._editWillInsert) ? qsTr("Insert") : qsTr("Save")
                    iconText: (dlg.mode === "insert" || dlg._editWillInsert) ? "+" : "✓"
                    primary: true
                    onClicked: dlg.trySave()
                }
            }
        }
    }

    // Unsaved changes guard
    RagoDialog {
        id: discardConfirm
        title: qsTr("Discard Changes?")
        acceptText: qsTr("Discard")
        rejectText: qsTr("Keep Editing")
        showReject: true
        dangerAccept: true
        width: Math.min(visibleWidth - 24, 380)

        contentItem: Label {
            text: qsTr("You have unsaved edits in this JSON document. Discard them?")
            wrapMode: Text.WordWrap
            font.pixelSize: 13
            color: Theme.textColor
        }

        onAccepted: dlg.reject()
    }
}
