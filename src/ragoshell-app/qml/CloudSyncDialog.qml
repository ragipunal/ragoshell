pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

// Dedicated Cloud Account & Encrypted Sync dialog opened from the top bar,
// More menu, or Command Palette without Preferences' unrelated Save/Cancel flow.
RagoDialog {
    id: dlg
    title: qsTr("Cloud & Sync")
    acceptText: qsTr("Close")
    showReject: false
    width: Math.min(visibleWidth - 24, 560)
    height: Math.min(implicitHeight, visibleHeight - 24)

    property bool _startSetupOnOpen: false

    function openDialog(startSetup) {
        _startSetupOnOpen = (startSetup === true)
        open()
    }

    onOpened: {
        accountView.closeEditors()
        accountView.syncFields()
        if (_startSetupOnOpen) {
            _startSetupOnOpen = false
            accountView.openSyncSetup()
        }
    }

    onClosed: {
        _startSetupOnOpen = false
        accountView.commitDeviceName()
        accountView.closeEditors()
    }

    FocusReveal { flickable: syncScroll.contentItem; space: dlg.visibleHeight }

    contentItem: ScrollView {
        id: syncScroll
        clip: true
        contentWidth: availableWidth
        implicitHeight: accountView.implicitHeight

        PreferencesAccountSection {
            id: accountView
            width: syncScroll.availableWidth
            standalone: true
        }
    }
}
