pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

Item {
    id: confirmDialogs
    required property var page
    visible: false
    width: 0
    height: 0

    function openDeleteConfirm() {
        deleteDocsConfirm.open()
    }

    function openWriteConfirm() {
        destructiveScriptConfirm.open()
    }

    RagoDialog {
        id: deleteDocsConfirm
        title: confirmDialogs.page.selectedDocIndices.length > 1
               ? qsTr("Delete %n Documents", "", confirmDialogs.page.selectedDocIndices.length)
               : qsTr("Delete Document")
        acceptText: confirmDialogs.page.selectedDocIndices.length > 1
                    ? qsTr("Delete %n Documents", "", confirmDialogs.page.selectedDocIndices.length)
                    : qsTr("Delete Document")
        rejectText: qsTr("Cancel")
        showReject: true
        dangerAccept: true
        width: Math.min(visibleWidth - 24, 470)

        contentItem: ColumnLayout {
            spacing: 10

            Flow {
                Layout.fillWidth: true
                spacing: 6

                RagoBadge {
                    visible: confirmDialogs.page.shell && confirmDialogs.page.shell.activeServer.length > 0
                    text: "⛁ " + (confirmDialogs.page.shell ? confirmDialogs.page.shell.activeServer : "")
                    badgeColor: confirmDialogs.page.serverColorTag.length > 0
                                ? confirmDialogs.page.serverColorTag
                                : Theme.disabledTextColor
                }

                RagoBadge {
                    visible: confirmDialogs.page.shell && confirmDialogs.page.shell.activeDatabase.length > 0
                    text: (confirmDialogs.page.shell ? confirmDialogs.page.shell.activeDatabase : "") + "." +
                          (confirmDialogs.page.shell ? confirmDialogs.page.shell.activeCollection : "")
                    badgeColor: "#ef4444"
                }
            }

            Label {
                text: confirmDialogs.page.selectedDocIndices.length > 1
                      ? qsTr("Permanently delete %n selected documents from '%1.%2'?", "", confirmDialogs.page.selectedDocIndices.length)
                            .arg(Language.isolate(confirmDialogs.page.shell ? confirmDialogs.page.shell.activeDatabase : ""))
                            .arg(Language.isolate(confirmDialogs.page.shell ? confirmDialogs.page.shell.activeCollection : ""))
                      : qsTr("Permanently delete the selected document from '%1.%2'?")
                            .arg(Language.isolate(confirmDialogs.page.shell ? confirmDialogs.page.shell.activeDatabase : ""))
                            .arg(Language.isolate(confirmDialogs.page.shell ? confirmDialogs.page.shell.activeCollection : ""))
                wrapMode: Text.WordWrap
                font.pixelSize: 13
                font.bold: true
                color: Theme.textColor
                Layout.fillWidth: true
            }

            Rectangle {
                readonly property string idsText: deleteDocsConfirm.opened ? confirmDialogs.page._buildSelectedIdsPreview() : ""
                visible: idsText.length > 0
                Layout.fillWidth: true
                implicitHeight: idsPreviewLabel.implicitHeight + 16
                radius: 8
                color: Theme.surfaceColor
                border.width: 1
                border.color: Theme.glassBorder

                Label {
                    id: idsPreviewLabel
                    anchors.fill: parent
                    anchors.margins: 8
                    text: parent.idsText
                    font.family: AppSettings.codeFontFamily
                    font.pixelSize: Math.max(11, AppSettings.codeFontSize - 1)
                    color: Theme.textColor
                    wrapMode: Text.WrapAnywhere
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: delWarnLbl.implicitHeight + 16
                radius: 8
                color: Theme.dark ? Qt.rgba(0.94, 0.27, 0.27, 0.14) : Qt.rgba(0.94, 0.27, 0.27, 0.08)
                border.width: 1
                border.color: confirmDialogs.page.serverColorTag.length > 0
                              ? confirmDialogs.page.serverColorTag
                              : Qt.rgba(0.94, 0.27, 0.27, 0.50)

                Label {
                    id: delWarnLbl
                    anchors.fill: parent
                    anchors.margins: 8
                    text: qsTr("⚠ Deleted documents are removed immediately from MongoDB and cannot be recovered.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: Theme.textColor
                }
            }
        }

        onAccepted: {
            if (confirmDialogs.page.shell)
                confirmDialogs.page.shell.deleteDocumentsAt(confirmDialogs.page.selectedDocIndices)
        }
    }

    // ---- Write / Mutating / Destructive Shell Execution Confirmation --------
    function _classifyWriteOp(opLabel) {
        var s = String(opLabel || "")
        if (s.indexOf("drop") === 0 || s.indexOf("delete") === 0 || s.indexOf("remove") === 0 ||
            s.indexOf("findOneAndDelete") === 0 || s.indexOf("findAndModify(remove") === 0 ||
            s.indexOf("killOp") === 0 ||
            s === "runCommand(dropDatabase)" || s === "runCommand(drop)" ||
            s === "runCommand(delete)" || s === "runCommand(dropIndexes)" ||
            s === "runCommand(dropUser)" || s === "runCommand(dropAllUsersFromDatabase)" ||
            s === "runCommand(killOp)") {
            return "delete"
        }
        if (s.indexOf("insert") === 0 || s === "save()" || s.indexOf("create") === 0 ||
            s.indexOf("ensureIndex") === 0 ||
            s === "runCommand(insert)" || s === "runCommand(create)" ||
            s === "runCommand(createIndexes)" || s === "runCommand(createUser)") {
            return "insert"
        }
        return "update"
    }

    function _hasWriteOpKind(kind) {
        for (var i = 0; i < confirmDialogs.page._pendingDestructiveOps.length; ++i) {
            if (confirmDialogs._classifyWriteOp(confirmDialogs.page._pendingDestructiveOps[i]) === kind)
                return true
        }
        return false
    }

    RagoDialog {
        id: destructiveScriptConfirm
        readonly property bool hasDeleteOp: confirmDialogs._hasWriteOpKind("delete")
        readonly property bool hasUpdateOp: confirmDialogs._hasWriteOpKind("update")
        readonly property bool hasInsertOp: confirmDialogs._hasWriteOpKind("insert")

        title: {
            var cats = []
            if (hasDeleteOp) cats.push(qsTr("Delete"))
            if (hasUpdateOp) cats.push(qsTr("Update"))
            if (hasInsertOp) cats.push(qsTr("Insert"))
            return cats.length > 0
                   ? qsTr("Confirm Execution (%1)").arg(cats.join(" / "))
                   : qsTr("Confirm Write Execution")
        }
        acceptText: hasDeleteOp ? qsTr("Execute Delete / Write") : qsTr("Execute Command")
        rejectText: qsTr("Cancel")
        showReject: true
        dangerAccept: hasDeleteOp
        width: Math.min(visibleWidth - 24, 520)

        contentItem: ColumnLayout {
            spacing: 10

            Flow {
                Layout.fillWidth: true
                spacing: 6

                RagoBadge {
                    visible: confirmDialogs.page.shell && confirmDialogs.page.shell.activeServer.length > 0
                    text: "⛁ " + (confirmDialogs.page.shell ? confirmDialogs.page.shell.activeServer : "")
                    badgeColor: confirmDialogs.page.serverColorTag.length > 0
                                ? confirmDialogs.page.serverColorTag
                                : Theme.disabledTextColor
                }

                RagoBadge {
                    visible: confirmDialogs.page.shell && confirmDialogs.page.shell.activeDatabase.length > 0
                    text: "DB: " + (confirmDialogs.page.shell ? confirmDialogs.page.shell.activeDatabase : "")
                    badgeColor: "#3b9eff"
                }

                Repeater {
                    model: confirmDialogs.page._pendingDestructiveOps
                    delegate: RagoBadge {
                        required property string modelData
                        readonly property string opKind: confirmDialogs._classifyWriteOp(modelData)
                        text: (opKind === "delete" ? "✕ " : (opKind === "update" ? "✎ " : "+ ")) + modelData
                        badgeColor: opKind === "delete" ? "#ef4444"
                                  : opKind === "update" ? "#d97706"
                                                        : "#10b981"
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: writeSummaryLbl.implicitHeight + 16
                radius: 8
                color: destructiveScriptConfirm.hasDeleteOp
                       ? Qt.rgba(Theme.dangerColor.r, Theme.dangerColor.g, Theme.dangerColor.b, 0.12)
                       : Qt.rgba(0.96, 0.62, 0.04, 0.12)
                border.width: 1
                border.color: destructiveScriptConfirm.hasDeleteOp
                              ? Qt.rgba(Theme.dangerColor.r, Theme.dangerColor.g, Theme.dangerColor.b, 0.40)
                              : Qt.rgba(0.96, 0.62, 0.04, 0.48)

                Label {
                    id: writeSummaryLbl
                    anchors.fill: parent
                    anchors.margins: 8
                    text: destructiveScriptConfirm.hasDeleteOp
                          ? qsTr("⚠ This script contains delete/drop commands that permanently remove documents or database objects on MongoDB. Do you want to proceed?")
                          : qsTr("⚠ This script contains write commands (insert / update) that will add or modify documents/objects on MongoDB. Do you want to proceed?")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    color: destructiveScriptConfirm.hasDeleteOp
                           ? Theme.dangerColor
                           : (Theme.dark ? "#fcd34d" : "#92400e")
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: Math.min(130, scriptSnippetLbl.implicitHeight + 18)
                radius: 8
                color: Theme.surfaceColor
                border.width: 1
                border.color: confirmDialogs.page.serverColorTag.length > 0
                              ? confirmDialogs.page.serverColorTag
                              : Theme.borderColor
                clip: true

                Label {
                    id: scriptSnippetLbl
                    anchors.fill: parent
                    anchors.margins: 9
                    text: {
                        var s = String(confirmDialogs.page._pendingScriptToRun || "").trim()
                        return s.length > 360 ? (s.substring(0, 360) + " …") : s
                    }
                    font.family: AppSettings.codeFontFamily
                    font.pixelSize: Math.max(11, AppSettings.codeFontSize - 1)
                    color: Theme.textColor
                    wrapMode: Text.WrapAnywhere
                }
            }

            RagoCheckBox {
                id: keepWarnEnabledCheck
                text: qsTr("Always warn before executing insert, update, or delete commands")
                checked: AppSettings.confirmDestructiveQueries
                onToggled: {
                    AppSettings.confirmDestructiveQueries = checked
                    AppSettings.save()
                }
            }
        }

        onAccepted: {
            if (confirmDialogs.page.shell && confirmDialogs.page._pendingScriptToRun.length > 0) {
                var toRun = confirmDialogs.page._pendingScriptToRun
                confirmDialogs.page._pendingScriptToRun = ""
                confirmDialogs.page._pendingDestructiveOps = []
                confirmDialogs.page.shell.execute(toRun)
            }
        }
    }
}
