pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

ColumnLayout {
    id: savedSection
    required property var pane
    spacing: 0

    component ActionItem: RagoMenuItem {}
    component ActionSeparator: RagoMenuSeparator {}

    // ---- Saved connections header ----------------------------------
    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 14
        Layout.rightMargin: 14
        Layout.topMargin: 12
        Layout.bottomMargin: 8

        Label {
            text: qsTr("SAVED CONNECTIONS")
            font.bold: true
            font.pixelSize: 11
            font.letterSpacing: Language.rtl ? 0 : 1.1
            color: Theme.disabledTextColor
            Layout.fillWidth: true
        }

        RagoIconButton {
            glyph: "+"
            size: 24
            accent: true
            toolTipText: qsTr("New Connection")
            onClicked: savedSection.pane.newConnectionRequested()
        }
    }

    ListView {
        id: connList
        Layout.fillWidth: true
        Layout.preferredHeight: connList.count === 0
                                ? 52
                                : Math.min(contentHeight,
                                           Backend.connectedUuids.length > 0 ? 176 : 260)
        clip: true
        spacing: 8
        model: ConnectionModel

        delegate: Item {
            id: connDelegate
            required property int index
            required property var model
            width: connList.width
            height: 54

            readonly property bool isOnline: Backend.connectedUuids.indexOf(connDelegate.model.uuid) >= 0

            property bool _longPressTriggered: false

            function openConnMenu(targetItem, x, y) {
                savedSection.pane._ctxConnRow = connDelegate.index
                savedSection.pane._ctxConnUuid = connDelegate.model.uuid
                savedSection.pane._ctxConnName = connDelegate.model.name || connDelegate.model.address
                savedSection.pane._ctxConnOnline = connDelegate.isOnline

                if (targetItem) {
                    if (targetItem === connDelegate || targetItem === glassCard) {
                        let posX = (x !== undefined) ? x : 16
                        let posY = (y !== undefined) ? y : targetItem.height
                        let rootPt = targetItem.mapToItem(null, posX, posY)
                        let windowW = (Window.width > 0) ? Window.width : 800
                        if (rootPt.x + connMenu.width > windowW - 8) {
                            let overflow = (rootPt.x + connMenu.width) - (windowW - 8)
                            posX = Math.max(8, posX - overflow)
                        }
                        connMenu.popup(targetItem, Math.max(8, posX), posY)
                    } else {
                        let menuX = targetItem.width - connMenu.width
                        connMenu.popup(targetItem, menuX, targetItem.height + 4)
                    }
                } else {
                    connMenu.popup(connDelegate, 16, connDelegate.height)
                }
            }

            RagoGlassCard {
                id: glassCard
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                active: connDelegate.isOnline
                tintColor: connDelegate.model.colorTag || ""
                onDoubleClicked: Backend.connectToServer(connDelegate.model.uuid)
                onClicked: {
                    if (connDelegate._longPressTriggered) {
                        connDelegate._longPressTriggered = false
                        return
                    }
                    if (Platform.isMobile)
                        Backend.connectToServer(connDelegate.model.uuid)
                }

                TapHandler {
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.Stylus
                    acceptedButtons: Qt.RightButton
                    onTapped: (point) => connDelegate.openConnMenu(glassCard, point.position.x, point.position.y)
                }

                TapHandler {
                    id: connLongPress
                    acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
                    acceptedButtons: Qt.LeftButton
                    longPressThreshold: 0.8
                    onPressedChanged: {
                        if (pressed)
                            connDelegate._longPressTriggered = false
                    }
                    onLongPressed: {
                        connDelegate._longPressTriggered = true
                        connDelegate.openConnMenu(glassCard, connLongPress.point.position.x, connLongPress.point.position.y)
                    }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 10
                    spacing: 8

                    RagoStatusDot {
                        online: connDelegate.isOnline
                        pulsing: connDelegate.isOnline
                        Layout.alignment: Qt.AlignVCenter
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 36
                        spacing: 2
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 5

                            Label {
                                text: connDelegate.model.name || connDelegate.model.address
                                font.bold: true
                                font.pixelSize: 13
                                color: Theme.textColor
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                            }

                            RagoBadge {
                                visible: connDelegate.model.isReplicaSet
                                text: "RS"
                                badgeColor: Theme.accentColor
                            }
                            RagoBadge {
                                visible: connDelegate.model.sslEnabled
                                text: "TLS"
                                badgeColor: "#60a5fa"
                            }
                            RagoBadge {
                                visible: connDelegate.model.sshEnabled
                                text: "SSH"
                                badgeColor: "#f59e0b"
                            }
                        }
                        Label {
                            text: connDelegate.model.authEnabled ? qsTr("%1 · Auth").arg(connDelegate.model.address)
                                               : connDelegate.model.address
                            font.pixelSize: 11
                            color: Theme.disabledTextColor
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                        }
                    }

                    RowLayout {
                        spacing: 2
                        RagoIconButton {
                            glyph: connDelegate.isOnline ? "↻" : "▶"
                            size: 24
                            accent: true
                            toolTipText: connDelegate.isOnline ? qsTr("Refresh Server") : qsTr("Connect")
                            onClicked: {
                                if (connDelegate.isOnline)
                                    Backend.refreshServer(connDelegate.model.uuid)
                                else
                                    Backend.connectToServer(connDelegate.model.uuid)
                            }
                        }
                        RagoIconButton {
                            glyph: "✎\uFE0E"
                            size: 24
                            toolTipText: qsTr("Edit Connection")
                            onClicked: savedSection.pane.editConnectionRequested(connDelegate.index)
                        }
                        RagoIconButton {
                            id: connActionBtn
                            glyph: "⋮"
                            size: 24
                            toolTipText: qsTr("Connection Actions")
                            onClicked: connDelegate.openConnMenu(connActionBtn)
                        }
                    }
                }
            }
        }

        Label {
            anchors.centerIn: parent
            visible: connList.count === 0
            text: qsTr("No saved connections yet")
            font.pixelSize: 12
            color: Theme.disabledTextColor
        }

        ScrollBar.vertical: ScrollBar {}
    }

    RagoPillButton {
        compact: true
        text: qsTr("New Connection")
        iconText: "+"
        stretch: true
        Layout.margins: 10
        Layout.topMargin: 6
        onClicked: savedSection.pane.newConnectionRequested()
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: Theme.glassBorder
        Layout.topMargin: 2
        Layout.bottomMargin: 8
    }


    RagoMenu {
        id: connMenu
        width: 215

        ActionItem {
            text: savedSection.pane._ctxConnOnline ? qsTr("Refresh Server") : qsTr("Connect")
            onTriggered: {
                if (savedSection.pane._ctxConnOnline)
                    Backend.refreshServer(savedSection.pane._ctxConnUuid)
                else
                    Backend.connectToServer(savedSection.pane._ctxConnUuid)
            }
        }
        ActionItem {
            visible: savedSection.pane._ctxConnOnline
            text: qsTr("Server Monitor")
            onTriggered: Backend.openMonitor(savedSection.pane._ctxConnUuid)
        }
        ActionItem {
            visible: savedSection.pane._ctxConnOnline
            text: qsTr("Disconnect")
            onTriggered: savedSection.pane.requestDisconnectServer(savedSection.pane._ctxConnUuid, savedSection.pane._ctxConnName)
        }
        ActionSeparator {}
        ActionItem {
            text: qsTr("Edit Connection…")
            onTriggered: savedSection.pane.editConnectionRequested(savedSection.pane._ctxConnRow)
        }
        ActionItem {
            text: qsTr("Clone Connection")
            onTriggered: ConnectionModel.cloneConnection(savedSection.pane._ctxConnRow)
        }
        ActionItem {
            // The local schema catalog (docs/SchemaIntelligence.md); the server is not touched.
            text: qsTr("Forget Schema Data")
            onTriggered: Backend.forgetSchema(savedSection.pane._ctxConnUuid)
        }
        ActionSeparator {}
        ActionItem {
            text: qsTr("Move Up")
            enabled: savedSection.pane._ctxConnRow > 0
            onTriggered: ConnectionModel.moveUp(savedSection.pane._ctxConnRow)
        }
        ActionItem {
            text: qsTr("Move Down")
            enabled: savedSection.pane._ctxConnRow >= 0 && savedSection.pane._ctxConnRow < ConnectionModel.count - 1
            onTriggered: ConnectionModel.moveDown(savedSection.pane._ctxConnRow)
        }
        ActionSeparator {}
        ActionItem {
            text: qsTr("Remove Connection…")
            danger: true
            onTriggered: {
                savedSection.pane.openDestructiveConfirm({
                    action: "removeConn",
                    uuid: savedSection.pane._ctxConnUuid,
                    serverLabel: savedSection.pane._ctxConnName,
                    title: qsTr("Remove Saved Connection"),
                    acceptText: qsTr("Remove Connection"),
                    msg: qsTr("Permanently remove saved connection '%1'?").arg(Language.isolate(savedSection.pane._ctxConnName)),
                    consequence: savedSection.pane._ctxConnOnline
                        ? qsTr("This connection is currently active — it will be disconnected and its saved credentials/SSH/TLS settings will be deleted from %1.").arg(Brand.productName)
                        : qsTr("Saved host, credentials, SSH/TLS settings, and color tag for this connection will be deleted from %1. Your MongoDB server data is not affected.").arg(Brand.productName)
                })
            }
        }
    }
}
