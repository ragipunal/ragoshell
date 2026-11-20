import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: dlg

    property string acceptText: qsTr("OK")
    property string rejectText: qsTr("Cancel")
    property bool showReject: false
    property bool closable: true
    property bool showFooter: true
    property bool dangerAccept: false
    property bool acceptEnabled: true
    property var beforeAccept: null

    parent: Overlay.overlay
    // The safe area that the on-screen keyboard and system cutouts leave free; dialogs size
    // themselves by it instead of by the whole window, and are centered in it (docs/Platforms.md).
    readonly property real visibleWidth: visibleArea.width
    readonly property real visibleHeight: visibleArea.height
    // Little room is left (a phone, or a tablet with the keyboard open): no title row, tighter padding,
    // and dialogs may leave out secondary rows (see ConnectionDialog).
    readonly property bool cramped: visibleHeight < 380
    VisibleArea { id: visibleArea; reference: dlg.parent }
    height: Math.min(implicitHeight, visibleHeight - 16)
    x: Math.round(visibleArea.left + Math.max(0, (visibleWidth - width) / 2))
    y: Math.round(visibleArea.top + Math.max(0, (visibleHeight - height) / 2))
    modal: true
    closePolicy: closable ? Popup.CloseOnEscape : Popup.NoAutoClose
    topInset: 0
    bottomInset: 0
    leftInset: 0
    rightInset: 0
    topPadding: cramped ? 8 : 12
    bottomPadding: cramped ? 8 : 12
    leftPadding: width < 420 ? 12 : 18
    rightPadding: width < 420 ? 12 : 18

    Overlay.modal: Rectangle {
        color: Theme.dark ? "#99000000" : "#55000000"
    }

    background: Rectangle {
        radius: 14
        color: Theme.baseColor
        border.width: 1
        border.color: Theme.glassBorder

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            height: 1
            color: Theme.dark ? Qt.rgba(1, 1, 1, 0.10) : Qt.rgba(1, 1, 1, 0.70)
        }
    }

    header: Item {
        visible: dlg.title.length > 0 && !dlg.cramped
        implicitHeight: visible ? 42 : 0

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: dlg.width < 420 ? 12 : 18
            anchors.rightMargin: 10
            spacing: 8

            Label {
                text: dlg.title
                font.bold: true
                font.pixelSize: 14
                color: Theme.textColor
                elide: Text.ElideRight
                Layout.fillWidth: true
                Layout.minimumWidth: 0
            }

            RagoIconButton {
                visible: dlg.closable
                glyph: "✕"
                size: 24
                toolTipText: qsTr("Close")
                onClicked: dlg.reject()
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: Theme.glassBorder
        }
    }

    footer: Item {
        visible: dlg.showFooter
        implicitHeight: visible ? 48 : 0

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: Theme.glassBorder
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: dlg.width < 420 ? 12 : 18
            anchors.rightMargin: dlg.width < 420 ? 12 : 18
            spacing: 8

            Item { Layout.fillWidth: true }

            RagoPillButton {
                visible: dlg.showReject
                text: dlg.rejectText
                onClicked: dlg.reject()
            }

            RagoPillButton {
                text: dlg.acceptText
                enabled: dlg.acceptEnabled
                primary: !dlg.dangerAccept
                danger: dlg.dangerAccept
                onClicked: {
                    if (dlg.beforeAccept && !dlg.beforeAccept())
                        return
                    dlg.accept()
                }
            }
        }
    }
}
