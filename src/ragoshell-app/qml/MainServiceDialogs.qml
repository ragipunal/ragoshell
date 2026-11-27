pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs as QtDialogs
import RagoShell

Item {
    id: svcHost
    required property var app
    required property var explorerDrawer
    visible: false
    width: 0
    height: 0

    function openUnsavedConfirm() {
        unsavedTabsConfirm.open()
    }

    function openResetPlaygroundConfirm() {
        resetPlaygroundConfirm.open()
    }

    // Message dialog driven by QmlUiServices (rago-core prompts).
    RagoDialog {
        id: msgDialog
        property string msg: ""
        property int level: 0
        title: Brand.productName
        acceptText: qsTr("OK")
        showReject: false
        width: Math.min(visibleWidth - 24, 440)

        contentItem: Label {
            text: msgDialog.msg
            wrapMode: Text.WordWrap
            font.pixelSize: 13
            color: Theme.textColor
        }

        onAccepted: UiServices.dismissMessage()
        onRejected: UiServices.dismissMessage()
    }

    // Text input dialog driven by QmlUiServices.
    RagoDialog {
        id: textDialog
        property string label: ""
        property bool passwordMode: false
        title: qsTr("Input Required")
        acceptText: qsTr("OK")
        rejectText: qsTr("Cancel")
        showReject: true
        width: Math.min(visibleWidth - 24, 440)

        contentItem: ColumnLayout {
            spacing: 10
            Label {
                text: textDialog.label
                wrapMode: Text.WordWrap
                font.pixelSize: 13
                Layout.fillWidth: true
                color: Theme.textColor
            }
            RagoTextField {
                id: textInput
                Layout.fillWidth: true
                echoMode: textDialog.passwordMode ? TextInput.Password
                                                  : TextInput.Normal
                onAccepted: textDialog.accept()
            }
        }
        onOpened: textInput.forceActiveFocus()
        onAccepted: UiServices.resolveText(textInput.text)
        onRejected: UiServices.cancelPending()
    }

    // File dialogs driven by QmlUiServices.
    QtDialogs.FileDialog {
        id: openFileDialog
        onAccepted: UiServices.resolveFile(Backend.urlToLocalFile(selectedFile))
        onRejected: UiServices.cancelPending()
    }
    QtDialogs.FileDialog {
        id: saveFileDialog
        fileMode: QtDialogs.FileDialog.SaveFile
        onAccepted: UiServices.resolveFile(Backend.urlToLocalFile(selectedFile))
        onRejected: UiServices.cancelPending()
    }

    // Route QmlUiServices signals to the QML dialogs.
    Connections {
        target: UiServices
        function onMessageRequested(title, message, level) {
            msgDialog.title = title || Brand.productName
            msgDialog.msg = message
            msgDialog.level = level
            msgDialog.open()
        }
        function onTextRequested(title, label, passwordMode) {
            textDialog.title = title || qsTr("Input Required")
            textDialog.label = label
            textDialog.passwordMode = passwordMode
            textInput.text = ""
            textDialog.open()
        }
        function onOpenFileRequested(caption, dir, filter) {
            openFileDialog.title = caption
            openFileDialog.open()
        }
        function onSaveFileRequested(caption, dir, filter) {
            saveFileDialog.title = caption
            saveFileDialog.open()
        }
    }

    // A failed MCP allow-list write (an unticked or deleted connection): the
    // dialog that caused it has closed, and AI tools may keep their access.
    Connections {
        target: McpSettings
        function onWriteFailed(error) {
            msgDialog.title = Brand.productName
            msgDialog.msg = qsTr("The last change to the MCP settings could not be saved, so AI tools may keep their access: %1")
                                .arg(error)
            msgDialog.level = 2
            msgDialog.open()
        }
    }

    // Connection failure notifications + shell/monitor tab tracking.
    Connections {
        target: Backend
        function onConnectionError(uuid, message) {
            // The app checked this connection by itself after the network changed: no dialog.
            if (Backend.autoRefreshing(uuid))
                return
            errDialog.msg = message
            errDialog.open()
        }
        function onShellOpened() {
            if (svcHost.app.narrow && svcHost.explorerDrawer.opened)
                svcHost.explorerDrawer.close()
        }
        function onMonitorOpened() {
            if (svcHost.app.narrow && svcHost.explorerDrawer.opened)
                svcHost.explorerDrawer.close()
        }
        function onMonitorActivated() {
            if (svcHost.app.narrow && svcHost.explorerDrawer.opened)
                svcHost.explorerDrawer.close()
        }
    }

    RagoDialog {
        id: unsavedTabsConfirm
        title: svcHost.app._pendingCloseAction === "window"
               ? qsTr("Unsaved Scripts — Exit %1?").arg(Brand.productName)
               : qsTr("Close Unsaved Script Tab?")
        acceptText: svcHost.app._pendingCloseAction === "window"
                    ? qsTr("Discard & Exit")
                    : qsTr("Discard & Close")
        rejectText: qsTr("Cancel")
        showReject: true
        dangerAccept: true
        width: Math.min(visibleWidth - 24, 480)

        contentItem: ColumnLayout {
            spacing: 10

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: unsavedWarnCol.implicitHeight + 16
                radius: 8
                color: Qt.rgba(Theme.dangerColor.r, Theme.dangerColor.g, Theme.dangerColor.b, 0.12)
                border.width: 1
                border.color: Qt.rgba(Theme.dangerColor.r, Theme.dangerColor.g, Theme.dangerColor.b, 0.40)

                ColumnLayout {
                    id: unsavedWarnCol
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 4

                    Label {
                        text: svcHost.app._pendingDirtyTitles.length === 1
                              ? qsTr("⚠ '%1' has unsaved script modifications.").arg(Language.isolate(svcHost.app._pendingDirtyTitles[0]))
                              : qsTr("⚠ %n open Shell tab(s) have unsaved script modifications:", "", svcHost.app._pendingDirtyTitles.length)
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        color: Theme.dangerColor
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Label {
                        visible: svcHost.app._pendingDirtyTitles.length > 1
                        text: "• " + svcHost.app._pendingDirtyTitles.join("\n• ")
                        font.family: AppSettings.codeFontFamily
                        font.pixelSize: 11
                        color: Theme.textColor
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }

            Label {
                text: Platform.withShortcut(qsTr("Closing without saving (%1) will permanently discard your script edits."), "Ctrl+S")
                font.pixelSize: 12
                color: Theme.disabledTextColor
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        onAccepted: {
            var act = svcHost.app._pendingCloseAction
            var idx = svcHost.app._pendingCloseTabIndex
            svcHost.app._pendingCloseAction = ""
            svcHost.app._pendingCloseTabIndex = -1
            svcHost.app._pendingDirtyTitles = []
            svcHost.app._performCloseActionNow(act, idx)
        }
    }

    RagoDialog {
        id: errDialog
        property string msg: ""
        title: qsTr("Operation Failed")
        acceptText: qsTr("Close")
        showReject: false
        dangerAccept: true
        width: Math.min(visibleWidth - 24, 460)

        contentItem: Label {
            text: errDialog.msg
            wrapMode: Text.WordWrap
            font.pixelSize: 13
            color: Theme.textColor
        }
    }

    RagoDialog {
        id: resetPlaygroundConfirm
        title: qsTr("Reset Demo Data?")
        acceptText: qsTr("Reset Demo Data")
        rejectText: qsTr("Cancel")
        showReject: true
        dangerAccept: true
        width: Math.min(visibleWidth - 24, 440)

        contentItem: Label {
            text: qsTr("Restore the in-browser demo database to its initial sample collections and documents? Any changes made in this session will be discarded.")
            wrapMode: Text.WordWrap
            font.pixelSize: 13
            color: Theme.textColor
        }

        onAccepted: Backend.resetPlaygroundData()
    }

    MandatoryUpdateDialog {
        id: mandatoryUpdateDialog
    }

    Connections {
        target: UpdateService
        function onStateChanged() {
            if (UpdateService.mandatory && svcHost.explorerDrawer.opened)
                svcHost.explorerDrawer.close()
        }
    }
}
