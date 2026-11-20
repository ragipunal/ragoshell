pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

GridLayout {
    id: sshTab
    required property var dlg
    required property bool isDirectConnection

    function loadFrom(ssh) {
        var s = ssh || ({})
        sshEnabled.checked = !!s.enabled
        sshHostField.text = s.host || ""
        sshPortField.text = String(s.port || 22)
        sshUserField.text = s.userName || ""
        sshMethodCombo.currentIndex = s.method === "password" ? 1 : 0
        sshPassField.text = s.userPassword || ""
        sshKeyField.text = s.privateKeyFile || ""
        sshPassphraseField.text = s.passphrase || ""
        sshAskPasswordCheck.checked = !!s.askPassword
    }

    function toSshMap(isRs, prevPublicKeyFile) {
        return {
            "enabled": !isRs && sshEnabled.checked,
            "host": sshHostField.text.trim(),
            "port": parseInt(sshPortField.text, 10) || 22,
            "userName": sshUserField.text.trim(),
            "userPassword": sshAskPasswordCheck.checked ? "" : sshPassField.text,
            "method": sshMethodCombo.currentIndex === 1 ? "password" : "publickey",
            "privateKeyFile": sshKeyField.text.trim(),
            "publicKeyFile": prevPublicKeyFile || "",
            "passphrase": sshAskPasswordCheck.checked ? "" : sshPassphraseField.text,
            "askPassword": sshAskPasswordCheck.checked
        }
    }
    columns: Window.width < 520 ? 1 : 2
    columnSpacing: 14
    rowSpacing: 8

    readonly property bool rsBlocksSsh: !sshTab.isDirectConnection
    readonly property bool sshActive: !rsBlocksSsh && sshEnabled.checked

    Label {
        visible: sshTab.rsBlocksSsh
        Layout.columnSpan: Math.min(2, sshTab.columns)
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: qsTr("SSH tunnel is only available for Direct Connections (disabled for Replica Sets and DNS Seedlist).")
        font.pixelSize: 12
        color: Theme.disabledTextColor
    }

    RagoCheckBox {
        id: sshEnabled
        text: qsTr("Use SSH tunnel")
        enabled: !sshTab.rsBlocksSsh
        Layout.columnSpan: Math.min(2, sshTab.columns)
    }

    Label {
        text: qsTr("SSH Address:")
        color: sshTab.sshActive ? Theme.textColor : Theme.disabledTextColor
        font.pixelSize: 13
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        RagoTextField {
            id: sshHostField
            ltr: true
            placeholderText: "ssh.example.com"
            Layout.fillWidth: true
            enabled: sshTab.isDirectConnection && sshEnabled.checked
        }
        Label {
            text: ":"
            color: Theme.disabledTextColor
        }
        RagoTextField {
            id: sshPortField
            ltr: true
            Layout.preferredWidth: 86
            enabled: sshTab.isDirectConnection && sshEnabled.checked
            validator: IntValidator { bottom: 1; top: 65535 }
        }
    }

    Label {
        text: qsTr("User name:")
        color: sshTab.sshActive ? Theme.textColor : Theme.disabledTextColor
        font.pixelSize: 13
    }
    RagoTextField {
        id: sshUserField
        ltr: true
        Layout.fillWidth: true
        enabled: sshTab.sshActive
    }

    Label {
        text: qsTr("Auth Method:")
        color: sshTab.sshActive ? Theme.textColor : Theme.disabledTextColor
        font.pixelSize: 13
    }
    RagoComboBox {
        id: sshMethodCombo
        Layout.fillWidth: true
        enabled: sshTab.sshActive
        model: [qsTr("Private Key"), qsTr("Password")]
    }

    Label {
        visible: sshMethodCombo.currentIndex === 1
        text: qsTr("Password:")
        color: sshTab.sshActive ? Theme.textColor : Theme.disabledTextColor
        font.pixelSize: 13
    }
    RowLayout {
        visible: sshMethodCombo.currentIndex === 1
        Layout.fillWidth: true
        spacing: 6
        RagoTextField {
            id: sshPassField
            Layout.fillWidth: true
            echoMode: sshTab.dlg.showSshPass ? TextInput.Normal : TextInput.Password
            enabled: sshTab.isDirectConnection && sshEnabled.checked && !sshAskPasswordCheck.checked
        }
        RagoIconButton {
            glyph: sshTab.dlg.showSshPass ? "◡" : "◉"
            size: 30
            enabled: sshTab.isDirectConnection && sshEnabled.checked && !sshAskPasswordCheck.checked
            onClicked: sshTab.dlg.showSshPass = !sshTab.dlg.showSshPass
        }
    }

    Label {
        visible: sshMethodCombo.currentIndex === 0
        text: qsTr("Private Key:")
        color: sshTab.sshActive ? Theme.textColor : Theme.disabledTextColor
        font.pixelSize: 13
    }
    RowLayout {
        visible: sshMethodCombo.currentIndex === 0
        Layout.fillWidth: true
        spacing: 8
        RagoTextField {
            id: sshKeyField
            ltr: true
            placeholderText: "~/.ssh/id_rsa"
            Layout.fillWidth: true
            enabled: sshTab.isDirectConnection && sshEnabled.checked
        }
        RagoPillButton {
            text: "…"
            implicitWidth: 42
            enabled: sshTab.isDirectConnection && sshEnabled.checked
            onClicked: sshTab.dlg.pickFile(sshKeyField, qsTr("Select SSH Private Key"))
        }
    }

    Label {
        visible: sshMethodCombo.currentIndex === 0
        text: qsTr("Passphrase:")
        color: sshTab.sshActive ? Theme.textColor : Theme.disabledTextColor
        font.pixelSize: 13
    }
    RowLayout {
        visible: sshMethodCombo.currentIndex === 0
        Layout.fillWidth: true
        spacing: 6
        RagoTextField {
            id: sshPassphraseField
            placeholderText: qsTr("Optional key passphrase")
            Layout.fillWidth: true
            echoMode: sshTab.dlg.showSshPass ? TextInput.Normal : TextInput.Password
            enabled: sshTab.isDirectConnection && sshEnabled.checked && !sshAskPasswordCheck.checked
        }
        RagoIconButton {
            glyph: sshTab.dlg.showSshPass ? "◡" : "◉"
            size: 30
            enabled: sshTab.isDirectConnection && sshEnabled.checked && !sshAskPasswordCheck.checked
            onClicked: sshTab.dlg.showSshPass = !sshTab.dlg.showSshPass
        }
    }

    RagoCheckBox {
        id: sshAskPasswordCheck
        text: sshMethodCombo.currentIndex === 1
              ? qsTr("Prompt for SSH password every time on connect")
              : qsTr("Prompt for key passphrase every time on connect")
        enabled: sshTab.sshActive
        Layout.columnSpan: Math.min(2, sshTab.columns)
    }

    Item { Layout.fillHeight: true; Layout.columnSpan: Math.min(2, sshTab.columns) }
}
