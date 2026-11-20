pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

ComboBox {
    id: control

    implicitHeight: Theme.fieldHeight
    implicitWidth: 168
    leftPadding: mirrored ? 26 : 10
    rightPadding: mirrored ? 10 : 26
    font.pixelSize: 12
    opacity: enabled ? 1.0 : 0.45

    contentItem: Label {
        text: control.displayText
        font: control.font
        color: Theme.textColor
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Label {
        x: control.mirrored ? 9 : control.width - width - 9
        y: (control.availableHeight - height) / 2
        text: "▾"
        font.pixelSize: 11
        color: Theme.disabledTextColor
    }

    background: Rectangle {
        radius: 7
        color: control.down ? Theme.cardBackgroundHover : Theme.glassBackground
        border.width: 1
        border.color: control.visualFocus || control.popup.visible ? Theme.accentColor
                    : (control.hovered ? Theme.glassBorderHover : Theme.glassBorder)

        Behavior on border.color { ColorAnimation { duration: 120 } }
    }

    delegate: ItemDelegate {
        id: itemDel
        required property int index
        required property var model
        required property var modelData
        width: control.width - 8
        implicitHeight: Theme.menuItemHeight
        highlighted: control.highlightedIndex === itemDel.index
        hoverEnabled: true

        contentItem: Label {
            text: (control.textRole && itemDel.model && itemDel.model[control.textRole] !== undefined)
                  ? itemDel.model[control.textRole]
                  : itemDel.modelData
            font: control.font
            color: {
                if (Theme.dark)
                    return (itemDel.down || itemDel.highlighted) ? "#4ade80" : Theme.textColor
                return itemDel.down ? "#064e3b" : (itemDel.highlighted ? "#065f46" : Theme.textColor)
            }
            verticalAlignment: Text.AlignVCenter
            leftPadding: control.mirrored ? 0 : 8
            rightPadding: control.mirrored ? 8 : 0
            elide: Text.ElideRight
        }

        background: Rectangle {
            radius: 6
            color: {
                if (Theme.dark)
                    return itemDel.down ? "#1f4735" : (itemDel.highlighted ? "#173628" : "transparent")
                return itemDel.down ? "#cceee0" : (itemDel.highlighted ? "#e4f5ed" : "transparent")
            }
        }
    }

    popup: Popup {
        y: control.height + 4
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + 8, 260)
        padding: 4

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollBar.vertical: ScrollBar {}
        }

        background: Rectangle {
            radius: 10
            color: Theme.dark ? "#101914" : "#ffffff"
            border.width: 1
            border.color: Theme.dark ? Theme.glassBorder : "#cbd9d1"
        }
    }
}
