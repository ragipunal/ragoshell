import QtQuick
import QtQuick.Controls.Basic

Menu {
    id: root

    padding: 5

    palette.window: Theme.dark ? "#101914" : "#ffffff"
    palette.windowText: Theme.textColor
    palette.base: Theme.dark ? "#101914" : "#ffffff"
    palette.text: Theme.textColor
    palette.button: Theme.buttonColor
    palette.buttonText: Theme.textColor
    palette.highlight: Theme.selectedColor
    palette.highlightedText: Theme.textColor
    palette.mid: Theme.hoverColor
    palette.dark: Theme.selectedColor

    background: Rectangle {
        implicitWidth: 220
        radius: 10
        color: Theme.dark ? "#101914" : "#ffffff"
        border.width: 1
        border.color: Theme.dark ? Theme.glassBorder : "#cbd9d1"
    }
}
