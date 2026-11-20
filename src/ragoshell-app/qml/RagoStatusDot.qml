import QtQuick

Item {
    id: root

    property bool online: true
    property bool pulsing: true
    property int dotSize: 8

    implicitWidth: dotSize * 2
    implicitHeight: dotSize * 2

    readonly property color dotColor: root.online ? "#00e668" : "#ff5c5c"

    // Outer pulsing halo ring
    Rectangle {
        id: halo
        anchors.centerIn: parent
        width: root.dotSize * 2
        height: root.dotSize * 2
        radius: width / 2
        color: root.dotColor
        opacity: 0.35
        visible: root.online && root.pulsing

        SequentialAnimation on scale {
            running: root.online && root.pulsing
            loops: Animation.Infinite
            NumberAnimation { from: 0.8; to: 1.6; duration: 1600; easing.type: Easing.OutSine }
            NumberAnimation { from: 1.6; to: 0.8; duration: 1600; easing.type: Easing.InSine }
        }

        SequentialAnimation on opacity {
            running: root.online && root.pulsing
            loops: Animation.Infinite
            NumberAnimation { from: 0.45; to: 0.05; duration: 1600; easing.type: Easing.OutSine }
            NumberAnimation { from: 0.05; to: 0.45; duration: 1600; easing.type: Easing.InSine }
        }
    }

    // Solid inner core dot
    Rectangle {
        anchors.centerIn: parent
        width: root.dotSize
        height: root.dotSize
        radius: root.dotSize / 2
        color: root.dotColor

        border.width: 1
        border.color: Qt.rgba(1, 1, 1, 0.4)
    }
}
