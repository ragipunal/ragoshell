import QtQuick
import QtQuick.Controls.Basic

MenuSeparator {
    id: sep

    width: parent ? parent.width : 240
    height: visible ? 9 : 0
    implicitHeight: visible ? 9 : 0
    padding: 0
    topPadding: 4
    bottomPadding: 4
    leftPadding: 6
    rightPadding: 6

    contentItem: Rectangle {
        implicitWidth: 200
        implicitHeight: 1
        color: Theme.dark ? Theme.glassBorder : "#e2ebe6"
    }

    background: Item {}
}
