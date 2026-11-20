import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root

    property string text: ""
    property string iconText: ""
    property bool primary: false
    property bool danger: false
    property bool compact: false
    property bool checkable: false
    property bool checked: false
    property alias hovered: mouseArea.containsMouse

    signal clicked()

    readonly property bool _symbolOnly: root.text.length <= 2 && root.iconText.length === 0
    readonly property bool _isCompact: root.compact || root.implicitHeight <= 28
    readonly property int _hPad: root._symbolOnly ? 14 : (root._isCompact ? 16 : 20)
    readonly property bool _hot: (!Theme.touch && mouseArea.containsMouse) || mouseArea.pressed

    // Outer item provides the touch hit target; inner bg renders sleek minimalist chrome.
    implicitHeight: root.compact ? (Theme.touch ? 32 : 25) : Theme.controlHeight
    implicitWidth: Math.max(root._symbolOnly ? 30 : (root._isCompact ? 42 : 58),
                            contentRow.implicitWidth + root._hPad)
    property bool stretch: false
    Layout.minimumWidth: root._symbolOnly ? 28 : 38
    Layout.fillWidth: true
    Layout.maximumWidth: stretch ? 16777215 : Math.max(implicitWidth, Layout.preferredWidth)
    Layout.preferredHeight: Theme.touch ? Math.max(implicitHeight, root._isCompact ? 32 : Theme.controlHeight) : implicitHeight
    opacity: enabled ? 1.0 : 0.45

    scale: !enabled ? 1.0 : (mouseArea.pressed ? 0.96 : ((!Theme.touch && mouseArea.containsMouse) ? 1.015 : 1.0))
    Behavior on scale {
        NumberAnimation { duration: 110; easing.type: Easing.OutQuad }
    }

    Rectangle {
        id: bg
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: Theme.touch ? Math.max(24, root.height - (root._isCompact ? 5 : 4)) : root.height
        radius: root._isCompact ? 7 : 8

        gradient: root.primary ? primaryGrad : (root.danger ? dangerGrad : null)
        color: (root.primary || root.danger) ? "transparent" :
               (root.checked ? Theme.selectedColor :
               (root._hot ? Theme.cardBackgroundHover : Theme.glassBackground))

        border.width: 1
        border.color: root.primary ?
                      (root._hot ? "#7affb8" : Qt.rgba(1, 1, 1, 0.30)) :
                      root.danger ?
                      (root._hot ? "#ff9494" : Qt.rgba(1, 0.45, 0.45, 0.42)) :
                      (root.checked ? Theme.accentColor :
                      (root._hot ? Theme.glassBorderHover : Theme.glassBorder))

        Behavior on color { ColorAnimation { duration: 120 } }
        Behavior on border.color { ColorAnimation { duration: 120 } }

        Gradient {
            id: primaryGrad
            GradientStop {
                position: 0.0
                color: root._hot ? "#20fa84" : "#00e668"
            }
            GradientStop {
                position: 1.0
                color: root._hot ? "#00be54" : "#00a344"
            }
        }

        Gradient {
            id: dangerGrad
            GradientStop {
                position: 0.0
                color: root._hot ? "#ff6161" : "#eb4444"
            }
            GradientStop {
                position: 1.0
                color: root._hot ? "#d93232" : "#c42b2b"
            }
        }
    }

    RowLayout {
        id: contentRow
        anchors.centerIn: bg
        width: Math.min(implicitWidth, Math.max(0, root.width - (root._hPad - 4)))
        spacing: root._isCompact ? 4 : 5

        Label {
            visible: root.iconText.length > 0
            text: root.iconText
            font.pixelSize: root._isCompact ? 11 : 12
            color: root.primary ? "#05140e" :
                   root.danger ? "#ffffff" :
                   (root.checked ? Theme.accentColor : Theme.textColor)
        }

        Label {
            visible: root.text.length > 0
            text: root.text
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            font.pixelSize: 12
            font.weight: (root.primary || root.danger || root.checked) ? Font.Bold : Font.Medium
            color: root.primary ? "#05140e" :
                   root.danger ? "#ffffff" :
                   (root.checked ? Theme.accentColor : Theme.textColor)
        }
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        enabled: root.enabled
        hoverEnabled: !Theme.touch
        cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: {
            if (root.checkable)
                root.checked = !root.checked
            root.clicked()
        }
    }
}
