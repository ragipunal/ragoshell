import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

TextField {
    id: control

    // Host names, ports, paths, JSON and identifiers read left to right even in a
    // right-to-left UI (Arabic); other fields follow the UI direction.
    // Most fields here hold host names, identifiers, JSON or paths: no auto-capitals and no
    // word suggestions from the on-screen keyboard. numeric: a digits keypad.
    property bool numeric: false
    inputMethodHints: numeric ? (Qt.ImhDigitsOnly | Qt.ImhNoPredictiveText)
                              : (echoMode === TextInput.Password
                                 ? (Qt.ImhSensitiveData | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText)
                                 : (Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText))

    property bool ltr: false
    LayoutMirroring.enabled: ltr ? false : Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    property string tintColor: ""
    readonly property bool hasTint: tintColor.length > 0
    readonly property color parsedTint: hasTint ? Qt.color(tintColor) : "transparent"

    implicitHeight: Theme.fieldHeight
    implicitWidth: 200
    // The 200 is a wish, not a need: in a narrow row the field gives way instead of pushing the page wider.
    Layout.minimumWidth: 52
    leftPadding: 10
    rightPadding: 10
    topPadding: 0
    bottomPadding: 0
    font.pixelSize: 13
    color: Theme.textColor
    placeholderTextColor: Theme.disabledTextColor
    selectionColor: Theme.accentColor
    selectedTextColor: Theme.onAccentColor
    selectByMouse: true
    opacity: enabled ? 1.0 : 0.45

    background: Rectangle {
        radius: 7
        color: control.hasTint
               ? Qt.rgba(control.parsedTint.r, control.parsedTint.g, control.parsedTint.b,
                         control.activeFocus ? (Theme.dark ? 0.18 : 0.14)
                                             : (Theme.dark ? 0.12 : 0.09))
               : (control.activeFocus ? Theme.surfaceColor : Theme.glassBackground)
        border.width: (control.hasTint && control.activeFocus) ? 2 : 1
        border.color: control.hasTint
                      ? (control.activeFocus
                         ? control.parsedTint
                         : Qt.rgba(control.parsedTint.r, control.parsedTint.g, control.parsedTint.b, 0.72))
                      : (control.activeFocus ? Theme.accentColor
                         : (control.hovered ? Theme.glassBorderHover : Theme.glassBorder))

        Behavior on color { ColorAnimation { duration: 120 } }
        Behavior on border.color { ColorAnimation { duration: 120 } }
    }
}
