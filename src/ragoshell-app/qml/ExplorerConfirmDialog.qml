pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

RagoDialog {
    id: confirmDialog
    required property var pane

    function openWithOptions(opts) {
        confirmDialog.pane._ctxAction = opts.action || ""
        confirmDialog.title = opts.title || qsTr("Confirm Action")
        confirmDialog.acceptText = opts.acceptText || qsTr("Confirm")
        confirmDialog.serverUuid = opts.uuid || ""
        confirmDialog.serverLabel = opts.serverLabel || (opts.uuid ? Backend.serverAddress(opts.uuid) : "")
        var _rev = ConnectionModel.revision
        confirmDialog.serverColorTag = opts.uuid ? ConnectionModel.colorTagForUuid(opts.uuid) : ""
        confirmDialog.targetNamespace = opts.ns || ""
        confirmDialog.msg = opts.msg || ""
        confirmDialog.consequenceText = opts.consequence || ""
        confirmDialog.expectedConfirmText = opts.typeToConfirm || ""
        confirmDialog.showCloseShellsOption = !!opts.showCloseShellsOption
        confirmDialog.closeShellsChecked = !!opts.defaultCloseShells
        confirmTypeInput.text = ""
        confirmDialog.open()
    }
    property string serverUuid: ""
    property string serverLabel: ""
    property string serverColorTag: ""
    property string targetNamespace: ""
    property string msg: ""
    property string consequenceText: ""
    property string expectedConfirmText: ""
    property bool showCloseShellsOption: false
    property bool closeShellsChecked: false

    acceptText: qsTr("Delete")
    rejectText: qsTr("Cancel")
    showReject: true
    dangerAccept: true
    acceptEnabled: expectedConfirmText.length === 0 ||
                   confirmTypeInput.text.trim() === expectedConfirmText
    width: Math.min(visibleWidth - 24, 480)

    onOpened: {
        if (expectedConfirmText.length > 0)
            Qt.callLater(function() { confirmTypeInput.forceActiveFocus() })
    }

    contentItem: ColumnLayout {
        spacing: 10

        // Target Server (Environment Color Tag) & Namespace Badges
        Flow {
            visible: confirmDialog.serverLabel.length > 0 || confirmDialog.targetNamespace.length > 0
            Layout.fillWidth: true
            spacing: 6

            RagoBadge {
                visible: confirmDialog.serverLabel.length > 0
                text: "⛁ " + confirmDialog.serverLabel
                badgeColor: confirmDialog.serverColorTag.length > 0
                            ? confirmDialog.serverColorTag
                            : Theme.disabledTextColor
            }

            RagoBadge {
                visible: confirmDialog.targetNamespace.length > 0
                text: confirmDialog.targetNamespace
                badgeColor: "#ef4444"
            }
        }

        Label {
            text: confirmDialog.msg
            wrapMode: Text.WordWrap
            font.pixelSize: 13
            font.bold: true
            color: Theme.textColor
            Layout.fillWidth: true
        }

        // Consequence / Warning Banner
        Rectangle {
            visible: confirmDialog.consequenceText.length > 0
            Layout.fillWidth: true
            implicitHeight: consequenceLabel.implicitHeight + 18
            radius: 8
            color: Theme.dark ? Qt.rgba(0.94, 0.27, 0.27, 0.14) : Qt.rgba(0.94, 0.27, 0.27, 0.08)
            border.width: 1
            border.color: confirmDialog.serverColorTag.length > 0
                          ? confirmDialog.serverColorTag
                          : Qt.rgba(0.94, 0.27, 0.27, 0.50)

            Label {
                id: consequenceLabel
                anchors.fill: parent
                anchors.margins: 9
                text: "⚠ " + confirmDialog.consequenceText
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.textColor
            }
        }

        // Optional checkbox when disconnecting a server with open Shell tabs
        RagoCheckBox {
            visible: confirmDialog.showCloseShellsOption
            text: qsTr("Also close open Shell tabs connected to this server")
            checked: confirmDialog.closeShellsChecked
            onToggled: confirmDialog.closeShellsChecked = checked
        }

        // Type-to-confirm safety guard for catastrophic drops (Database / Collection / All Documents)
        ColumnLayout {
            visible: confirmDialog.expectedConfirmText.length > 0
            Layout.fillWidth: true
            spacing: 4

            Label {
                text: qsTr("Type '%1' to confirm:").arg(confirmDialog.expectedConfirmText)
                font.pixelSize: 12
                color: Theme.disabledTextColor
                Layout.fillWidth: true
            }

            RagoTextField {
                id: confirmTypeInput
                Layout.fillWidth: true
                font.family: AppSettings.codeFontFamily
                font.pixelSize: AppSettings.codeFontSize
                placeholderText: confirmDialog.expectedConfirmText
                tintColor: confirmDialog.serverColorTag
                onAccepted: {
                    if (confirmDialog.acceptEnabled)
                        confirmDialog.accept()
                }
            }
        }
    }

    onAccepted: {
        if (confirmDialog.pane._ctxAction === "removeConn") {
            if (confirmDialog.pane._ctxConnOnline && confirmDialog.pane._ctxConnUuid.length > 0)
                Backend.disconnectServer(confirmDialog.pane._ctxConnUuid, true)
            ConnectionModel.remove(confirmDialog.pane._ctxConnRow)
        } else if (confirmDialog.pane._ctxAction === "disconnectConn") {
            Backend.disconnectServer(confirmDialog.pane._ctxUuid, confirmDialog.closeShellsChecked)
        } else if (confirmDialog.pane._ctxAction === "dropDb") {
            Backend.dropDatabase(confirmDialog.pane._ctxUuid, confirmDialog.pane._ctxDb)
        } else if (confirmDialog.pane._ctxAction === "dropColl") {
            Backend.dropCollection(confirmDialog.pane._ctxUuid, confirmDialog.pane._ctxDb, confirmDialog.pane._ctxColl)
        } else if (confirmDialog.pane._ctxAction === "removeAllDocs") {
            Backend.removeAllDocuments(confirmDialog.pane._ctxUuid, confirmDialog.pane._ctxDb, confirmDialog.pane._ctxColl)
        } else if (confirmDialog.pane._ctxAction === "dropIndex") {
            Backend.dropIndex(confirmDialog.pane._ctxUuid, confirmDialog.pane._ctxDb, confirmDialog.pane._ctxColl, confirmDialog.pane._ctxRawName)
        } else if (confirmDialog.pane._ctxAction === "hideIndex") {
            Backend.setIndexHidden(confirmDialog.pane._ctxUuid, confirmDialog.pane._ctxDb, confirmDialog.pane._ctxColl, confirmDialog.pane._ctxIndexMeta, true)
        } else if (confirmDialog.pane._ctxAction === "reIndex") {
            Backend.openShellWithScript(
                confirmDialog.pane._ctxUuid, confirmDialog.pane._ctxDb,
                "db.getCollection('" + confirmDialog.pane.escapeSingleQuotes(confirmDialog.pane._ctxColl) + "').reIndex()",
                true)
        } else if (confirmDialog.pane._ctxAction === "compactColl") {
            Backend.openShellWithScript(
                confirmDialog.pane._ctxUuid, confirmDialog.pane._ctxDb,
                "db.getCollection('" + confirmDialog.pane.escapeSingleQuotes(confirmDialog.pane._ctxColl) + "').compact()",
                true)
        } else if (confirmDialog.pane._ctxAction === "repairDb") {
            Backend.openShellWithScript(
                confirmDialog.pane._ctxUuid, confirmDialog.pane._ctxDb,
                "db.repairDatabase()",
                true)
        } else if (confirmDialog.pane._ctxAction === "dropFunc") {
            Backend.dropFunction(confirmDialog.pane._ctxUuid, confirmDialog.pane._ctxDb, confirmDialog.pane._ctxRawName)
        } else if (confirmDialog.pane._ctxAction === "dropUser") {
            Backend.dropUser(confirmDialog.pane._ctxUuid, confirmDialog.pane._ctxDb, confirmDialog.pane._ctxRawName)
        }
    }
}
