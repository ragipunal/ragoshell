pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

GridLayout {
    id: tlsTab
    required property var dlg
    // A client certificate whose passphrase is asked at connect time: a
    // headless client (the MCP server) cannot answer the prompt.
    readonly property bool asksPemPassphrase: tlsEnabled.checked && pemField.text.trim().length > 0
                                              && pemAskPassphraseCheck.checked

    function applyUri(res) {
        tlsEnabled.checked = !!res.sslEnabled
        caField.text = res.caFile || ""
        pemField.text = res.pemKeyFile || ""
        pemPassField.text = res.pemPassPhrase || ""
        invalidHostnames.checked = !!res.allowInvalidHostnames
        invalidCerts.checked = !!res.allowInvalidCertificates
    }

    function loadFrom(ssl) {
        var s = ssl || ({})
        tlsEnabled.checked = !!s.sslEnabled
        caField.text = s.caFile || ""
        pemField.text = s.pemKeyFile || ""
        pemPassField.text = s.pemPassPhrase || ""
        pemAskPassphraseCheck.checked = !!s.askPassphrase
        crlEnabledCheck.checked = !!s.useAdvancedOptions
        crlField.text = s.crlFile || ""
        invalidHostnames.checked = !!s.allowInvalidHostnames
        invalidCerts.checked = !!s.allowInvalidCertificates
    }

    function toSslMap() {
        return {
            "sslEnabled": tlsEnabled.checked,
            "caFile": caField.text.trim(),
            "usePemFile": pemField.text.trim().length > 0,
            "pemKeyFile": pemField.text.trim(),
            "pemPassPhrase": pemAskPassphraseCheck.checked ? "" : pemPassField.text,
            "useAdvancedOptions": crlEnabledCheck.checked && crlField.text.trim().length > 0,
            "crlFile": crlField.text.trim(),
            "allowInvalidHostnames": invalidHostnames.checked,
            "allowInvalidCertificates": invalidCerts.checked,
            "askPassphrase": pemAskPassphraseCheck.checked
        }
    }
    columns: Window.width < 520 ? 1 : 2
    columnSpacing: 14
    rowSpacing: 8

    RagoCheckBox {
        id: tlsEnabled
        text: qsTr("Use TLS / SSL protocol")
        Layout.columnSpan: Math.min(2, tlsTab.columns)
    }

    Label {
        text: qsTr("CA File:")
        color: tlsEnabled.checked ? Theme.textColor : Theme.disabledTextColor
        font.pixelSize: 13
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        RagoTextField {
            id: caField
            ltr: true
            placeholderText: qsTr("Path to Root CA (.pem / .crt)")
            Layout.fillWidth: true
            enabled: tlsEnabled.checked
        }
        RagoPillButton {
            text: "…"
            implicitWidth: 42
            enabled: tlsEnabled.checked
            onClicked: tlsTab.dlg.pickFile(caField, qsTr("Select CA Certificate"))
        }
    }

    Label {
        text: qsTr("Client PEM:")
        color: tlsEnabled.checked ? Theme.textColor : Theme.disabledTextColor
        font.pixelSize: 13
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        RagoTextField {
            id: pemField
            ltr: true
            placeholderText: qsTr("Optional client certificate/key (.pem)")
            Layout.fillWidth: true
            enabled: tlsEnabled.checked
        }
        RagoPillButton {
            text: "…"
            implicitWidth: 42
            enabled: tlsEnabled.checked
            onClicked: tlsTab.dlg.pickFile(pemField, qsTr("Select Client PEM Key"))
        }
    }

    Label {
        text: qsTr("PEM Passphrase:")
        color: tlsEnabled.checked ? Theme.textColor : Theme.disabledTextColor
        font.pixelSize: 13
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        RagoTextField {
            id: pemPassField
            placeholderText: qsTr("Optional PEM passphrase")
            Layout.fillWidth: true
            echoMode: tlsTab.dlg.showPemPass ? TextInput.Normal : TextInput.Password
            enabled: tlsEnabled.checked && !pemAskPassphraseCheck.checked
        }
        RagoIconButton {
            glyph: tlsTab.dlg.showPemPass ? "◡" : "◉"
            size: 30
            enabled: tlsEnabled.checked && !pemAskPassphraseCheck.checked
            onClicked: tlsTab.dlg.showPemPass = !tlsTab.dlg.showPemPass
        }
    }

    RagoCheckBox {
        id: pemAskPassphraseCheck
        text: qsTr("Prompt for PEM passphrase every time on connect")
        enabled: tlsEnabled.checked
        Layout.columnSpan: Math.min(2, tlsTab.columns)
    }

    RagoCheckBox {
        id: crlEnabledCheck
        text: qsTr("Use Certificate Revocation List (CRL)")
        enabled: tlsEnabled.checked
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        RagoTextField {
            id: crlField
            ltr: true
            placeholderText: qsTr("Optional CRL file (.pem / .crl)")
            Layout.fillWidth: true
            enabled: tlsEnabled.checked && crlEnabledCheck.checked
        }
        RagoPillButton {
            text: "…"
            implicitWidth: 42
            enabled: tlsEnabled.checked && crlEnabledCheck.checked
            onClicked: tlsTab.dlg.pickFile(crlField, qsTr("Select CRL File"))
        }
    }

    RagoCheckBox {
        id: invalidHostnames
        text: qsTr("Allow invalid hostnames")
        enabled: tlsEnabled.checked
        Layout.columnSpan: Math.min(2, tlsTab.columns)
    }
    RagoCheckBox {
        id: invalidCerts
        text: qsTr("Allow self-signed / invalid certificates")
        enabled: tlsEnabled.checked
        Layout.columnSpan: Math.min(2, tlsTab.columns)
    }

    Item { Layout.fillHeight: true; Layout.columnSpan: Math.min(2, tlsTab.columns) }
}
