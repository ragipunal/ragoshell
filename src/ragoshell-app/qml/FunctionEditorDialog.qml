import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Modal JavaScript stored-function editor (db.system.js) for Add / Edit Function,
// with MongoDB 6.0+ / 7.0+ / 8.0+ server-side JavaScript deprecation notice.
RagoDialog {
    id: dlg

    property string serverUuid: ""
    property string serverLabel: ""
    property string serverColorTag: ""
    property double serverVer: 0.0
    property string serverVerText: ""
    property string dbName: ""
    property string oldFunctionName: ""
    property string errorMessage: ""

    title: oldFunctionName.length > 0 ? qsTr("Edit Function") : qsTr("Add Function")
    acceptText: oldFunctionName.length > 0 ? qsTr("Save Function") : qsTr("Create Function")
    rejectText: qsTr("Cancel")
    showReject: true
    width: Math.min(visibleWidth - 24, 660)
    height: Math.min(visibleHeight - 24, 520)

    function _loadServerMeta(uuid) {
        serverUuid = uuid || ""
        serverLabel = Backend.serverAddress(serverUuid)
        serverVer = Backend.serverVersion(serverUuid)
        serverVerText = Backend.serverVersionString(serverUuid)
        var _rev = ConnectionModel.revision
        serverColorTag = ConnectionModel.colorTagForUuid(serverUuid)
    }

    function openForAdd(uuid, db) {
        _loadServerMeta(uuid)
        dbName = db
        oldFunctionName = ""
        errorMessage = ""
        nameField.text = ""
        codeEditor.setText("function(x, y) {\n    return x + y;\n}")
        open()
        nameField.forceActiveFocus()
    }

    function openForEdit(uuid, db, meta) {
        _loadServerMeta(uuid)
        dbName = db
        var m = meta || ({})
        oldFunctionName = m.name || ""
        errorMessage = ""
        nameField.text = m.name || ""
        codeEditor.setText(m.code && String(m.code).length > 0
                           ? String(m.code)
                           : "function() {\n    \n}")
        open()
        codeEditor.forceEditorFocus()
    }

    beforeAccept: function() {
        var fName = nameField.text.trim()
        if (fName.length === 0) {
            errorMessage = qsTr("Function name is required.")
            return false
        }
        var codeText = codeEditor.text.trim()
        if (codeText.length === 0) {
            errorMessage = qsTr("Function JavaScript body cannot be empty.")
            return false
        }
        errorMessage = ""
        return Backend.createOrUpdateFunction(
            serverUuid, dbName, oldFunctionName, fName, codeText)
    }

    contentItem: ColumnLayout {
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            RagoBadge {
                visible: dlg.serverLabel.length > 0
                text: "⛁ " + dlg.serverLabel
                badgeColor: dlg.serverColorTag.length > 0
                            ? dlg.serverColorTag
                            : Theme.disabledTextColor
            }
            RagoBadge {
                text: "DB: " + dlg.dbName + " (system.js)"
                badgeColor: "#3b9eff"
            }
            RagoBadge {
                visible: dlg.serverVerText.length > 0
                text: "MongoDB v" + dlg.serverVerText
                badgeColor: Theme.accentColor
            }
            Item { Layout.fillWidth: true }
        }

        // MongoDB 6.0+ / 7.0+ / 8.0+ server-side JS deprecation banner
        Rectangle {
            visible: dlg.serverVer >= 6.0
            Layout.fillWidth: true
            implicitHeight: jsWarnLabel.implicitHeight + 16
            radius: 8
            color: Theme.dark ? Qt.rgba(0.96, 0.62, 0.04, 0.14) : Qt.rgba(0.96, 0.62, 0.04, 0.12)
            border.width: 1
            border.color: Qt.rgba(0.96, 0.62, 0.04, 0.55)

            Label {
                id: jsWarnLabel
                anchors.fill: parent
                anchors.margins: 8
                text: dlg.serverVer >= 8.0
                      ? qsTr("⚠ MongoDB 8.0+ disables server-side JavaScript execution ($where, $function, system.js) by default unless mongod is started with --enableJavaScriptScripting. Prefer native Aggregation Pipeline expressions.")
                      : qsTr("⚠ Stored server-side JavaScript (db.system.js) is deprecated in MongoDB 6.0+. For modern deployments, prefer Aggregation Pipeline stages and expressions.")
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: Theme.dark ? "#fcd34d" : "#92400e"
            }
        }

        ScrollView {
            id: formScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 80
            contentWidth: availableWidth
            contentHeight: formContent.height
            rightPadding: formContent.implicitHeight > availableHeight ? 8 : 0
            clip: true

            ColumnLayout {
                id: formContent
                width: formScroll.availableWidth
                height: Math.max(implicitHeight, formScroll.availableHeight)
                spacing: 8

                ColumnLayout {
                    spacing: 4
                    Layout.fillWidth: true
                    Label {
                        text: qsTr("Function Name (_id)")
                        font.pixelSize: 12
                        color: Theme.disabledTextColor
                    }
                    RagoTextField {
                        id: nameField
                        placeholderText: qsTr("e.g. sumValues")
                        Layout.fillWidth: true
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: qsTr("JavaScript Function Code")
                        font.pixelSize: 12
                        color: Theme.disabledTextColor
                        Layout.fillWidth: true
                    }
                    RagoPillButton {
                        text: qsTr("Template")
                        implicitHeight: 26
                        onClicked: codeEditor.setText("function(arg1) {\n    // Stored server-side function\n    return arg1;\n}")
                    }
                }

                RagoCodeEditor {
                    id: codeEditor
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 120
                    folding: true
                    showLineNumbers: true
                }
            }
        }

        Label {
            visible: dlg.errorMessage.length > 0
            text: "✕ " + dlg.errorMessage
            font.pixelSize: 12
            color: "#ff6b6b"
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }
}
