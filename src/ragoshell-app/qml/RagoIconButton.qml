import QtQuick
import QtQuick.Controls.Basic

Item {
    id: root

    property string glyph: ""
    property string toolTipText: ""
    property bool accent: false
    property bool danger: false
    property int size: 28

    signal clicked()

    // Outer item gives a comfortable touch hit box; inner bg stays ghost-clean at rest
    // so toolbars and cards never fill up with heavy outlined circles.
    implicitWidth: Theme.touch ? Math.max(size + 6, Theme.iconButtonSize) : size
    implicitHeight: implicitWidth

    readonly property bool _hot: (!Theme.touch && mouseArea.containsMouse) || mouseArea.pressed

    scale: !enabled ? 1.0 : (mouseArea.pressed ? 0.92 : ((!Theme.touch && mouseArea.containsMouse) ? 1.05 : 1.0))
    Behavior on scale {
        NumberAnimation { duration: 100; easing.type: Easing.OutQuad }
    }

    Rectangle {
        id: bg
        anchors.centerIn: parent
        width: Math.min(parent.width, root.size + (Theme.touch ? 2 : 0))
        height: width
        radius: 7

        color: mouseArea.pressed ?
               (root.danger ? Qt.rgba(1, 0.36, 0.36, 0.24) :
               (root.accent ? Qt.rgba(0, 0.75, 0.33, 0.24) :
               (Theme.dark ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(0, 0, 0, 0.08)))) :
               (root._hot ?
               (root.danger ? Qt.rgba(1, 0.36, 0.36, 0.16) :
               (root.accent ? Qt.rgba(0, 0.75, 0.33, 0.16) : Theme.glassBackground)) :
               (root.accent ? Qt.rgba(0, 0.75, 0.33, Theme.dark ? 0.12 : 0.08) : "transparent"))

        border.width: (root._hot || root.accent) ? 1 : 0
        border.color: root.danger ? "#ff5c5c" :
                      (root.accent ? Qt.rgba(0, 0.75, 0.33, root._hot ? 0.65 : 0.28) : Theme.glassBorderHover)

        Behavior on color { ColorAnimation { duration: 110 } }
    }

    Label {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: root.glyph === "⋮" ? -1 : 0
        text: root.glyph
        font.pixelSize: Math.max(13, Math.round(root.size * 0.50))
        font.bold: root.glyph === "⋮" || root.glyph === "+"
        color: root.danger ? (root._hot ? "#ff6b6b" : Theme.dangerColor) :
               (root.accent ? Theme.accentColor : Theme.textColor)
        opacity: root.enabled ? ((root._hot || root.accent) ? 1.0 : 0.76) : 0.38

        Behavior on color { ColorAnimation { duration: 110 } }
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        enabled: root.enabled
        hoverEnabled: !Theme.touch
        cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: root.clicked()
    }

    ToolTip {
        visible: !Theme.touch && mouseArea.containsMouse && root.toolTipText.length > 0
        delay: 400
        text: root.toolTipText
    }
}
