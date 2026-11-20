import QtQuick
import QtQuick.Controls.Basic

SpinBox {
    id: control

    implicitHeight: Theme.fieldHeight
    implicitWidth: 118
    editable: true
    font.pixelSize: 12
    opacity: enabled ? 1.0 : 0.45

    function commitText() {
        if (!spinInput.text || spinInput.text.trim().length === 0)
            return
        var parsed = parseInt(spinInput.text.trim(), 10)
        if (!isNaN(parsed)) {
            let clamped = Math.max(control.from, Math.min(control.to, parsed))
            if (control.value !== clamped) {
                control.value = clamped
                control.valueModified()
            }
        }
        spinInput.text = control.textFromValue(control.value, control.locale)
    }

    contentItem: TextInput {
        id: spinInput
        z: 2
        text: control.textFromValue(control.value, control.locale)
        font: control.font
        color: Theme.textColor
        selectionColor: Theme.accentColor
        selectedTextColor: Theme.onAccentColor
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: Qt.ImhDigitsOnly
        onTextEdited: {
            var parsed = parseInt(text.trim(), 10)
            if (!isNaN(parsed) && parsed >= control.from && parsed <= control.to && control.value !== parsed) {
                control.value = parsed
                control.valueModified()
            }
        }
        onEditingFinished: control.commitText()

        Binding {
            target: spinInput
            property: "text"
            value: control.textFromValue(control.value, control.locale)
            when: !spinInput.activeFocus
        }
    }

    down.indicator: Rectangle {
        x: control.mirrored ? parent.width - width : 0
        height: parent.height
        implicitWidth: 28
        radius: 7
        color: control.down.pressed ? Theme.selectedColor
             : (control.down.hovered ? Theme.cardBackgroundHover : "transparent")

        Label {
            anchors.centerIn: parent
            text: "−"
            font.pixelSize: 13
            font.bold: true
            color: control.value > control.from ? Theme.textColor : Theme.disabledTextColor
        }
    }

    up.indicator: Rectangle {
        x: control.mirrored ? 0 : parent.width - width
        height: parent.height
        implicitWidth: 28
        radius: 7
        color: control.up.pressed ? Theme.selectedColor
             : (control.up.hovered ? Theme.cardBackgroundHover : "transparent")

        Label {
            anchors.centerIn: parent
            text: "+"
            font.pixelSize: 13
            font.bold: true
            color: control.value < control.to ? Theme.textColor : Theme.disabledTextColor
        }
    }

    background: Rectangle {
        radius: 7
        color: Theme.glassBackground
        border.width: 1
        border.color: control.activeFocus ? Theme.accentColor
                    : (control.hovered ? Theme.glassBorderHover : Theme.glassBorder)
    }
}
