pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Query history and saved snippets (QueryHistory singleton). Entries can be
// loaded into the current shell tab, run there, or opened in a new tab.
RagoDialog {
    id: dlg

    required property var app   // Main window: currentShellPage(), currentShell()

    title: qsTr("Query History & Snippets")
    acceptText: qsTr("Close")
    width: Math.min(visibleWidth - 24, 860)
    height: Math.min(visibleHeight - 24, 640)

    property int _renameId: -1

    function openHistory(snippets) {
        QueryHistory.snippetsOnly = !!snippets
        open()
    }

    // Server to open a new tab on: the entry's own if connected, else the
    // current tab's, else any connected server.
    function _targetServer(uuid) {
        if (uuid && Backend.connectedUuids.indexOf(uuid) >= 0)
            return uuid
        var s = dlg.app.currentShell()
        if (s)
            return String(s.serverUuid).split("@")[0]
        return Backend.firstConnectedUuid()
    }
    function loadEntry(script, run) {
        var p = dlg.app.currentShellPage()
        if (!p)
            return false
        p.loadScript(script)
        if (run)
            p.executeCurrent()
        dlg.close()
        return true
    }
    function openInNewTab(script, database, uuid) {
        var target = _targetServer(uuid)
        if (target.length === 0)
            return
        Backend.openShellWithScript(target, database, script, false)
        dlg.close()
    }

    onOpened: searchField.forceActiveFocus()

    contentItem: ColumnLayout {
        spacing: 10

        GridLayout {
            Layout.fillWidth: true
            columns: dlg.width < 560 ? 1 : 2
            columnSpacing: 6
            rowSpacing: 6

            RowLayout {
                Layout.fillWidth: dlg.width < 560
                spacing: 6

                RagoPillButton {
                    text: qsTr("History")
                    checked: !QueryHistory.snippetsOnly
                    onClicked: QueryHistory.snippetsOnly = false
                }
                RagoPillButton {
                    text: qsTr("Snippets")
                    iconText: "★"
                    checked: QueryHistory.snippetsOnly
                    onClicked: QueryHistory.snippetsOnly = true
                }
                Item {
                    visible: dlg.width < 560
                    Layout.fillWidth: true
                }
                RagoPillButton {
                    visible: !QueryHistory.snippetsOnly && dlg.width < 560
                    text: qsTr("Clear History")
                    danger: true
                    onClicked: clearConfirm.open()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                RagoTextField {
                    id: searchField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Search script, snippet name or database…")
                    text: QueryHistory.filterText
                    onTextChanged: QueryHistory.filterText = text
                }
                RagoPillButton {
                    visible: !QueryHistory.snippetsOnly && dlg.width >= 560
                    text: qsTr("Clear History")
                    danger: true
                    onClicked: clearConfirm.open()
                }
            }
        }

        Label {
            visible: list.count === 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.WordWrap
            color: Theme.disabledTextColor
            text: QueryHistory.snippetsOnly
                  ? qsTr("No snippets yet. Star a history entry, or use Shell › Save Script as Snippet.")
                  : (QueryHistory.filterText.length > 0 ? qsTr("No matching queries.")
                                                         : qsTr("Executed scripts will appear here."))
        }

        ListView {
            id: list
            visible: count > 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 4
            model: QueryHistory

            delegate: Rectangle {
                id: entry
                required property var entryId
                required property string script
                required property string preview
                required property string database
                required property string serverName
                required property string serverUuid
                required property string executedAt
                required property var durationMs
                required property int resultCount
                required property bool ok
                required property string error
                required property bool favorite
                required property string name

                width: ListView.view.width
                implicitHeight: entryGrid.implicitHeight + 14
                radius: 8
                color: rowHover.hovered ? Theme.hoverColor : Theme.surfaceColor
                border.width: 1
                border.color: Theme.glassBorder

                HoverHandler { id: rowHover }
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onDoubleTapped: {
                        if (!dlg.loadEntry(entry.script, false))
                            dlg.openInNewTab(entry.script, entry.database, entry.serverUuid)
                    }
                }

                GridLayout {
                    id: entryGrid
                    anchors.fill: parent
                    anchors.margins: 7
                    anchors.leftMargin: 8
                    columns: entry.width < 540 ? 1 : 2
                    columnSpacing: 8
                    rowSpacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        RagoIconButton {
                            glyph: entry.favorite ? "★" : "☆"
                            accent: entry.favorite
                            size: 26
                            toolTipText: entry.favorite ? qsTr("Remove from snippets") : qsTr("Save as snippet")
                            onClicked: QueryHistory.setFavorite(entry.entryId, !entry.favorite)
                        }

                        ColumnLayout {
                            id: entryCol
                            Layout.fillWidth: true
                            spacing: 3

                            Label {
                                visible: entry.name.length > 0
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: entry.name
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
                                color: entry.ok ? Theme.disabledTextColor : Theme.dangerColor
                                text: {
                                    var parts = []
                                    if (entry.database.length > 0) parts.push(entry.database)
                                    if (entry.serverName.length > 0) parts.push(entry.serverName)
                                    parts.push(entry.executedAt)
                                    if (entry.ok)
                                        parts.push(qsTr("%n doc(s), %1 ms", "", entry.resultCount).arg(entry.durationMs))
                                    else
                                        parts.push(qsTr("error: %1").arg(Language.isolate(entry.error)))
                                    return parts.join("  ·  ")
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.alignment: entry.width < 540 ? Qt.AlignRight : Qt.AlignVCenter
                        spacing: 4
                        RagoPillButton {
                            text: qsTr("Load")
                            enabled: !!dlg.app.currentShellPage()
                            onClicked: dlg.loadEntry(entry.script, false)
                        }
                        RagoPillButton {
                            //: Button: run the selected query from the history again.
                            text: qsTr("Run")
                            primary: true
                            enabled: !!dlg.app.currentShellPage()
                            onClicked: dlg.loadEntry(entry.script, true)
                        }
                        RagoPillButton {
                            text: qsTr("New Tab")
                            enabled: Backend.connectedUuids.length > 0
                            onClicked: dlg.openInNewTab(entry.script, entry.database, entry.serverUuid)
                        }
                        RagoIconButton {
                            visible: entry.favorite
                            glyph: "✎"
                            size: 26
                            toolTipText: qsTr("Rename snippet")
                            onClicked: {
                                dlg._renameId = entry.entryId
                                renameDialog.initialText = entry.name
                                renameDialog.open()
                            }
                        }
                        RagoIconButton {
                            glyph: "✕"
                            danger: true
                            size: 26
                            toolTipText: qsTr("Delete")
                            onClicked: QueryHistory.remove(entry.entryId)
                        }
                    }
                }
            }
            ScrollBar.vertical: ScrollBar {}
        }
    }

    InputDialog {
        id: renameDialog
        title: qsTr("Rename Snippet")
        label: qsTr("Snippet name")
        onSubmitted: function(text) { QueryHistory.rename(dlg._renameId, text) }
    }

    RagoDialog {
        id: clearConfirm
        title: qsTr("Clear Query History")
        acceptText: qsTr("Clear")
        dangerAccept: true
        showReject: true
        width: Math.min(visibleWidth - 24, 420)
        contentItem: Label {
            text: qsTr("Remove all history entries? Snippets are kept.")
            wrapMode: Text.WordWrap
            color: Theme.textColor
        }
        onAccepted: QueryHistory.clearHistory()
    }
}
