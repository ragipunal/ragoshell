pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

ColumnLayout {
    id: root
    spacing: 12

    readonly property bool singleColumn: root.width < 520
    property bool serverEditOpen: false
    property bool _editServerCustom: false
    property string _serverFormError: ""

    function syncFields() {
        deviceNameInput.text = CloudService.deviceName
    }

    function openServerEditor() {
        _editServerCustom = (CloudService.configuredServerMode === "custom")
        serverModeCombo.currentIndex = _editServerCustom ? 1 : 0
        apiUrlInput.text = CloudService.configuredApiUrl
        portalUrlInput.text = CloudService.configuredPortalUrl
        bridgeUrlInput.text = CloudService.configuredBridgeUrl
        pubkeysInput.text = CloudService.configuredEntitlementPubkeys
        _serverFormError = ""
        serverEditOpen = true
    }

    function closeEditors() {
        serverEditOpen = false
        _serverFormError = ""
        serverSwitchDialog.close()
        syncWipeDialog.close()
        syncCol.closeSetup()
    }

    function commitDeviceName() {
        var trimmed = deviceNameInput.text.trim()
        if (trimmed.length > 0 && trimmed !== CloudService.deviceName)
            CloudService.updateDeviceName(trimmed)
    }

    Component.onCompleted: syncFields()

    Connections {
        target: CloudService
        function onStateChanged() {
            if (!deviceNameInput.activeFocus && deviceNameInput.text !== CloudService.deviceName)
                deviceNameInput.text = CloudService.deviceName
        }
    }

    Connections {
        target: Language
        function onLanguageChanged() {
            if (root.serverEditOpen)
                serverModeCombo.currentIndex = root._editServerCustom ? 1 : 0
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Label {
            text: qsTr("CLOUD ACCOUNT (OPTIONAL)")
            font.bold: true
            font.pixelSize: 11
            font.letterSpacing: Language.rtl ? 0 : 1.1
            color: Theme.accentColor
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            elide: Text.ElideRight
        }

        RagoBadge {
            visible: CloudService.signedIn
            text: CloudService.statusLabel
            badgeColor: CloudService.statusId === "past_due" || CloudService.statusId === "canceled"
                        ? Theme.warningColor
                        : Theme.accentColor
        }
    }

    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: 12
        color: Theme.disabledTextColor
        text: qsTr("All local MongoDB connections, SSH/TLS tunnels, the embedded shell, schema analysis, and bring-your-own-key AI work without an account. Sign in only if you want optional encrypted cross-device sync or the hosted connection bridge.")
    }

    Rectangle {
        visible: CloudService.errorMessage.length > 0
        Layout.fillWidth: true
        implicitHeight: errLabel.implicitHeight + 16
        radius: 8
        color: Qt.rgba(Theme.warningColor.r, Theme.warningColor.g, Theme.warningColor.b, 0.12)
        border.width: 1
        border.color: Qt.rgba(Theme.warningColor.r, Theme.warningColor.g, Theme.warningColor.b, 0.40)

        Label {
            id: errLabel
            anchors.fill: parent
            anchors.margins: 8
            wrapMode: Text.WordWrap
            font.pixelSize: 12
            color: Theme.warningColor
            text: CloudService.errorMessage
        }
    }

    Label {
        visible: !CloudService.keyStoreAvailable
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        color: Theme.warningColor
        text: qsTr("OS keychain is unavailable; cloud sign-in will apply to this session only and will never write tokens to disk.")
    }

    // 0. Cloud server endpoint & enterprise policy profile (docs/EnterpriseServerProfile.md §6 Step E)
    Rectangle {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        implicitHeight: serverCardCol.implicitHeight + 24
        radius: 10
        color: Theme.glassBackground
        border.width: 1
        border.color: CloudService.policyInvalid ? Theme.warningColor : Theme.glassBorder

        ColumnLayout {
            id: serverCardCol
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10

            GridLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                columns: root.singleColumn ? 1 : 2
                columnSpacing: 8
                rowSpacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 8

                    Label {
                        text: qsTr("Cloud server")
                        font.pixelSize: 12
                        color: Theme.disabledTextColor
                        elide: Text.ElideRight
                        Layout.minimumWidth: 0
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        elide: Text.ElideMiddle
                        font.pixelSize: 13
                        font.weight: Font.Medium
                        color: CloudService.policyInvalid ? Theme.warningColor : Theme.textColor
                        text: CloudService.activeServerSummary
                    }
                }

                Flow {
                    Layout.fillWidth: root.singleColumn
                    Layout.alignment: root.singleColumn ? Qt.AlignLeft : Qt.AlignRight
                    Layout.minimumWidth: 0
                    spacing: 8

                    RagoBadge {
                        visible: CloudService.policyLocked
                        text: qsTr("Managed by organization")
                        badgeColor: CloudService.policyInvalid ? Theme.warningColor : Theme.accentColor
                    }

                    RagoPillButton {
                        compact: true
                        visible: !CloudService.policyLocked && !root.serverEditOpen
                        enabled: !CloudService.busy && !CloudService.syncBusy
                        text: qsTr("Change Server…")
                        Accessible.name: text
                        onClicked: root.openServerEditor()
                    }
                }
            }

            Label {
                visible: CloudService.envOverridden && !CloudService.policyLocked
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: Theme.disabledTextColor
                text: qsTr("Overridden for this process by environment variables.")
            }

            Label {
                visible: CloudService.policyInvalid
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: Theme.warningColor
                text: qsTr("The enterprise policy file is invalid or unreadable. Cloud features and update checks are disabled.")
            }

            ColumnLayout {
                visible: root.serverEditOpen && !CloudService.policyLocked
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 8

                GridLayout {
                    Layout.fillWidth: true
                    columns: root.singleColumn ? 1 : 2
                    columnSpacing: 16
                    rowSpacing: 8

                    Label {
                        text: qsTr("Server mode")
                        font.pixelSize: 12
                        color: Theme.textColor
                        Layout.fillWidth: root.singleColumn
                        Layout.minimumWidth: 0
                    }

                    RagoComboBox {
                        id: serverModeCombo
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        model: [qsTr("%1 Cloud (default)").arg(Brand.productName), qsTr("Custom server")]
                        Accessible.name: qsTr("Server mode")
                        onActivated: index => {
                            root._editServerCustom = (index === 1)
                            root._serverFormError = ""
                        }
                    }

                    Label {
                        visible: root._editServerCustom
                        text: qsTr("API URL")
                        font.pixelSize: 12
                        color: Theme.textColor
                        Layout.fillWidth: root.singleColumn
                        Layout.minimumWidth: 0
                    }

                    RagoTextField {
                        id: apiUrlInput
                        visible: root._editServerCustom
                        ltr: true
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        placeholderText: "https://api.example.com"
                        Accessible.name: qsTr("API URL")
                        onTextEdited: root._serverFormError = ""
                    }

                    Label {
                        visible: root._editServerCustom
                        text: qsTr("Portal URL")
                        font.pixelSize: 12
                        color: Theme.textColor
                        Layout.fillWidth: root.singleColumn
                        Layout.minimumWidth: 0
                    }

                    RagoTextField {
                        id: portalUrlInput
                        visible: root._editServerCustom
                        ltr: true
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        placeholderText: "https://app.example.com"
                        Accessible.name: qsTr("Portal URL")
                        onTextEdited: root._serverFormError = ""
                    }

                    Label {
                        visible: root._editServerCustom
                        text: qsTr("Bridge URL (optional)")
                        font.pixelSize: 12
                        color: Theme.textColor
                        Layout.fillWidth: root.singleColumn
                        Layout.minimumWidth: 0
                    }

                    RagoTextField {
                        id: bridgeUrlInput
                        visible: root._editServerCustom
                        ltr: true
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        readonly property string derivedPreview: CloudService.deriveBridgeUrlPreview(portalUrlInput.text)
                        placeholderText: derivedPreview.length > 0 ? derivedPreview : "wss://bridge.example.com/ws"
                        Accessible.name: qsTr("Bridge URL (optional)")
                        onTextEdited: root._serverFormError = ""
                    }

                    Label {
                        visible: root._editServerCustom
                        text: qsTr("ES256 public keys")
                        font.pixelSize: 12
                        color: Theme.textColor
                        Layout.fillWidth: root.singleColumn
                        Layout.minimumWidth: 0
                    }

                    RagoTextField {
                        id: pubkeysInput
                        visible: root._editServerCustom
                        ltr: true
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        placeholderText: "2026-11:MFkwEwY..."
                        Accessible.name: qsTr("ES256 public keys")
                        onTextEdited: root._serverFormError = ""
                    }
                }

                Label {
                    visible: root._editServerCustom
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.disabledTextColor
                    text: qsTr("Format: <kid>:<base64-spki>;... Requires https:// and wss:// (http:// and ws:// are allowed only on localhost).")
                }

                Label {
                    visible: root._serverFormError.length > 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.dangerColor
                    text: root._serverFormError
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: root.singleColumn ? 1 : 3
                    columnSpacing: 8
                    rowSpacing: 8

                    RagoPillButton {
                        Layout.fillWidth: true
                        stretch: true
                        primary: true
                        text: qsTr("Apply Server")
                        Accessible.name: text
                        onClicked: {
                            var modeId = root._editServerCustom ? "custom" : "default"
                            var check = CloudService.validateServerProfile(
                                modeId, apiUrlInput.text, portalUrlInput.text,
                                bridgeUrlInput.text, pubkeysInput.text)
                            if (!check.ok) {
                                root._serverFormError = check.error
                                return
                            }
                            if (check.wouldSwitchSession) {
                                serverSwitchDialog.resetRequested = false
                                serverSwitchDialog.pendingMode = modeId
                                serverSwitchDialog.open()
                                return
                            }
                            var err = CloudService.applyServerProfile(
                                modeId, apiUrlInput.text, portalUrlInput.text,
                                bridgeUrlInput.text, pubkeysInput.text)
                            if (err.length > 0) {
                                root._serverFormError = err
                            } else {
                                root._serverFormError = ""
                                root.serverEditOpen = false
                            }
                        }
                    }

                    RagoPillButton {
                        visible: CloudService.configuredServerMode === "custom" || CloudService.customServerActive
                        Layout.fillWidth: true
                        stretch: true
                        text: qsTr("Reset to Default")
                        Accessible.name: text
                        onClicked: {
                            var check = CloudService.validateServerProfile("default", "", "", "", "")
                            if (!check.ok) {
                                root._serverFormError = check.error
                                return
                            }
                            if (check.wouldSwitchSession) {
                                serverSwitchDialog.resetRequested = true
                                serverSwitchDialog.pendingMode = "default"
                                serverSwitchDialog.open()
                                return
                            }
                            var err = CloudService.resetServerProfileToDefault()
                            if (err.length > 0) {
                                root._serverFormError = err
                            } else {
                                root._serverFormError = ""
                                root.serverEditOpen = false
                            }
                        }
                    }

                    RagoPillButton {
                        Layout.fillWidth: true
                        stretch: true
                        text: qsTr("Cancel")
                        Accessible.name: text
                        onClicked: {
                            root._serverFormError = ""
                            root.serverEditOpen = false
                        }
                    }
                }
            }
        }
    }

    // 1. Signed-out state (or requesting initial device code)
    ColumnLayout {
        visible: !CloudService.signedIn && !CloudService.awaitingApproval
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 10

        GridLayout {
            Layout.fillWidth: true
            columns: root.singleColumn ? 1 : 2
            columnSpacing: 16
            rowSpacing: 8

            Label {
                text: qsTr("Device name")
                font.pixelSize: 13
                color: Theme.textColor
                Layout.fillWidth: root.singleColumn
            }

            RagoTextField {
                id: deviceNameInput
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                maximumLength: 64
                enabled: !CloudService.requestingCode && !CloudService.policyInvalid
                placeholderText: qsTr("Workstation")
                onEditingFinished: root.commitDeviceName()
            }
        }

        RagoPillButton {
            Layout.fillWidth: root.singleColumn
            stretch: root.singleColumn
            primary: true
            enabled: !CloudService.requestingCode && !AppSettings.disableHttpsFeatures && !CloudService.policyInvalid
            text: CloudService.requestingCode ? qsTr("Requesting Code…") : qsTr("Sign In with Browser…")
            Accessible.name: text
            onClicked: {
                root.commitDeviceName()
                CloudService.startSignIn(deviceNameInput.text)
            }
        }
    }

    // 2. Pending RFC 8628 browser verification state
    Rectangle {
        visible: CloudService.awaitingApproval
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        implicitHeight: pendingCol.implicitHeight + 24
        radius: 10
        color: Theme.glassBackground
        border.width: 1
        border.color: Theme.glassBorder

        ColumnLayout {
            id: pendingCol
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.textColor
                text: qsTr("Open the verification page in your browser and confirm this one-time device code:")
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: codeText.implicitHeight + 16
                radius: 8
                color: Theme.cardBackgroundHover
                border.width: 1
                border.color: Theme.accentColor

                Label {
                    id: codeText
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.margins: 8
                    horizontalAlignment: Text.AlignHCenter
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 12
                    elide: Text.ElideMiddle
                    text: CloudService.userCode
                    font.family: AppSettings.codeFontFamily
                    font.bold: true
                    font.pixelSize: 20
                    font.letterSpacing: 2.0
                    color: Theme.textColor
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: Theme.disabledTextColor
                text: qsTr("Verification URL: %1 · Expires in %2")
                          .arg(CloudService.verificationUri)
                          .arg(CloudService.formattedRemainingTime)
            }

            GridLayout {
                Layout.fillWidth: true
                columns: root.singleColumn ? 1 : 3
                columnSpacing: 8
                rowSpacing: 8

                RagoPillButton {
                    Layout.fillWidth: true
                    stretch: true
                    primary: true
                    text: qsTr("Open Browser")
                    Accessible.name: text
                    onClicked: CloudService.openVerificationBrowser()
                }

                RagoPillButton {
                    Layout.fillWidth: true
                    stretch: true
                    text: CloudService.codeCopied ? qsTr("Code Copied") : qsTr("Copy Code")
                    Accessible.name: text
                    onClicked: CloudService.copyUserCode()
                }

                RagoPillButton {
                    Layout.fillWidth: true
                    stretch: true
                    text: qsTr("Cancel")
                    Accessible.name: text
                    onClicked: CloudService.cancelSignIn()
                }
            }
        }
    }

    // 3. Signed-in state
    Rectangle {
        visible: CloudService.signedIn
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        implicitHeight: signedInCol.implicitHeight + 24
        radius: 10
        color: Theme.glassBackground
        border.width: 1
        border.color: Theme.glassBorder

        ColumnLayout {
            id: signedInCol
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10

            GridLayout {
                Layout.fillWidth: true
                columns: root.singleColumn ? 1 : 2
                columnSpacing: 16
                rowSpacing: 6

                Label {
                    text: qsTr("Account")
                    font.pixelSize: 12
                    color: Theme.disabledTextColor
                }
                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    elide: Text.ElideMiddle
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    color: Theme.textColor
                    text: CloudService.userEmail.length > 0 ? CloudService.userEmail : CloudService.userId
                }

                Label {
                    text: qsTr("Registered device")
                    font.pixelSize: 12
                    color: Theme.disabledTextColor
                }
                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    wrapMode: Text.WordWrap
                    font.pixelSize: 13
                    color: Theme.textColor
                    text: CloudService.deviceIdShort.length > 0
                          ? (CloudService.deviceName + " (" + CloudService.deviceIdShort + ")")
                          : CloudService.deviceName
                }

                Label {
                    text: qsTr("Encrypted sync")
                    font.pixelSize: 12
                    color: Theme.disabledTextColor
                }
                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    wrapMode: Text.WordWrap
                    font.pixelSize: 13
                    color: CloudService.syncActive ? Theme.accentColor : Theme.disabledTextColor
                    text: CloudService.syncActive
                          ? qsTr("On · %1").arg(CloudService.syncLastSyncText)
                          : qsTr("Off")
                }
            }

            Label {
                visible: CloudService.validitySummary.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: Theme.disabledTextColor
                text: CloudService.validitySummary
            }

            GridLayout {
                Layout.fillWidth: true
                columns: root.singleColumn ? 1 : (Platform.isMobile ? 2 : 3)
                columnSpacing: 8
                rowSpacing: 8

                RagoPillButton {
                    Layout.fillWidth: true
                    stretch: true
                    enabled: !CloudService.busy && !AppSettings.disableHttpsFeatures && !CloudService.policyInvalid
                    text: CloudService.busy ? qsTr("Refreshing…") : qsTr("Refresh Status")
                    Accessible.name: text
                    onClicked: CloudService.refreshStatus()
                }

                RagoPillButton {
                    visible: !Platform.isMobile && !CloudService.policyInvalid
                    Layout.fillWidth: true
                    stretch: true
                    text: qsTr("Manage Account…")
                    Accessible.name: text
                    onClicked: {
                        if (!Platform.isMobile)
                            CloudService.openManageAccountPortal()
                    }
                }

                RagoPillButton {
                    Layout.fillWidth: true
                    stretch: true
                    danger: true
                    text: qsTr("Sign Out")
                    Accessible.name: text
                    onClicked: CloudService.signOut()
                }
            }
        }
    }

    // 4. Encrypted sync controls (A7)
    Rectangle {
        visible: CloudService.signedIn
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        implicitHeight: syncCol.implicitHeight + 24
        radius: 10
        color: Theme.glassBackground
        border.width: 1
        border.color: Theme.glassBorder

        ColumnLayout {
            id: syncCol
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10

            property bool setupOpen: false
            property string setupError: ""
            property bool attachMode: false

            function closeSetup() {
                setupOpen = false
                setupError = ""
                attachMode = false
                syncPassInput.text = ""
                syncPassConfirm.text = ""
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    text: qsTr("ENCRYPTED SYNC")
                    font.bold: true
                    font.pixelSize: 11
                    font.letterSpacing: Language.rtl ? 0 : 1.1
                    color: Theme.accentColor
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    elide: Text.ElideRight
                }

                RagoBadge {
                    visible: CloudService.syncActive
                    text: CloudService.syncStateText
                    badgeColor: CloudService.syncLastError.length > 0
                                ? Theme.warningColor : Theme.accentColor
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: Theme.disabledTextColor
                text: qsTr("Sync data leaves this device only as client-encrypted ciphertext. "
                           + "The server never sees your passphrase, keys, or plain settings.")
            }

            Label {
                visible: CloudService.syncLastError.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: Theme.warningColor
                text: CloudService.syncLastError
            }

            // Active state: collection selection + actions
            ColumnLayout {
                visible: CloudService.syncActive
                Layout.fillWidth: true
                spacing: 6

                Repeater {
                    model: [
                        { "id": "connections",   "label": qsTr("Connections") },
                        { "id": "snippets",      "label": qsTr("Saved snippets") },
                        { "id": "preferences",   "label": qsTr("Preferences") },
                        { "id": "query_history", "label": qsTr("Query history") }
                    ]
                    delegate: CheckBox {
                        required property var modelData
                        text: modelData.label
                        checked: CloudService.syncCollections.indexOf(modelData.id) >= 0
                        onToggled: {
                            var cols = CloudService.syncCollections.slice()
                            var i = cols.indexOf(modelData.id)
                            if (checked && i < 0)
                                cols.push(modelData.id)
                            else if (!checked && i >= 0)
                                cols.splice(i, 1)
                            CloudService.syncSetCollections(
                                cols, CloudService.syncIncludePasswords)
                        }
                    }
                }

                CheckBox {
                    visible: CloudService.syncCollections.indexOf("connections") >= 0
                    text: qsTr("Include saved passwords (still end-to-end encrypted)")
                    checked: CloudService.syncIncludePasswords
                    onToggled: CloudService.syncSetCollections(
                        CloudService.syncCollections, checked)
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: root.singleColumn ? 1 : (Platform.isMobile ? 2 : 3)
                columnSpacing: 8
                rowSpacing: 8

                RagoPillButton {
                    visible: CloudService.syncActive
                    Layout.fillWidth: true
                    stretch: true
                    enabled: !CloudService.syncBusy
                    text: CloudService.syncBusy ? qsTr("Syncing…") : qsTr("Sync Now")
                    Accessible.name: text
                    onClicked: CloudService.syncNow()
                }

                RagoPillButton {
                    visible: !CloudService.syncActive && !syncCol.setupOpen
                    Layout.fillWidth: true
                    stretch: true
                    enabled: !CloudService.busy && CloudService.keyStoreAvailable
                    text: qsTr("Set Up Encrypted Sync…")
                    Accessible.name: text
                    onClicked: syncCol.setupOpen = true
                }

                RagoPillButton {
                    visible: CloudService.syncActive
                    Layout.fillWidth: true
                    stretch: true
                    danger: true
                    text: qsTr("Disable Sync")
                    Accessible.name: text
                    onClicked: CloudService.syncDisable()
                }

                RagoPillButton {
                    visible: CloudService.syncActive
                    Layout.fillWidth: true
                    stretch: true
                    danger: true
                    text: qsTr("Delete Remote Data…")
                    Accessible.name: text
                    onClicked: syncWipeDialog.open()
                }
            }

            // Setup panel: passphrase or device-key provisioning
            ColumnLayout {
                visible: syncCol.setupOpen && !CloudService.syncActive
                Layout.fillWidth: true
                spacing: 8

                CheckBox {
                    text: qsTr("This device already has a sync vault (join with existing passphrase)")
                    checked: syncCol.attachMode
                    onToggled: syncCol.attachMode = checked
                }

                TextField {
                    id: syncPassInput
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    echoMode: TextInput.Password
                    placeholderText: syncCol.attachMode
                                     ? qsTr("Existing sync passphrase")
                                     : qsTr("New sync passphrase (min 8 chars)")
                }

                TextField {
                    id: syncPassConfirm
                    visible: !syncCol.attachMode
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    echoMode: TextInput.Password
                    placeholderText: qsTr("Repeat passphrase")
                }

                Label {
                    visible: syncCol.setupError.length > 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.warningColor
                    text: {
                        var c = syncCol.setupError
                        if (c === "passphrase_too_short")
                            return qsTr("Passphrase must be at least 8 characters.")
                        if (c === "passphrase_mismatch")
                            return qsTr("Passphrase does not match the existing vault.")
                        if (c === "keystore_unavailable" || c === "keystore_write_failed")
                            return qsTr("The system keychain is unavailable.")
                        return qsTr("Setup failed: %1").arg(c)
                    }
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: root.singleColumn ? 1 : 3
                    columnSpacing: 8

                    RagoPillButton {
                        Layout.fillWidth: true
                        stretch: true
                        enabled: syncPassInput.text.length >= 8
                                 && (syncCol.attachMode
                                     || syncPassInput.text === syncPassConfirm.text)
                        text: syncCol.attachMode ? qsTr("Join Vault") : qsTr("Create Vault")
                        Accessible.name: text
                        onClicked: {
                            var err = syncCol.attachMode
                                      ? CloudService.syncAttachVault(syncPassInput.text)
                                      : CloudService.syncEnablePassphrase(syncPassInput.text)
                            if (err.length === 0)
                                syncCol.closeSetup()
                            else
                                syncCol.setupError = err
                            syncPassInput.text = ""
                            syncPassConfirm.text = ""
                        }
                    }

                    RagoPillButton {
                        visible: !syncCol.attachMode
                        Layout.fillWidth: true
                        stretch: true
                        text: qsTr("Use Device Key Instead")
                        Accessible.name: text
                        onClicked: {
                            var err = CloudService.syncEnableDeviceKey()
                            if (err.length === 0)
                                syncCol.closeSetup()
                            else
                                syncCol.setupError = err
                        }
                    }

                    RagoPillButton {
                        Layout.fillWidth: true
                        stretch: true
                        text: qsTr("Cancel")
                        Accessible.name: text
                        onClicked: syncCol.closeSetup()
                    }
                }
            }
        }
    }

    Dialog {
        id: syncWipeDialog
        modal: true
        title: qsTr("Delete Remote Sync Data")
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: parent
        onAccepted: CloudService.syncWipeRemote()
        Label {
            wrapMode: Text.WordWrap
            text: qsTr("All encrypted collections stored on the server will be deleted "
                       + "permanently. Local settings stay on this device.")
        }
    }

    Dialog {
        id: serverSwitchDialog
        property bool resetRequested: false
        property string pendingMode: "default"
        modal: true
        title: qsTr("Switch Cloud Server")
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: parent
        width: Math.min(400, root.width > 32 ? root.width - 32 : 360)
        onAccepted: {
            var err = resetRequested
                ? CloudService.resetServerProfileToDefault()
                : CloudService.applyServerProfile(
                      pendingMode, apiUrlInput.text, portalUrlInput.text,
                      bridgeUrlInput.text, pubkeysInput.text)
            if (err.length > 0) {
                root._serverFormError = err
            } else {
                root._serverFormError = ""
                root.serverEditOpen = false
            }
        }
        Label {
            width: serverSwitchDialog.availableWidth
            wrapMode: Text.WordWrap
            text: qsTr("Switching the cloud server will sign out of your current session and clear local sync state. Saved local MongoDB connections, scripts, and query history stay on this device.")
        }
    }
}
