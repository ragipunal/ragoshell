import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

MenuItem {
    id: control

    property bool danger: false
    property string shortcutText: ""
    // A key sequence in the portable spelling ("Ctrl+Shift+C"): written the way this platform writes it, and
    // only where shortcuts are shown (docs/DesignSystem.md). shortcutText wins when it is set.
    property string keys: ""
    readonly property string _hint: shortcutText.length > 0
                                    ? shortcutText
                                    : (keys.length > 0 && Platform.shortcutsVisible ? Platform.keyLabel(keys) : "")

    width: parent ? parent.width : 240
    height: visible ? Theme.menuItemHeight : 0
    implicitHeight: visible ? Theme.menuItemHeight : 0
    padding: 0
    leftPadding: 10
    rightPadding: 10
    hoverEnabled: true

    readonly property bool _pressedState: control.enabled && control.down
    readonly property bool _hoverState: control.enabled && control.highlighted

    contentItem: RowLayout {
        spacing: 12
        opacity: control.enabled ? 1.0 : 0.42

        Label {
            Layout.fillWidth: true
            text: control.text
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            color: {
                if (Theme.dark) {
                    if (control.danger)
                        return control._pressedState ? "#ffa8a8" : (control._hoverState ? "#ff8787" : "#ff6b6b")
                    return control._pressedState ? "#86efac" : (control._hoverState ? "#4ade80" : Theme.textColor)
                }
                if (control.danger)
                    return control._pressedState ? "#991b1b" : (control._hoverState ? "#b91c1c" : "#dc2626")
                return control._pressedState ? "#064e3b" : (control._hoverState ? "#065f46" : Theme.textColor)
            }
        }

        Label {
            visible: control._hint.length > 0
            text: control._hint
            font.pixelSize: 11
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignRight
            color: {
                if (Theme.dark) {
                    if (control.danger)
                        return control._hoverState ? "#ffa8a8" : "#cc5858"
                    return control._hoverState ? "#6ee7b7" : Theme.disabledTextColor
                }
                if (control.danger)
                    return control._hoverState ? "#b91c1c" : "#e05252"
                return control._hoverState ? "#059669" : "#6b7f74"
            }
        }
    }

    background: Rectangle {
        radius: 6
        color: {
            if (!control.enabled)
                return "transparent"
            if (Theme.dark) {
                if (control.danger)
                    return control._pressedState ? "#472024" : (control._hoverState ? "#35181b" : "transparent")
                return control._pressedState ? "#1f4735" : (control._hoverState ? "#173628" : "transparent")
            }
            if (control.danger)
                return control._pressedState ? "#fecdd3" : (control._hoverState ? "#fee4e6" : "transparent")
            return control._pressedState ? "#cceee0" : (control._hoverState ? "#e4f5ed" : "transparent")
        }

        Behavior on color { ColorAnimation { duration: 80 } }
    }
}
