pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Bottom log panel fed by Backend.logModel (EventBus LogEvent lines).
Rectangle {
    id: panel
    /// Shown in its own window: the window button docks it back.
    property bool detached: false
    signal detachToggled()
    signal closeRequested()

    color: Theme.baseColor
    border.color: Theme.glassBorder
    border.width: 1

    Rectangle {
        id: logHeader
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Math.max(36, logHeaderRow.implicitHeight + 8)
        color: Theme.surfaceColor

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: Theme.glassBorder
        }

        RowLayout {
            id: logHeaderRow
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 10
            spacing: 8

            Label {
                text: "▤"
                font.pixelSize: 13
                color: Theme.accentColor
            }

            Label {
                text: qsTr("Diagnostic Log")
                font.bold: true
                font.pixelSize: 12
                color: Theme.textColor
                elide: Text.ElideRight
                Layout.fillWidth: true
                Layout.minimumWidth: 0
            }

            RagoBadge {
                text: String(Backend.logModel.count)
                badgeColor: Theme.accentColor
            }

            RagoPillButton {
                visible: Platform.isMobile
                text: qsTr("Share…")
                implicitHeight: 24
                Layout.fillWidth: false
                onClicked: Backend.openLogFolder()
            }

            RagoPillButton {
                text: qsTr("Clear")
                implicitHeight: 24
                Layout.fillWidth: false
                enabled: Backend.logModel.count > 0
                onClicked: Backend.logModel.clear()
            }

            RagoIconButton {
                visible: !Platform.isMobile
                glyph: panel.detached ? "⇲" : "❐"
                size: 24
                toolTipText: panel.detached ? qsTr("Dock Log Panel") : qsTr("Open Log in a Separate Window")
                onClicked: panel.detachToggled()
            }

            RagoIconButton {
                glyph: "✕"
                size: 24
                toolTipText: Platform.withShortcut(qsTr("Close Log (%1)"), "Ctrl+L")
                onClicked: panel.closeRequested()
            }
        }
    }

    ListView {
        id: list
        anchors.top: logHeader.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        model: Backend.logModel

        onCountChanged: positionViewAtEnd()

        delegate: Rectangle {
            id: logRow
            required property int index
            required property string time
            required property string severity
            required property string message
            width: list.width
            height: Math.max(26, AppSettings.codeFontSize + 12)
            color: index % 2 === 1 ? Theme.alternateColor : "transparent"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 10

                Label {
                    text: logRow.time
                    font.family: AppSettings.codeFontFamily
                    font.pixelSize: Math.max(11, AppSettings.codeFontSize - 1)
                    color: Theme.disabledTextColor
                    Layout.preferredWidth: 74
                }
                Label {
                    text: logRow.severity
                    font.family: AppSettings.codeFontFamily
                    font.bold: true
                    font.pixelSize: Math.max(11, AppSettings.codeFontSize - 1)
                    color: logRow.severity === "ERROR" ? "#ff6b6b"
                         : logRow.severity === "WARN" ? "#f0b44c"
                         : Theme.accentColor
                    Layout.preferredWidth: 52
                }
                Label {
                    text: logRow.message
                    font.family: AppSettings.codeFontFamily
                    font.pixelSize: Math.max(12, AppSettings.codeFontSize - 1)
                    color: Theme.textColor
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }
            }
        }

        Label {
            anchors.centerIn: parent
            visible: list.count === 0
            text: qsTr("No log entries yet")
            font.pixelSize: 12
            color: Theme.disabledTextColor
        }

        ScrollBar.vertical: ScrollBar {}
    }
}
