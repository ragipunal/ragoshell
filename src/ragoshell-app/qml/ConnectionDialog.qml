pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs as QtDialogs

// Connection editor dialog. Works on a QVariantMap compatible with
// ConnectionSettings::fromVariant() — see core/settings/*.cpp for keys.
RagoDialog {
    id: dlg
    title: editRow >= 0 ? qsTr("Edit Connection") : qsTr("New Connection")
    acceptText: qsTr("Save")
    rejectText: qsTr("Cancel")
    showReject: true
    width: Math.min(visibleWidth - 24, 700)
    // Grow by the diagnostics panel so it never squeezes the tab pages.
    height: Math.min(visibleHeight - 24,
                     660 + (diagFooter.diagVisible ? diagFooter.diagPreferredHeight + 10 : 0))

    // Name given to a connection that was not named; the stored name is data, so
    // the English default is still recognised after a language switch.
    readonly property string defaultConnectionName: qsTr("New Connection")

    // Mod C/B channel list (wasm only): "hosted" appears only while the
    // account holds the hosted_bridge entitlement — the gateway is
    // authoritative either way (docs/HostedGateway.md §5).
    property var channelKinds: []
    property var channelLabels: []

    function rebuildChannels() {
        var kinds = ["memory"], labels = [qsTr("Demo (in-browser)")]
        var hosted = typeof CloudService !== "undefined" && CloudService.hostedBridgeEnabled
        if (hosted) {
            kinds.push("hosted"); labels.push(qsTr("RagoShell Cloud (Pro)"))
        }
        kinds.push("agent");  labels.push(qsTr("Local Agent (localhost)"))
        kinds.push("custom"); labels.push(qsTr("Custom Gateway (ws://…)"))
        channelKinds = kinds
        channelLabels = labels
    }

    // Appends a selectable placeholder for a stored kind the live list hides
    // (hosted while signed out) so saving never silently rewrites the kind.
    function ensureChannelVisible(chan) {
        if (channelKinds.indexOf(chan) >= 0)
            return
        channelKinds = channelKinds.concat([chan])
        channelLabels = channelLabels.concat([chan === "hosted"
            ? qsTr("RagoShell Cloud (Pro) — sign-in required") : chan])
    }

    Component.onCompleted: rebuildChannels()

    Connections {
        target: typeof CloudService !== "undefined" ? CloudService : null
        function onStateChanged() {
            var keep = channelKinds[channelCombo.currentIndex]
            rebuildChannels()
            if (keep)
                ensureChannelVisible(keep)
            var idx = channelKinds.indexOf(keep)
            channelCombo.currentIndex = idx >= 0 ? idx : 0
        }
    }
    // ConnectionSettings::schemaCollectionMode values; "" = the Preferences setting.
    readonly property var schemaModeIds: ["", "off", "onUse", "wholeDatabase"]
    function isDefaultConnectionName(name) {
        return name === dlg.defaultConnectionName || name === "New Connection"
    }

    // Authentication mechanism of the connection being edited, so that one this build cannot offer is kept.
    property string _savedMechanism: ""

    // -1 = creating new, otherwise row in ConnectionModel
    property int editRow: -1
    // editable copy of the variant map
    property var data: ({})
    property string selectedColorTag: ""
    property bool showAuthPass: false
    property bool showSshPass: false
    property bool showPemPass: false
    property var diagnosticSteps: []
    property bool showDiagnostics: false
    // The phone and tablet builds have no mongodb+srv (the driver is built without DNS seedlists there), so the
    // type is not offered; a connection that already is one keeps the entry so it can still be read and edited.
    property bool srvKept: false
    // Full error of the last failed test, shown in the details panel.
    property string testErrorText: ""
    // Whether the connection was on the MCP allow-list when the dialog opened.
    property bool mcpWasAllowed: false
    // Connections the read-only MCP server cannot serve (McpGuards::unsupportedReason).
    readonly property string mcpBlockedReason: sshTab.sshActive
        ? qsTr("AI tools cannot use a connection through an SSH tunnel.")
        : (tlsTab.asksPemPassphrase
           ? qsTr("AI tools cannot use a client certificate whose passphrase is asked when connecting.") : "")

    readonly property var colorPalette: [
        { "color": "",        "label": qsTr("Default") },
        { "color": "#22c55e", "label": qsTr("Dev (Green)") },
        { "color": "#38bdf8", "label": qsTr("QA / Info (Blue)") },
        { "color": "#f59e0b", "label": qsTr("Staging (Amber)") },
        { "color": "#ef4444", "label": qsTr("Production (Red)") },
        { "color": "#a855f7", "label": qsTr("Analytics (Violet)") }
    ]

    function validateBeforeSave() {
        if (typeCombo.currentIndex === 2) {
            var srvHost = hostField.text.trim().replace(/^mongodb\+srv:\/\//i, "")
            // Drivers require at least three labels (host.domain.tld) and no port.
            if (srvHost.split(".").length < 3 || srvHost.indexOf(":") >= 0
                    || srvHost.indexOf("/") >= 0) {
                tabBar.currentIndex = 0
                diagFooter.statusText = qsTr("✕ SRV host must be a hostname like cluster0.example.mongodb.net, without port.")
                diagFooter.statusColor = "#ff6b6b"
                return false
            }
        }
        if (typeCombo.currentIndex === 1) {
            var members = rsMembersEditor.collectCleanedMembers()
            if (members.length === 0) {
                tabBar.currentIndex = 0
                diagFooter.statusText = qsTr("✕ Replica set members cannot be empty. Please add at least one host:port.")
                diagFooter.statusColor = "#ff6b6b"
                return false
            }
            var seen = ({})
            for (var i = 0; i < members.length; ++i) {
                var key = members[i].toLowerCase()
                if (seen[key]) {
                    tabBar.currentIndex = 0
                    diagFooter.statusText = qsTr("✕ Duplicate replica set member '%1'. Two members cannot share the same host:port.")
                                                  .arg(Language.isolate(members[i]))
                    diagFooter.statusColor = "#ff6b6b"
                    return false
                }
                seen[key] = true
            }
        }
        return true
    }

    beforeAccept: validateBeforeSave

    function importFromUri() {
        var res = Backend.parseMongoUri(uriField.text)
        if (!res.ok) {
            diagFooter.statusText = "✕ " + res.error
            diagFooter.statusColor = "#ff6b6b"
            return
        }

        srvKept = !!res.isSrv
        typeCombo.currentIndex = res.isSrv ? 2 : (res.isReplicaSet ? 1 : 0)
        if (res.isSrv) {
            hostField.text = res.serverHost || ""
        } else if (res.isReplicaSet) {
            rsMembersEditor.setMembersFromArray(res.hosts || [])
            rsSetNameField.text = res.setName || ""
            rsSetNameField.placeholderText = qsTr("Optional — auto-discovered if empty")
            rsReadPrefCombo.currentIndex = res.readPreference || 0
        } else {
            hostField.text = res.serverHost || "localhost"
            portField.text = String(res.serverPort || 27017)
        }

        dbField.text = res.defaultDatabase || ""
        if (nameField.text.trim().length === 0 || dlg.isDefaultConnectionName(nameField.text.trim()))
            nameField.text = res.suggestedName || dlg.defaultConnectionName

        authTab.applyUri(res)
        tlsTab.applyUri(res)
        diagFooter.statusText = qsTr("✓ Imported connection details from MongoDB URI")
        diagFooter.statusColor = Theme.accentColor
    }

    function openNew() {
        editRow = -1
        data = {
            "connectionName": dlg.defaultConnectionName,
            "serverHost": "localhost",
            "serverPort": 27017,
            "defaultDatabase": "",
            "colorTag": "",
            "isReplicaSet": false,
            "isSrv": false,
            "replicaSet": {
                "0": "localhost:27017",
                "setNameUserEntered": "",
                "cachedSetName": "",
                "readPreference": 0
            },
            "credentials": [],
            "transportKind": Qt.platform.os === "wasm" ? "memory" : "",
            "transportToken": "",
            "ssh": { "enabled": false, "host": "", "port": 22, "userName": "",
                     "userPassword": "", "privateKeyFile": "", "publicKeyFile": "",
                     "passphrase": "", "method": "publickey", "askPassword": false },
            "ssl": { "sslEnabled": false, "caFile": "", "usePemFile": false,
                     "pemKeyFile": "", "pemPassPhrase": "", "useAdvancedOptions": false,
                     "crlFile": "", "allowInvalidHostnames": false,
                     "allowInvalidCertificates": false, "askPassphrase": false }
        }
        loadFields()
        open()
    }

    function openEdit(row) {
        editRow = row
        data = ConnectionModel.get(row)
        if (!data.ssh) data.ssh = {}
        if (!data.ssl) data.ssl = {}
        if (!data.replicaSet) data.replicaSet = {}
        if (!data.credentials) data.credentials = []
        loadFields()
        open()
    }

    function loadFields() {
        tabBar.currentIndex = 0
        diagFooter.statusText = ""
        diagnosticSteps = []
        showDiagnostics = false
        testErrorText = ""
        showAuthPass = false
        showSshPass = false
        showPemPass = false
        uriField.text = ""

        nameField.text = data.connectionName || ""
        selectedColorTag = data.colorTag || ""
        srvKept = !!data.isSrv
        typeCombo.currentIndex = data.isSrv ? 2 : (data.isReplicaSet ? 1 : 0)
        hostField.text = data.serverHost || "localhost"
        portField.text = String(data.serverPort || 27017)
        dbField.text = data.defaultDatabase || ""
        schemaModeCombo.currentIndex = Math.max(0, dlg.schemaModeIds.indexOf(data.schemaCollectionMode || ""))
        // The command line may have changed mcp.json since the last look.
        McpSettings.reload()
        mcpWasAllowed = McpSettings.isAllowed(data.uuid || "")
        mcpCheck.checked = mcpWasAllowed

        // Mod C/B channel (wasm); empty kind maps to the demo channel.
        var stored = data.transportKind || "memory"
        var chan = (stored === "" || stored === "memory") ? "memory"
                 : (stored === "hosted" || stored === "agent") ? stored : "custom"
        ensureChannelVisible(chan)
        var idx = channelKinds.indexOf(chan)
        channelCombo.currentIndex = idx >= 0 ? idx : 0
        gatewayUrlField.text = chan === "custom" ? stored : ""
        transportTokenField.text = data.transportToken || ""

        var rs = data.replicaSet || {}
        var members = []
        for (var i = 0; rs[String(i)] !== undefined; ++i) {
            if (String(rs[String(i)]).length > 0)
                members.push(String(rs[String(i)]))
        }
        if (members.length === 0)
            members.push((data.serverHost || "localhost") + ":" + (data.serverPort || 27017))
        rsMembersEditor.setMembersFromArray(members)

        rsSetNameField.text = rs.setNameUserEntered || ""
        rsSetNameField.placeholderText = (rs.cachedSetName && String(rs.cachedSetName).length > 0)
                                         ? qsTr("%1 (auto-discovered)").arg(String(rs.cachedSetName))
                                         : qsTr("Optional — auto-discovered if empty")
        var rp = Number(rs.readPreference || 0)
        rsReadPrefCombo.currentIndex = (rp >= 0 && rp <= 4) ? rp : 0

        var cred = (data.credentials && data.credentials.length > 0)
                   ? data.credentials[0] : ({})
        authTab.loadFrom(cred)
        sshTab.loadFrom(data.ssh)
        tlsTab.loadFrom(data.ssl)
    }

    function normalizeManualDbs(raw) {
        var parts = String(raw || "").split(",")
        var out = []
        for (var i = 0; i < parts.length; ++i) {
            var p = parts[i].trim()
            if (p.length > 0 && out.indexOf(p) < 0)
                out.push(p)
        }
        return out.join(",")
    }

    function collectData() {
        var isRs = (typeCombo.currentIndex === 1)
        var isSrv = (typeCombo.currentIndex === 2)
        data.connectionName = nameField.text.trim() || dlg.defaultConnectionName
        data.colorTag = selectedColorTag
        data.isReplicaSet = isRs
        data.isSrv = isSrv
        data.defaultDatabase = dbField.text.trim()
        data.schemaCollectionMode = dlg.schemaModeIds[schemaModeCombo.currentIndex] || ""

        // Mod C/B channel — wasm only; desktop edits preserve stored values.
        if (Qt.platform.os === "wasm") {
            var gw = gatewayUrlField.text.trim()
            if (gw !== "" && !gw.startsWith("ws://") && !gw.startsWith("wss://"))
                gw = "wss://" + gw
            var kind = channelKinds[channelCombo.currentIndex] || "memory"
            data.transportKind = kind === "custom" ? gw : kind
            // Hosted auth comes from the cloud session — never persist a
            // pairing token alongside it.
            data.transportToken = kind === "hosted"
                ? "" : transportTokenField.text.trim()
        }

        if (isRs) {
            var cleanMembers = rsMembersEditor.collectCleanedMembers()
            if (cleanMembers.length === 0)
                cleanMembers.push("localhost:27017")

            var prevRs = data.replicaSet || {}
            var rsMap = {
                "setNameUserEntered": rsSetNameField.text.trim(),
                "cachedSetName": rsSetNameField.text.trim().length > 0
                                 ? "" : (prevRs.cachedSetName || ""),
                "readPreference": rsReadPrefCombo.currentIndex
            }
            for (var m = 0; m < cleanMembers.length; ++m)
                rsMap[String(m)] = cleanMembers[m]
            data.replicaSet = rsMap

            var firstColon = cleanMembers[0].lastIndexOf(":")
            data.serverHost = firstColon > 0 ? cleanMembers[0].substring(0, firstColon) : cleanMembers[0]
            data.serverPort = firstColon > 0 ? (parseInt(cleanMembers[0].substring(firstColon + 1), 10) || 27017) : 27017
        } else if (isSrv) {
            // SRV hostname only: no port, hosts come from DNS.
            data.serverHost = hostField.text.trim().replace(/^mongodb\+srv:\/\//i, "")
            data.serverPort = 27017
        } else {
            data.serverHost = hostField.text.trim() || "localhost"
            data.serverPort = parseInt(portField.text, 10) || 27017
        }

        data.credentials = [authTab.toCredentialMap()]
        data.ssh = sshTab.toSshMap(isRs, (data.ssh && data.ssh.publicKeyFile) || "")
        data.ssl = tlsTab.toSslMap()
        return data
    }

    onAccepted: {
        var map = collectData()
        var uuid = map.uuid || ""
        if (editRow >= 0)
            ConnectionModel.update(editRow, map)
        else
            uuid = ConnectionModel.create(map)
        // Written only when the box changed (an untouched box must not undo a
        // revoke made meanwhile); a connection the server cannot serve (now
        // SSH, say) leaves the list. A failed write is reported by MainServiceDialogs.
        var blocked = dlg.mcpBlockedReason.length > 0
        if (McpSettings.available && (mcpCheck.checked !== dlg.mcpWasAllowed || (blocked && dlg.mcpWasAllowed)))
            McpSettings.setAllowed(uuid, mcpCheck.checked && !blocked)
    }

    QtDialogs.FileDialog {
        id: filePicker
        property var targetField: null
        fileMode: QtDialogs.FileDialog.OpenFile
        onAccepted: {
            if (targetField) {
                var imported = Backend.importCredentialFile(selectedFile)
                if (imported.length > 0)
                    targetField.text = imported
            }
        }
    }

    function pickFile(field, caption) {
        filePicker.targetField = field
        filePicker.title = caption
        filePicker.open()
    }

    function firstLine(text) {
        var lines = String(text || "").split("\n")
        for (var i = 0; i < lines.length; ++i) {
            var l = lines[i].trim()
            if (l.length > 0)
                return l
        }
        return ""
    }

    // Driver errors list one "[... calling hello on 'host']" per member on a
    // single line; put each on its own line and drop blank-line runs.
    function formatErrorText(text) {
        return String(text || "")
            .replace(/\]\s*\[/g, "]\n[")
            .replace(/\s+Reason:\s*/g, "\nReason: ")
            .replace(/\n{2,}/g, "\n")
            .trim()
    }

    Connections {
        target: Backend
        function onTestResult(success, message) {
            var summary = dlg.firstLine(message)
            if (!success) {
                for (var i = 0; i < dlg.diagnosticSteps.length; ++i) {
                    var step = dlg.diagnosticSteps[i]
                    if (step.status === "error") {
                        summary = dlg.firstLine(step.detail)
                        break
                    }
                }
            }
            diagFooter.statusText = (success ? "✓ " : "✕ ") + summary
            diagFooter.statusColor = success ? Theme.accentColor : Theme.dangerColor
            dlg.testErrorText = success ? "" : dlg.formatErrorText(message)
            if (!success && dlg.diagnosticSteps.length > 0)
                dlg.showDiagnostics = true
        }
        function onTestDiagnosticSteps(steps) {
            dlg.diagnosticSteps = steps || []
        }
    }

    // The focused field stays in view when the keyboard takes space.
    FocusReveal { flickable: pageScroll.contentItem; space: dlg.visibleHeight }

    contentItem: ColumnLayout {
        spacing: 10

        // Segmented Liquid Glass TabBar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.touch ? 34 : 32
            radius: 8
            color: Theme.surfaceColor
            border.width: 1
            border.color: Theme.glassBorder

            RowLayout {
                id: tabBar
                property int currentIndex: 0
                anchors.fill: parent
                anchors.margins: 3
                spacing: 3

                Repeater {
                    model: [qsTr("Connection"), qsTr("Authentication"), qsTr("SSH"), qsTr("TLS / SSL")]
                    Item {
                        id: segBtn
                        required property int index
                        required property string modelData
                        readonly property bool isCurrent: tabBar.currentIndex === segBtn.index

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.preferredWidth: segLabel.implicitWidth + 14
                        Layout.minimumWidth: 36

                        Rectangle {
                            anchors.fill: parent
                            radius: 6
                            color: segBtn.isCurrent
                                   ? (Theme.dark ? Qt.rgba(1, 1, 1, 0.12) : "#ffffff")
                                   : ((!Theme.touch && segMouse.containsMouse) || segMouse.pressed ? Theme.cardBackgroundHover : "transparent")
                            border.width: segBtn.isCurrent ? 1 : 0
                            border.color: Theme.glassBorderHover
                        }

                        Label {
                            id: segLabel
                            anchors.centerIn: parent
                            width: Math.max(0, parent.width - 8)
                            text: segBtn.modelData
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            font.bold: segBtn.isCurrent
                            color: segBtn.isCurrent ? Theme.textColor : Theme.disabledTextColor
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        MouseArea {
                            id: segMouse
                            anchors.fill: parent
                            hoverEnabled: !Theme.touch
                            cursorShape: Qt.PointingHandCursor
                            onClicked: tabBar.currentIndex = segBtn.index
                        }
                    }
                }
            }
        }

        // Pages scroll when the window is too short for them (e.g. with
        // the diagnostics panel open at high display scaling).
        ScrollView {
            id: pageScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 60
            contentWidth: availableWidth
            contentHeight: pages.implicitHeight
            rightPadding: pages.implicitHeight > availableHeight ? 12 : 0
            clip: true

            StackLayout {
                id: pages
                width: pageScroll.availableWidth
                currentIndex: tabBar.currentIndex

                // ---- Connection tab ------------------------------------------
                ColumnLayout {
                    spacing: 8

                    GridLayout {
                        Layout.fillWidth: true
                        columns: Window.width < 520 ? 1 : 2
                        columnSpacing: 14
                        rowSpacing: 8

                        Label { text: qsTr("Type:"); color: Theme.textColor; font.pixelSize: 13 }
                        RagoComboBox {
                            id: typeCombo
                            Layout.fillWidth: true
                            // 0 = Direct, 1 = Replica Set, 2 = DNS Seedlist (mongodb+srv)
                            model: (!Platform.isMobile || dlg.srvKept)
                                   ? [qsTr("Direct Connection"), qsTr("Replica Set"),
                                      qsTr("DNS Seedlist (mongodb+srv)")]
                                   : [qsTr("Direct Connection"), qsTr("Replica Set")]
                        }

                        Label { text: qsTr("Name:"); color: Theme.textColor; font.pixelSize: 13 }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            RagoTextField {
                                id: nameField
                                placeholderText: qsTr("e.g. Local MongoDB")
                                tintColor: dlg.selectedColorTag
                                Layout.fillWidth: true
                            }

                            // Color tag swatches
                            RowLayout {
                                spacing: 4
                                Repeater {
                                    model: dlg.colorPalette
                                    Rectangle {
                                        id: swatch
                                        required property var modelData
                                        width: 20
                                        height: 20
                                        radius: 10
                                        color: swatch.modelData.color.length > 0
                                               ? swatch.modelData.color : Theme.surfaceColor
                                        border.width: dlg.selectedColorTag === swatch.modelData.color ? 2 : 1
                                        border.color: dlg.selectedColorTag === swatch.modelData.color
                                                      ? Theme.textColor : Theme.glassBorder

                                        Label {
                                            visible: swatch.modelData.color.length === 0
                                            anchors.centerIn: parent
                                            text: "∅"
                                            font.pixelSize: 10
                                            color: Theme.disabledTextColor
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: dlg.selectedColorTag = swatch.modelData.color
                                            ToolTip.visible: containsMouse
                                            ToolTip.delay: 300
                                            ToolTip.text: swatch.modelData.label
                                        }
                                    }
                                }
                            }
                        }

                        Label {
                            visible: typeCombo.currentIndex !== 1
                            text: typeCombo.currentIndex === 2 ? qsTr("SRV Host:") : qsTr("Address:")
                            color: Theme.textColor
                            font.pixelSize: 13
                        }
                        RowLayout {
                            visible: typeCombo.currentIndex !== 1
                            Layout.fillWidth: true
                            spacing: 8

                            RagoTextField {
                                id: hostField
                                ltr: true
                                placeholderText: typeCombo.currentIndex === 2
                                                 ? "cluster0.example.mongodb.net" : "localhost"
                                Layout.fillWidth: true
                            }
                            Label {
                                visible: typeCombo.currentIndex === 0
                                text: ":"
                                color: Theme.disabledTextColor
                                font.bold: true
                            }
                            RagoTextField {
                                id: portField
                                ltr: true
                                visible: typeCombo.currentIndex === 0
                                Layout.preferredWidth: 100
                                validator: IntValidator { bottom: 1; top: 65535 }
                            }
                        }

                        Label {
                            visible: typeCombo.currentIndex === 1
                            text: qsTr("Members:")
                            color: Theme.textColor
                            font.pixelSize: 13
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 6
                        }
                        ConnectionReplicaSetMembers {
                            id: rsMembersEditor
                            visible: typeCombo.currentIndex === 1
                            Layout.fillWidth: true
                        }

                        Label {
                            visible: typeCombo.currentIndex === 1
                            text: qsTr("Set Name:")
                            color: Theme.textColor
                            font.pixelSize: 13
                        }
                        RagoTextField {
                            id: rsSetNameField
                            visible: typeCombo.currentIndex === 1
                            placeholderText: qsTr("Optional — auto-discovered if empty")
                            Layout.fillWidth: true
                        }

                        Label {
                            visible: typeCombo.currentIndex === 1
                            text: qsTr("Read Pref:")
                            color: Theme.textColor
                            font.pixelSize: 13
                        }
                        RagoComboBox {
                            id: rsReadPrefCombo
                            visible: typeCombo.currentIndex === 1
                            Layout.fillWidth: true
                            // Index = ReplicaSetSettings::ReadPreference value
                            //: Read preference options: which replica set member reads go to.
                            model: [qsTr("Primary"), qsTr("Primary Preferred"), qsTr("Secondary"),
                                    qsTr("Secondary Preferred"), qsTr("Nearest")]
                        }

                        Label { text: qsTr("Default DB:"); color: Theme.textColor; font.pixelSize: 13 }
                        RagoTextField {
                            id: dbField
                            ltr: true
                            placeholderText: qsTr("Optional default database")
                            Layout.fillWidth: true
                        }

                        Label { text: qsTr("Schema analysis:"); color: Theme.textColor; font.pixelSize: 13 }
                        RagoComboBox {
                            id: schemaModeCombo
                            Layout.fillWidth: true
                            // Index = position in dlg.schemaModeIds.
                            model: [qsTr("As in Preferences"), qsTr("Off"), qsTr("Databases in use"),
                                    qsTr("All databases")]
                        }

                        // Read-only MCP server allow-list (docs/McpSettingsUi.md); desktop builds only.
                        Label {
                            visible: McpSettings.available
                            text: qsTr("AI tools (MCP):")
                            color: Theme.textColor
                            font.pixelSize: 13
                        }
                        RagoCheckBox {
                            id: mcpCheck
                            visible: McpSettings.available
                            Layout.fillWidth: true
                            enabled: McpSettings.writable && dlg.mcpBlockedReason.length === 0
                            text: qsTr("Let AI tools read this connection (read-only)")
                        }
                        Label {
                            visible: McpSettings.available && text.length > 0
                            Layout.columnSpan: Math.min(2, parent.columns)
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            font.pixelSize: 11
                            color: McpSettings.error.length > 0 ? Theme.dangerColor
                                 : (dlg.mcpBlockedReason.length > 0 || (mcpCheck.checked && !McpSettings.enabled))
                                   ? Theme.warningColor : Theme.disabledTextColor
                            text: McpSettings.error.length > 0 ? McpSettings.error
                                : dlg.mcpBlockedReason.length > 0 ? dlg.mcpBlockedReason
                                : !mcpCheck.checked
                                  ? (dlg.mcpWasAllowed ? qsTr("AI tools lose access to this connection at their next request.") : "")
                                : !McpSettings.enabled
                                  ? qsTr("The MCP server is off. Turn it on in Preferences › AI tools (MCP).")
                                : dlg.mcpWasAllowed
                                  ? qsTr("AI tools can list, query and explain this connection, never change it.")
                                  : qsTr("AI tools can list, query and explain this connection, never change it. An AI tool that is already running sees it after its MCP server restarts.")
                        }

                        // Mod C/B channel picker — browser only (docs/LocalAgent.md §4).
                        Label {
                            visible: Qt.platform.os === "wasm"
                            text: qsTr("Channel:")
                            color: Theme.textColor; font.pixelSize: 13
                        }
                        RagoComboBox {
                            id: channelCombo
                            visible: Qt.platform.os === "wasm"
                            Layout.fillWidth: true
                            model: channelLabels
                        }

                        Label {
                            visible: Qt.platform.os === "wasm"
                                     && channelKinds[channelCombo.currentIndex] === "hosted"
                            text: qsTr("Signs in with your RagoShell account. Targets must use TLS (mongodb+srv:// or ?tls=true).")
                            color: Theme.textDimmedColor; font.pixelSize: 11
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            Layout.columnSpan: 2
                        }

                        Label {
                            visible: Qt.platform.os === "wasm"
                                     && channelKinds[channelCombo.currentIndex] === "custom"
                            text: qsTr("Gateway URL:")
                            color: Theme.textColor; font.pixelSize: 13
                        }
                        RagoTextField {
                            id: gatewayUrlField
                            visible: Qt.platform.os === "wasm"
                                     && channelKinds[channelCombo.currentIndex] === "custom"
                            ltr: true
                            placeholderText: "wss://gateway.example.com"
                            Layout.fillWidth: true
                        }

                        Label {
                            visible: Qt.platform.os === "wasm"
                                     && ["agent", "custom"].indexOf(channelKinds[channelCombo.currentIndex]) >= 0
                            text: qsTr("Pairing Token:")
                            color: Theme.textColor; font.pixelSize: 13
                        }
                        RagoTextField {
                            id: transportTokenField
                            visible: Qt.platform.os === "wasm"
                                     && ["agent", "custom"].indexOf(channelKinds[channelCombo.currentIndex]) >= 0
                            ltr: true
                            echoMode: TextInput.PasswordEchoOnEdit
                            placeholderText: qsTr("Token printed by ragoshell-agent")
                            Layout.fillWidth: true
                        }
                    }

                    Item { Layout.fillHeight: true }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: Theme.glassBorder
                    }

                    // Classic "From URI" importer
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        RagoPillButton {
                            text: qsTr("From URI")
                            onClicked: dlg.importFromUri()
                        }

                        RagoTextField {
                            id: uriField
                            ltr: true
                            Layout.fillWidth: true
                            placeholderText: qsTr("Import connection details from MongoDB URI (mongodb://...)")
                            onAccepted: dlg.importFromUri()
                        }
                    }
                }


                ConnectionAuthTab {
                    id: authTab
                    dlg: dlg
                }

                ConnectionSshTab {
                    id: sshTab
                    dlg: dlg
                    isDirectConnection: typeCombo.currentIndex === 0
                }

                ConnectionTlsTab {
                    id: tlsTab
                    dlg: dlg
                }
            }
        }

        ConnectionDiagnosticsFooter {
            id: diagFooter
            Layout.fillWidth: true
            dlg: dlg
        }
    }
}
