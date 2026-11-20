pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// "Trust this SSH server?" for a host key that is new or has changed
// (Backend.hostKeyRequested). The SSH tunnel waits for the answer, so every
// way out of the dialog answers: Escape, the ✕ and Cancel all refuse.
// Questions that arrive while one is open are shown one after the other.
RagoDialog {
    id: dlg

    property var _queue: []
    property var _current: null          // { id, host, port, keyType, fingerprint, changed }
    readonly property bool changed: !!_current && _current.changed

    title: changed ? qsTr("SSH Host Key Changed") : qsTr("Unknown SSH Server")
    acceptText: changed ? qsTr("Trust New Key") : qsTr("Trust & Connect")
    rejectText: changed ? qsTr("Refuse") : qsTr("Cancel")
    showReject: true
    dangerAccept: changed
    closePolicy: Popup.CloseOnEscape
    width: Math.min(visibleWidth - 24, 560)

    function ask(id, host, port, keyType, fingerprint, changed) {
        _queue.push({ id: id, host: host, port: port, keyType: keyType,
                      fingerprint: fingerprint, changed: changed })
        if (!_current)
            _showNext()
    }

    function _showNext() {
        _current = _queue.length > 0 ? _queue.shift() : null
        if (_current)
            open()
    }

    function _finish(trust) {
        if (_current)
            Backend.answerHostKey(_current.id, trust)
        _current = null
        // The next question (if any) opens once this dialog is fully closed.
        Qt.callLater(_showNext)
    }

    onAccepted: _finish(true)
    onRejected: _finish(false)

    contentItem: ColumnLayout {
        spacing: 10

        Rectangle {
            visible: dlg.changed
            Layout.fillWidth: true
            implicitHeight: warnLabel.implicitHeight + 16
            radius: 8
            color: Qt.rgba(Theme.dangerColor.r, Theme.dangerColor.g, Theme.dangerColor.b, 0.12)
            border.width: 1
            border.color: Qt.rgba(Theme.dangerColor.r, Theme.dangerColor.g, Theme.dangerColor.b, 0.40)

            Label {
                id: warnLabel
                anchors.fill: parent
                anchors.margins: 8
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                font.weight: Font.DemiBold
                color: Theme.dangerColor
                text: qsTr("WARNING: The host key of this server has changed since you last connected. "
                           + "Someone may be intercepting the connection, or the server was reinstalled.")
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: 13
            color: Theme.textColor
            text: dlg.changed
                  ? qsTr("The SSH server %1 presented a different key than the one saved earlier.")
                        .arg(dlg._current ? Language.isolate(dlg._current.host + ":" + dlg._current.port) : "")
                  : qsTr("The authenticity of the SSH server %1 can't be established.")
                        .arg(dlg._current ? Language.isolate(dlg._current.host + ":" + dlg._current.port) : "")
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: keyCol.implicitHeight + 16
            radius: 8
            color: Theme.surfaceColor
            border.width: 1
            border.color: Theme.glassBorder

            ColumnLayout {
                id: keyCol
                anchors.fill: parent
                anchors.margins: 8
                spacing: 4

                Label {
                    text: qsTr("Key type")
                    font.pixelSize: 11
                    color: Theme.disabledTextColor
                }
                Label {
                    text: dlg._current ? dlg._current.keyType : ""
                    font.family: AppSettings.codeFontFamily
                    font.pixelSize: AppSettings.codeFontSize
                    color: Theme.textColor
                }
                Label {
                    text: qsTr("Fingerprint")
                    font.pixelSize: 11
                    color: Theme.disabledTextColor
                }
                TextEdit {
                    Layout.fillWidth: true
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere
                    text: dlg._current ? dlg._current.fingerprint : ""
                    font.family: AppSettings.codeFontFamily
                    font.pixelSize: AppSettings.codeFontSize
                    color: Theme.textColor
                }
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: 12
            color: Theme.disabledTextColor
            text: dlg.changed
                  ? qsTr("Refuse unless the server's administrator confirmed the change and the fingerprint above is the new one.")
                  : qsTr("Compare the fingerprint with the one the server's administrator gave you before you trust it. A trusted key is saved and not asked about again.")
        }
    }
}
