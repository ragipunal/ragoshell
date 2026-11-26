pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

// Cloud account & end-to-end encrypted sync controls (docs/CloudSyncUx.md).
// Hosted inline in PreferencesDialog (standalone: false) and inside CloudSyncDialog (standalone: true).
ColumnLayout {
    id: root
    spacing: 12

    property bool standalone: false
    readonly property bool singleColumn: root.width < 460
    property bool serverEditOpen: false
    property bool advancedOpen: false
    property bool _editServerCustom: false
    property string _serverFormError: ""

    // Only surface the server card at the top when a custom server, enterprise policy,
    // environment override, or active server edit applies. Default users reach it via
    // "Change Server…" (signed out) or "Advanced" (signed in).
    readonly property bool showServerCard: root.serverEditOpen
                                           || CloudService.customServerActive
                                           || CloudService.policyLocked
                                           || CloudService.policyInvalid
                                           || CloudService.envOverridden

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

    function openSyncSetup() {
        if (CloudService.signedIn && !CloudService.syncActive && CloudService.keyStoreAvailable)
            syncCol.openSetup(false)
    }

    function closeEditors() {
        serverEditOpen = false
        advancedOpen = false
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
        visible: !root.standalone
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
            badgeColor: CloudService.statusId === "past_due"
                        || CloudService.statusId === "canceled"
                        || CloudService.offlineGraceActive
                        ? Theme.warningColor
                        : Theme.accentColor
        }
    }

    // Reassurance prose is needed only before signing in; once signed in it becomes noise.
    Label {
        visible: !CloudService.signedIn && !CloudService.awaitingApproval
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

    // 0. Cloud server endpoint & enterprise policy profile (shown when non-default or being edited)
    Rectangle {
        visible: root.showServerCard
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

                Flow {
                    Layout.fillWidth: true
                    spacing: 8

                    RagoPillButton {
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

    // 1. Signed-out state
    Rectangle {
        visible: !CloudService.signedIn && !CloudService.awaitingApproval
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        implicitHeight: signedOutCol.implicitHeight + 24
        radius: 10
        color: Theme.glassBackground
        border.width: 1
        border.color: Theme.glassBorder

        ColumnLayout {
            id: signedOutCol
            anchors.fill: parent
            anchors.margins: 12
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

            Flow {
                Layout.fillWidth: true
                spacing: 8

                RagoPillButton {
                    primary: true
                    enabled: !CloudService.requestingCode && !AppSettings.disableHttpsFeatures && !CloudService.policyInvalid
                    text: CloudService.requestingCode ? qsTr("Requesting Code…") : qsTr("Sign In with Browser…")
                    Accessible.name: text
                    onClicked: {
                        root.commitDeviceName()
                        CloudService.startSignIn(deviceNameInput.text)
                    }
                }

                RagoPillButton {
                    visible: !CloudService.policyLocked && !root.showServerCard
                    enabled: !CloudService.busy && !CloudService.requestingCode
                    text: qsTr("Change Server…")
                    Accessible.name: text
                    onClicked: root.openServerEditor()
                }
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

            Flow {
                Layout.fillWidth: true
                spacing: 8

                RagoPillButton {
                    primary: true
                    text: qsTr("Open Browser")
                    Accessible.name: text
                    onClicked: CloudService.openVerificationBrowser()
                }

                RagoPillButton {
                    text: CloudService.codeCopied ? qsTr("Code Copied") : qsTr("Copy Code")
                    Accessible.name: text
                    onClicked: CloudService.copyUserCode()
                }

                RagoPillButton {
                    text: qsTr("Cancel")
                    Accessible.name: text
                    onClicked: CloudService.cancelSignIn()
                }
            }
        }
    }

    // 3. Signed-in account summary (compact, calm, no red buttons)
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
            spacing: 8

            GridLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                columns: root.singleColumn ? 1 : 2
                columnSpacing: 12
                rowSpacing: 8

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 3

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            elide: Text.ElideMiddle
                            font.pixelSize: 13
                            font.weight: Font.Medium
                            color: Theme.textColor
                            text: CloudService.userEmail.length > 0 ? CloudService.userEmail : CloudService.userId
                        }

                        RagoBadge {
                            visible: root.standalone
                            text: CloudService.statusLabel
                            badgeColor: CloudService.statusId === "past_due"
                                        || CloudService.statusId === "canceled"
                                        || CloudService.offlineGraceActive
                                        ? Theme.warningColor
                                        : Theme.accentColor
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        font.pixelSize: 12
                        color: Theme.disabledTextColor
                        text: qsTr("Device: %1").arg(CloudService.deviceName)
                    }

                    Label {
                        visible: CloudService.offlineGraceActive && CloudService.validitySummary.length > 0
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                        color: Theme.warningColor
                        text: CloudService.validitySummary
                    }
                }

                Flow {
                    Layout.fillWidth: root.singleColumn
                    Layout.alignment: root.singleColumn ? Qt.AlignLeft : Qt.AlignRight
                    Layout.minimumWidth: 0
                    spacing: 6

                    RagoPillButton {
                        compact: true
                        visible: !Platform.isMobile && !CloudService.policyInvalid
                        text: qsTr("Manage Account…")
                        Accessible.name: text
                        onClicked: {
                            if (!Platform.isMobile)
                                CloudService.openManageAccountPortal()
                        }
                    }

                    RagoPillButton {
                        compact: true
                        text: qsTr("Sign Out")
                        Accessible.name: text
                        onClicked: CloudService.signOut()
                    }
                }
            }
        }
    }

    // 4. Encrypted sync controls (focused primary workflow + collapsible Advanced panel)
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
            property bool showPass: false

            function openSetup(attach) {
                setupOpen = true
                setupError = ""
                attachMode = attach
                showPass = false
                syncPassInput.text = ""
                syncPassConfirm.text = ""
            }

            function setAttachMode(attach) {
                if (attachMode === attach)
                    return
                attachMode = attach
                setupError = ""
                syncPassInput.text = ""
                syncPassConfirm.text = ""
            }

            function closeSetup() {
                setupOpen = false
                setupError = ""
                attachMode = false
                showPass = false
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
                    text: CloudService.syncActive ? CloudService.syncStateText : qsTr("Off")
                    badgeColor: !CloudService.syncActive
                                ? Theme.disabledTextColor
                                : (CloudService.syncLastError.length > 0
                                   ? Theme.warningColor
                                   : Theme.accentColor)
                }
            }

            Label {
                visible: CloudService.syncActive
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.disabledTextColor
                text: qsTr("Last synced: %1").arg(CloudService.syncLastSyncText)
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: Theme.disabledTextColor
                text: qsTr("Sync data leaves this device only as client-encrypted ciphertext. The server never sees your passphrase, keys, or plain settings.")
            }

            Rectangle {
                visible: CloudService.syncLastError.length > 0
                Layout.fillWidth: true
                implicitHeight: syncErrLabel.implicitHeight + 14
                radius: 8
                color: Qt.rgba(Theme.warningColor.r, Theme.warningColor.g, Theme.warningColor.b, 0.12)
                border.width: 1
                border.color: Qt.rgba(Theme.warningColor.r, Theme.warningColor.g, Theme.warningColor.b, 0.40)

                Label {
                    id: syncErrLabel
                    anchors.fill: parent
                    anchors.margins: 7
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.warningColor
                    text: CloudService.syncLastError
                }
            }

            // Active state: compact 2-column RagoCheckBox selection
            ColumnLayout {
                visible: CloudService.syncActive
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 6

                GridLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    columns: root.singleColumn ? 1 : 2
                    columnSpacing: 16
                    rowSpacing: 4

                    Repeater {
                        model: [
                            { "id": "connections",   "label": qsTr("Connections") },
                            { "id": "snippets",      "label": qsTr("Saved snippets") },
                            { "id": "preferences",   "label": qsTr("Preferences") },
                            { "id": "query_history", "label": qsTr("Query history") }
                        ]
                        delegate: RagoCheckBox {
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
                }

                RagoCheckBox {
                    visible: CloudService.syncCollections.indexOf("connections") >= 0
                    text: qsTr("Include saved passwords (still end-to-end encrypted)")
                    checked: CloudService.syncIncludePasswords
                    onToggled: CloudService.syncSetCollections(
                        CloudService.syncCollections, checked)
                }
            }

            // Primary action bar (when setup panel is not open)
            Flow {
                visible: CloudService.syncActive || !syncCol.setupOpen
                Layout.fillWidth: true
                spacing: 8

                RagoPillButton {
                    visible: CloudService.syncActive
                    primary: true
                    iconText: CloudService.syncBusy ? "↻" : "⇅"
                    enabled: !CloudService.syncBusy && !CloudService.policyInvalid
                    text: CloudService.syncBusy ? qsTr("Syncing…") : qsTr("Sync Now")
                    Accessible.name: text
                    onClicked: CloudService.syncNow()
                }

                RagoPillButton {
                    visible: !CloudService.syncActive && !syncCol.setupOpen
                    primary: true
                    iconText: "⇅"
                    enabled: !CloudService.busy && CloudService.keyStoreAvailable && !CloudService.policyInvalid
                    text: qsTr("Set Up Encrypted Sync…")
                    Accessible.name: text
                    onClicked: syncCol.openSetup(false)
                }

                RagoPillButton {
                    checked: root.advancedOpen
                    text: qsTr("Advanced")
                    Accessible.name: text
                    onClicked: root.advancedOpen = !root.advancedOpen
                }
            }

            // Setup panel: explicit two-mode selector (New passphrase vs Existing passphrase)
            ColumnLayout {
                visible: syncCol.setupOpen && !CloudService.syncActive
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 8

                GridLayout {
                    Layout.fillWidth: true
                    columns: root.singleColumn ? 1 : 2
                    columnSpacing: 8
                    rowSpacing: 6

                    RagoPillButton {
                        Layout.fillWidth: true
                        stretch: true
                        checked: !syncCol.attachMode
                        text: qsTr("New Sync Passphrase")
                        Accessible.name: text
                        onClicked: syncCol.setAttachMode(false)
                    }

                    RagoPillButton {
                        Layout.fillWidth: true
                        stretch: true
                        checked: syncCol.attachMode
                        text: qsTr("Use Existing Passphrase")
                        Accessible.name: text
                        onClicked: syncCol.setAttachMode(true)
                    }
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: Theme.disabledTextColor
                    text: syncCol.attachMode
                          ? qsTr("Enter the sync passphrase you already created on another device.")
                          : qsTr("Choose a passphrase (at least 8 characters) to encrypt your sync data. Enter this same passphrase on your other devices.")
                }

                RagoTextField {
                    id: syncPassInput
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    echoMode: syncCol.showPass ? TextInput.Normal : TextInput.Password
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    placeholderText: syncCol.attachMode
                                     ? qsTr("Existing sync passphrase")
                                     : qsTr("New sync passphrase (min 8 chars)")
                    Accessible.name: placeholderText
                    onTextEdited: syncCol.setupError = ""
                }

                RagoTextField {
                    id: syncPassConfirm
                    visible: !syncCol.attachMode
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    echoMode: syncCol.showPass ? TextInput.Normal : TextInput.Password
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    placeholderText: qsTr("Repeat passphrase")
                    Accessible.name: placeholderText
                    onTextEdited: syncCol.setupError = ""
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 6

                    RagoPillButton {
                        compact: true
                        text: syncCol.showPass ? qsTr("Hide Passphrase") : qsTr("Show Passphrase")
                        Accessible.name: text
                        onClicked: syncCol.showPass = !syncCol.showPass
                    }

                    RagoPillButton {
                        compact: true
                        visible: !syncCol.attachMode
                        text: qsTr("Use Device Key Instead")
                        Accessible.name: text
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Encrypts backups with a key stored only on this device. Other devices will not be able to read them.")
                        onClicked: {
                            var err = CloudService.syncEnableDeviceKey()
                            if (err.length === 0)
                                syncCol.closeSetup()
                            else
                                syncCol.setupError = err
                        }
                    }
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

                Flow {
                    Layout.fillWidth: true
                    spacing: 8

                    RagoPillButton {
                        primary: true
                        enabled: syncPassInput.text.length >= 8
                                 && (syncCol.attachMode
                                     || syncPassInput.text === syncPassConfirm.text)
                        text: syncCol.attachMode ? qsTr("Connect & Sync") : qsTr("Start Encrypted Sync")
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
                        text: qsTr("Cancel")
                        Accessible.name: text
                        onClicked: syncCol.closeSetup()
                    }
                }
            }

            // Collapsible Advanced section: keeps administrative & destructive actions off the main canvas
            ColumnLayout {
                visible: root.advancedOpen
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 8

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: Theme.glassBorder
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 8

                    RagoPillButton {
                        compact: true
                        enabled: !CloudService.busy && !AppSettings.disableHttpsFeatures && !CloudService.policyInvalid
                        text: CloudService.busy ? qsTr("Refreshing…") : qsTr("Refresh Status")
                        Accessible.name: text
                        onClicked: CloudService.refreshStatus()
                    }

                    RagoPillButton {
                        compact: true
                        visible: !CloudService.policyLocked && !root.serverEditOpen
                        enabled: !CloudService.busy && !CloudService.syncBusy
                        text: qsTr("Change Server…")
                        Accessible.name: text
                        onClicked: root.openServerEditor()
                    }

                    RagoPillButton {
                        compact: true
                        visible: CloudService.syncActive
                        text: qsTr("Disable Sync")
                        Accessible.name: text
                        onClicked: {
                            CloudService.syncDisable()
                            root.advancedOpen = false
                        }
                    }

                    RagoPillButton {
                        compact: true
                        visible: CloudService.syncActive
                        danger: true
                        text: qsTr("Delete Remote Data…")
                        Accessible.name: text
                        onClicked: syncWipeDialog.open()
                    }
                }
            }
        }
    }

    RagoDialog {
        id: syncWipeDialog
        title: qsTr("Delete Remote Sync Data")
        acceptText: qsTr("Delete Remote Data…")
        rejectText: qsTr("Cancel")
        showReject: true
        dangerAccept: true
        width: Math.min(visibleWidth - 24, 420)
        onAccepted: CloudService.syncWipeRemote()

        contentItem: Label {
            width: syncWipeDialog.availableWidth
            wrapMode: Text.WordWrap
            font.pixelSize: 13
            color: Theme.textColor
            text: qsTr("All encrypted collections stored on the server will be deleted permanently. Local settings stay on this device.")
        }
    }

    RagoDialog {
        id: serverSwitchDialog
        property bool resetRequested: false
        property string pendingMode: "default"
        title: qsTr("Switch Cloud Server")
        acceptText: qsTr("Apply Server")
        rejectText: qsTr("Cancel")
        showReject: true
        width: Math.min(visibleWidth - 24, 420)
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

        contentItem: Label {
            width: serverSwitchDialog.availableWidth
            wrapMode: Text.WordWrap
            font.pixelSize: 13
            color: Theme.textColor
            text: qsTr("Switching the cloud server will sign out of your current session and clear local sync state. Saved local MongoDB connections, scripts, and query history stay on this device.")
        }
    }
}
