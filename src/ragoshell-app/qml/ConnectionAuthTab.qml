pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

GridLayout {
    id: authTab
    required property var dlg

    function applyUri(res) {
        authEnabled.checked = !!res.authEnabled
        authDbField.text = res.authDatabase || "admin"
        userField.text = res.userName || ""
        passField.text = res.userPassword || ""
        authTab.dlg._savedMechanism = res.authMechanism || ""
        var mechIdx = mechCombo.model.indexOf(res.authMechanism || "DEFAULT")
        mechCombo.currentIndex = mechIdx >= 0 ? mechIdx : 0
    }

    function loadFrom(cred) {
        var c = cred || ({})
        authEnabled.checked = !!c.enabled
        userField.text = c.userName || ""
        passField.text = c.userPassword || ""
        authDbField.text = (c.databaseName && c.databaseName !== "$external")
                           ? c.databaseName : "admin"
        authTab.dlg._savedMechanism = c.mechanism || ""
        mechCombo.currentIndex = Math.max(0, mechCombo.model.indexOf(c.mechanism || "DEFAULT"))
        manualDbsCheck.checked = !!c.useManuallyVisibleDbs
        manualDbsField.text = c.manuallyVisibleDbs || ""
    }

    function toCredentialMap() {
        return {
            "enabled": authEnabled.checked,
            "userName": userField.text.trim(),
            "userPassword": passField.text,
            "databaseName": authTab.externalAuth ? "$external" : (authDbField.text.trim() || "admin"),
            "mechanism": mechCombo.currentText,
            "useManuallyVisibleDbs": manualDbsCheck.checked,
            "manuallyVisibleDbs": authTab.dlg.normalizeManualDbs(manualDbsField.text)
        }
    }
    columns: Window.width < 520 ? 1 : 2
    columnSpacing: 14
    rowSpacing: 10

    // X.509 / Kerberos / LDAP authenticate against $external.
    readonly property bool externalAuth: mechCombo.currentText === "MONGODB-X509"
                                         || mechCombo.currentText === "GSSAPI"
                                         || mechCombo.currentText === "PLAIN"
    readonly property bool x509: mechCombo.currentText === "MONGODB-X509"

    RagoCheckBox {
        id: authEnabled
        text: qsTr("Perform authentication")
        Layout.columnSpan: Math.min(2, authTab.columns)
        checked: false
    }

    Label {
        text: qsTr("Auth DB:")
        color: authEnabled.checked && !authTab.externalAuth
               ? Theme.textColor : Theme.disabledTextColor
        font.pixelSize: 13
    }
    RagoTextField {
        id: authDbField
        ltr: true
        placeholderText: authTab.externalAuth ? "$external" : "admin"
        Layout.fillWidth: true
        enabled: authEnabled.checked && !authTab.externalAuth
    }

    Label {
        text: qsTr("User name:")
        color: authEnabled.checked ? Theme.textColor : Theme.disabledTextColor
        font.pixelSize: 13
    }
    RagoTextField {
        id: userField
        ltr: true
        Layout.fillWidth: true
        enabled: authEnabled.checked
    }

    Label {
        text: qsTr("Password:")
        color: authEnabled.checked && !authTab.x509
               ? Theme.textColor : Theme.disabledTextColor
        font.pixelSize: 13
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        RagoTextField {
            id: passField
            Layout.fillWidth: true
            echoMode: authTab.dlg.showAuthPass ? TextInput.Normal : TextInput.Password
            enabled: authEnabled.checked && !authTab.x509
        }
        RagoIconButton {
            glyph: authTab.dlg.showAuthPass ? "◡" : "◉"
            size: 30
            enabled: authEnabled.checked && !authTab.x509
            toolTipText: authTab.dlg.showAuthPass ? qsTr("Hide Password") : qsTr("Show Password")
            onClicked: authTab.dlg.showAuthPass = !authTab.dlg.showAuthPass
        }
    }

    Label {
        text: qsTr("Mechanism:")
        color: authEnabled.checked ? Theme.textColor : Theme.disabledTextColor
        font.pixelSize: 13
    }
    RagoComboBox {
        id: mechCombo
        Layout.fillWidth: true
        enabled: authEnabled.checked
        model: Backend.authMechanisms(authTab.dlg._savedMechanism)
    }

    Label {
        visible: authEnabled.checked && authTab.x509
        Layout.columnSpan: Math.min(2, authTab.columns)
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: qsTr("X.509 authenticates with the client certificate: enable TLS and set a PEM key file on the TLS / SSL tab. User name (certificate subject) is optional; Auth DB is $external.")
        font.pixelSize: 12
        color: Theme.disabledTextColor
    }

    RagoCheckBox {
        id: manualDbsCheck
        text: qsTr("Manually specify visible databases (when user lacks listDatabases role)")
        enabled: authEnabled.checked
        Layout.columnSpan: Math.min(2, authTab.columns)
    }

    Label {
        visible: manualDbsCheck.checked
        text: qsTr("Visible DBs:")
        color: authEnabled.checked ? Theme.textColor : Theme.disabledTextColor
        font.pixelSize: 13
    }
    RagoTextField {
        id: manualDbsField
        ltr: true
        visible: manualDbsCheck.checked
        enabled: authEnabled.checked && manualDbsCheck.checked
        placeholderText: qsTr("Comma-separated database names (e.g. app_db, analytics)")
        Layout.fillWidth: true
    }

    Item { Layout.fillHeight: true; Layout.columnSpan: Math.min(2, authTab.columns) }
}
