import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Generic single-line input dialog. Set 'title', 'label', 'initialText',
// open(), and listen to 'submitted(text)'.
RagoDialog {
    id: dlg
    property string label: ""
    property string initialText: ""
    property string placeholderText: ""
    signal submitted(string text)

    acceptText: qsTr("OK")
    rejectText: qsTr("Cancel")
    showReject: true
    width: Math.min(visibleWidth - 24, 400)

    onAccepted: submitted(field.text.trim())

    contentItem: ColumnLayout {
        spacing: 10

        Label {
            visible: dlg.label.length > 0
            text: dlg.label
            wrapMode: Text.WordWrap
            font.pixelSize: 13
            color: Theme.textColor
            Layout.fillWidth: true
        }

        RagoTextField {
            id: field
            placeholderText: dlg.placeholderText
            Layout.fillWidth: true
            onAccepted: dlg.accept()
        }
    }

    onOpened: {
        field.text = dlg.initialText
        field.selectAll()
        field.forceActiveFocus()
    }
}
