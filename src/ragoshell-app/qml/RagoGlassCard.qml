import QtQuick

Rectangle {
    id: root

    property bool hoverable: true
    property bool active: false
    property string tintColor: ""
    property alias containsMouse: mouseArea.containsMouse

    readonly property bool hasTint: tintColor.length > 0
    readonly property color parsedTint: hasTint ? Qt.color(tintColor) : "transparent"

    signal clicked()
    signal doubleClicked()

    radius: 12
    color: hasTint
           ? Qt.rgba(parsedTint.r, parsedTint.g, parsedTint.b,
                     active ? (Theme.dark ? 0.22 : 0.16)
                            : (hoverable && mouseArea.containsMouse
                               ? (Theme.dark ? 0.16 : 0.13)
                               : (Theme.dark ? 0.10 : 0.08)))
           : (active ? Theme.selectedColor
                     : (hoverable && mouseArea.containsMouse
                        ? Theme.cardBackgroundHover : Theme.cardBackground))

    border.width: active ? 2 : 1
    border.color: hasTint
                  ? (active ? parsedTint
                            : (hoverable && mouseArea.containsMouse
                               ? Qt.rgba(parsedTint.r, parsedTint.g, parsedTint.b, 0.90)
                               : Qt.rgba(parsedTint.r, parsedTint.g, parsedTint.b, 0.62)))
                  : (active ? Theme.accentColor
                            : (hoverable && mouseArea.containsMouse
                               ? Theme.glassBorderHover : Theme.glassBorder))

    Behavior on color { ColorAnimation { duration: 150 } }
    Behavior on border.color { ColorAnimation { duration: 150 } }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: root.hoverable
        cursorShape: root.hoverable ? Qt.PointingHandCursor : Qt.ArrowCursor
        propagateComposedEvents: true
        onClicked: function(mouse) {
            root.clicked()
            mouse.accepted = false
        }
        onDoubleClicked: function(mouse) {
            root.doubleClicked()
            mouse.accepted = false
        }
    }
}
