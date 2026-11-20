import QtQuick
import QtQuick.Controls.Basic

Item {
    id: root

    property string text: ""
    property color badgeColor: Theme.accentColor

    implicitWidth: badgeLabel.implicitWidth + 10
    implicitHeight: 19

    Rectangle {
        anchors.fill: parent
        radius: 5
        color: Qt.rgba(root.badgeColor.r, root.badgeColor.g, root.badgeColor.b, 0.15)
        border.width: 1
        border.color: Qt.rgba(root.badgeColor.r, root.badgeColor.g, root.badgeColor.b, 0.35)

        Label {
            id: badgeLabel
            anchors.centerIn: parent
            text: root.text
            font.pixelSize: 10
            font.bold: true
            color: root.badgeColor
        }
    }
}
