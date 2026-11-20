pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Unsaved scripts from a session that ended without saving them (a crash or a
// forced shutdown), kept by the Drafts singleton. Opening one puts its text in
// a new tab; when its connection is not open, that connects first, because the
// user asked for it. Nothing here connects by itself.
RagoDialog {
    id: dlg

    title: qsTr("Recover Unsaved Scripts")
    acceptText: qsTr("Close")
    width: Math.min(visibleWidth - 24, 820)
    height: Math.min(visibleHeight - 24, 560)

    // Nothing left to recover: no empty dialog.
    Connections {
        target: Drafts
        function onCountChanged() {
            if (Drafts.count === 0 && dlg.visible)
                dlg.close()
        }
    }

    contentItem: ColumnLayout {
        spacing: 10

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.disabledTextColor
            font.pixelSize: 12
            text: qsTr("%1 ended while some scripts had unsaved changes. Open one to continue editing; nothing is connected until you do.").arg(Brand.productName)
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 4
            model: Drafts

            delegate: Rectangle {
                id: entry
                required property string draftId
                required property string title
                required property string serverName
                required property string serverUuid
                required property string database
                required property string filePath
                required property string preview
                required property string script
                required property string savedAt

                readonly property bool connected: Backend.connectedUuids.indexOf(serverUuid) >= 0

                width: ListView.view.width
                implicitHeight: entryGrid.implicitHeight + 14
                radius: 8
                color: rowHover.hovered ? Theme.hoverColor : Theme.surfaceColor
                border.width: 1
                border.color: Theme.glassBorder

                HoverHandler { id: rowHover }

                GridLayout {
                    id: entryGrid
                    anchors.fill: parent
                    anchors.margins: 7
                    anchors.leftMargin: 10
                    columns: entry.width < 520 ? 1 : 2
                    columnSpacing: 8
                    rowSpacing: 6

                    ColumnLayout {
                        id: entryCol
                        Layout.fillWidth: true
                        spacing: 3

                        Label {
                            Layout.fillWidth: true
                            text: entry.title.length > 0 ? entry.title : qsTr("Untitled Script")
                            font.bold: true
                            font.pixelSize: 13
                            elide: Text.ElideRight
                            color: Theme.textColor
                        }
                        Label {
                            Layout.fillWidth: true
                            text: entry.preview
                            font.family: AppSettings.codeFontFamily
                            font.pixelSize: AppSettings.codeFontSize
                            elide: Text.ElideRight
                            color: Theme.textColor
                        }
                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font.pixelSize: 11
                            color: Theme.disabledTextColor
                            text: {
                                var parts = []
                                if (entry.serverName.length > 0) parts.push(Language.isolate(entry.serverName))
                                if (entry.database.length > 0) parts.push(Language.isolate(entry.database))
                                if (entry.filePath.length > 0) parts.push(Language.isolate(entry.filePath))
                                parts.push(entry.savedAt)
                                return parts.join("  ·  ")
                            }
                        }
                    }

                    RowLayout {
                        Layout.alignment: entry.width < 520 ? Qt.AlignRight : Qt.AlignVCenter
                        spacing: 4
                        RagoPillButton {
                            text: entry.connected ? qsTr("Open") : qsTr("Connect & Open")
                            primary: true
                            onClicked: Backend.recoverDraft(entry.draftId)
                        }
                        RagoPillButton {
                            text: qsTr("Copy")
                            onClicked: Backend.copyToClipboard(entry.script)
                        }
                        RagoIconButton {
                            glyph: "✕"
                            danger: true
                            size: 26
                            toolTipText: qsTr("Discard this script")
                            onClicked: Drafts.discard(entry.draftId)
                        }
                    }
                }
            }
            ScrollBar.vertical: ScrollBar {}
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            RagoPillButton {
                text: qsTr("Discard All")
                danger: true
                onClicked: discardAllConfirm.open()
            }
        }
    }

    RagoDialog {
        id: discardAllConfirm
        title: qsTr("Discard Unsaved Scripts")
        acceptText: qsTr("Discard")
        dangerAccept: true
        showReject: true
        width: Math.min(visibleWidth - 24, 420)
        contentItem: Label {
            text: qsTr("Permanently discard all unsaved scripts in this list?")
            wrapMode: Text.WordWrap
            color: Theme.textColor
        }
        onAccepted: Drafts.discardAll()
    }
}
