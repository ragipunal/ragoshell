import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

CheckBox {
    id: control

    spacing: 7
    font.pixelSize: 12
    implicitHeight: Theme.touch ? 32 : 24
    // A long text wraps in a narrow row instead of widening the page; it never grows past its natural width.
    Layout.fillWidth: true
    Layout.minimumWidth: 52
    Layout.maximumWidth: implicitWidth
    opacity: enabled ? 1.0 : 0.45

    indicator: Rectangle {
        implicitWidth: 16
        implicitHeight: 16
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: parent.height / 2 - height / 2
        radius: 4
        color: control.checked ? Theme.accentColor : Theme.glassBackground
        border.width: 1
        border.color: control.checked ? Theme.accentColor
                    : (control.hovered ? Theme.glassBorderHover : Theme.glassBorder)

        Behavior on color { ColorAnimation { duration: 120 } }

        Label {
            anchors.centerIn: parent
            visible: control.checked
            text: "✓"
            font.pixelSize: 12
            font.bold: true
            color: Theme.onAccentColor
        }
    }

    contentItem: Label {
        visible: control.text.length > 0
        text: control.text
        font: control.font
        color: Theme.textColor
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.WordWrap
        // Room for the indicator on its side (the right in right-to-left layouts).
        leftPadding: control.mirrored ? 0 : control.indicator.width + (control.text.length > 0 ? control.spacing : 0)
        rightPadding: control.mirrored ? control.indicator.width + (control.text.length > 0 ? control.spacing : 0) : 0
    }
}
