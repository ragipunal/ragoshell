pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Modal dialog for creating or editing a MongoDB database user and built-in/custom roles,
// adapting SCRAM mechanisms, X.509/$external users, and Database vs Cluster (@admin) roles
// to the connected MongoDB server version and target database.
RagoDialog {
    id: dlg

    property string serverUuid: ""
    property string serverLabel: ""
    property string serverColorTag: ""
    property double serverVer: 0.0
    property string serverVerText: ""
    property bool isMongos: false
    property string dbName: ""
    property bool isEditMode: false
    property string errorMessage: ""
    property bool showPassword: false
    property var selectedRoles: ({})

    readonly property bool allowExternalSwitch: !isEditMode && (dbName === "admin" || dbName === "$external")
    readonly property bool effectiveExternal: (dbName === "$external") || (allowExternalSwitch && authTypeCombo.currentIndex === 1)
    readonly property string effectiveDb: effectiveExternal
                                          ? "$external"
                                          : (dbName === "$external" ? "admin" : dbName)
    readonly property bool canCrossDbRoles: effectiveDb === "admin" || effectiveDb === "$external"
    readonly property bool supportsScram256: serverVer <= 0.0 || serverVer >= 4.0

    readonly property var databaseRoles: [
        "read", "readWrite", "dbAdmin", "dbOwner", "userAdmin", "enableSharding"
    ]

    readonly property var adminOnlyRoles: [
        "clusterMonitor", "clusterManager", "clusterAdmin", "hostManager",
        "readAnyDatabase", "readWriteAnyDatabase", "userAdminAnyDatabase",
        "dbAdminAnyDatabase", "backup", "restore", "root"
    ]

    title: isEditMode ? qsTr("Edit User") : qsTr("Add User")
    acceptText: isEditMode ? qsTr("Update User") : qsTr("Create User")
    rejectText: qsTr("Cancel")
    showReject: true
    width: Math.min(visibleWidth - 24, 580)
    height: Math.min(visibleHeight - 24, 620)

    function _loadServerMeta(uuid) {
        serverUuid = uuid || ""
        serverLabel = Backend.serverAddress(serverUuid)
        serverVer = Backend.serverVersion(serverUuid)
        serverVerText = Backend.serverVersionString(serverUuid)
        isMongos = Backend.serverIsMongos(serverUuid)
        var _rev = ConnectionModel.revision
        serverColorTag = ConnectionModel.colorTagForUuid(serverUuid)
    }

    function _mechanismsForComboIndex(idx) {
        if (!supportsScram256 || effectiveExternal)
            return []
        if (idx === 1)
            return ["SCRAM-SHA-256"]
        if (idx === 2)
            return ["SCRAM-SHA-256", "SCRAM-SHA-1"]
        if (idx === 3)
            return ["SCRAM-SHA-1"]
        return []
    }

    function openForAdd(uuid, db) {
        _loadServerMeta(uuid)
        dbName = db
        isEditMode = false
        errorMessage = ""
        showPassword = false
        authTypeCombo.currentIndex = (db === "$external") ? 1 : 0
        scramCombo.currentIndex = (serverVer >= 8.0) ? 1 : 0
        userField.text = ""
        passField.text = ""
        roleDbField.text = (db === "$external") ? "admin" : db
        customRolesField.text = ""
        selectedRoles = { "readWrite": true }
        open()
        userField.forceActiveFocus()
    }

    function openForEdit(uuid, db, meta) {
        _loadServerMeta(uuid)
        dbName = db
        isEditMode = true
        errorMessage = ""
        showPassword = false
        authTypeCombo.currentIndex = (db === "$external") ? 1 : 0
        var m = meta || ({})
        userField.text = m.name || ""
        passField.text = ""

        var mechs = m.mechanisms || []
        var has256 = mechs.indexOf("SCRAM-SHA-256") >= 0
        var has1 = mechs.indexOf("SCRAM-SHA-1") >= 0
        if (has256 && has1)
            scramCombo.currentIndex = 2
        else if (has256)
            scramCombo.currentIndex = 1
        else if (has1)
            scramCombo.currentIndex = 3
        else
            scramCombo.currentIndex = 0

        var homeDb = (db === "$external") ? "admin" : db
        var map = ({})
        var extras = []
        var dbRolePairs = []
        var existing = m.roles || []

        for (var i = 0; i < existing.length; ++i) {
            var raw = String(existing[i]).trim()
            if (raw.length === 0)
                continue
            var atPos = raw.indexOf("@")
            var rBase = atPos > 0 ? raw.substring(0, atPos) : raw
            var rTarget = atPos > 0 ? raw.substring(atPos + 1) : db

            if (adminOnlyRoles.indexOf(rBase) >= 0 && (rTarget === "admin" || (atPos < 0 && db === "admin"))) {
                map[rBase] = true
            } else if (databaseRoles.indexOf(rBase) >= 0) {
                dbRolePairs.push({ role: rBase, target: (rTarget === "$external" ? "admin" : rTarget) })
            } else {
                extras.push(raw)
            }
        }

        var chosenTarget = homeDb
        if (dbRolePairs.length > 0) {
            var hasHome = false
            for (var p = 0; p < dbRolePairs.length; ++p) {
                if (dbRolePairs[p].target === homeDb) {
                    hasHome = true
                    break
                }
            }
            chosenTarget = hasHome ? homeDb : dbRolePairs[0].target
            for (var q = 0; q < dbRolePairs.length; ++q) {
                if (dbRolePairs[q].target === chosenTarget)
                    map[dbRolePairs[q].role] = true
                else
                    extras.push(dbRolePairs[q].role + "@" + dbRolePairs[q].target)
            }
        }

        roleDbField.text = chosenTarget
        selectedRoles = map
        customRolesField.text = extras.join(", ")
        open()
        if (db === "$external")
            roleDbField.forceActiveFocus()
        else
            passField.forceActiveFocus()
    }

    function toggleRole(roleName, checked) {
        var next = ({})
        for (var k in selectedRoles) {
            if (selectedRoles[k])
                next[k] = true
        }
        if (checked)
            next[roleName] = true
        else
            delete next[roleName]
        selectedRoles = next
    }

    beforeAccept: function() {
        var uName = userField.text.trim()
        if (uName.length === 0) {
            errorMessage = qsTr("Username is required.")
            return false
        }
        if (!isEditMode && !effectiveExternal && passField.text.length === 0) {
            errorMessage = qsTr("Password is required when creating a user.")
            return false
        }

        var targetDb = effectiveDb
        var roleTargetDb = canCrossDbRoles
                           ? (roleDbField.text.trim() || (targetDb === "$external" ? "admin" : targetDb))
                           : targetDb

        var roleList = []
        for (var i = 0; i < databaseRoles.length; ++i) {
            var dr = databaseRoles[i]
            if (selectedRoles[dr])
                roleList.push(dr)
        }
        if (canCrossDbRoles) {
            for (var a = 0; a < adminOnlyRoles.length; ++a) {
                var ar = adminOnlyRoles[a]
                if (selectedRoles[ar])
                    roleList.push(ar + "@admin")
            }
        }
        var extraParts = customRolesField.text.split(",")
        for (var j = 0; j < extraParts.length; ++j) {
            var trimmed = extraParts[j].trim()
            if (trimmed.length > 0 && roleList.indexOf(trimmed) < 0)
                roleList.push(trimmed)
        }

        if (roleList.length === 0) {
            errorMessage = qsTr("Select at least one role for this user.")
            return false
        }

        errorMessage = ""
        var mechs = _mechanismsForComboIndex(scramCombo.currentIndex)
        return Backend.createOrUpdateUser(
            serverUuid, targetDb, uName,
            effectiveExternal ? "" : passField.text,
            roleTargetDb, roleList, isEditMode, mechs)
    }

    contentItem: ColumnLayout {
        spacing: 10

        // The badges wrap onto a second line on a narrow screen.
        Flow {
            Layout.fillWidth: true
            spacing: 6

            RagoBadge {
                visible: dlg.serverLabel.length > 0
                text: "⛁ " + dlg.serverLabel
                badgeColor: dlg.serverColorTag.length > 0
                            ? dlg.serverColorTag
                            : Theme.disabledTextColor
            }
            RagoBadge {
                text: "DB: " + dlg.effectiveDb
                badgeColor: "#3b9eff"
            }
            RagoBadge {
                visible: dlg.serverVerText.length > 0
                text: "MongoDB v" + dlg.serverVerText
                badgeColor: Theme.accentColor
            }
            RagoBadge {
                visible: dlg.isMongos
                text: "MONGOS"
                badgeColor: Theme.accentColor
            }
            RagoBadge {
                visible: dlg.effectiveExternal
                text: "X.509 / $external"
                badgeColor: Theme.disabledTextColor
            }
            RagoBadge {
                visible: !dlg.effectiveExternal && dlg.supportsScram256
                text: "SCRAM-SHA-256"
                badgeColor: Theme.disabledTextColor
            }
        }

        // MongoDB 8.0+ SCRAM-SHA-1 deprecation notice when legacy mechanism is selected
        Rectangle {
            visible: !dlg.effectiveExternal && dlg.serverVer >= 8.0 && scramCombo.currentIndex === 3
            Layout.fillWidth: true
            implicitHeight: scramWarnLabel.implicitHeight + 14
            radius: 8
            color: Theme.dark ? Qt.rgba(0.96, 0.62, 0.04, 0.14) : Qt.rgba(0.96, 0.62, 0.04, 0.12)
            border.width: 1
            border.color: Qt.rgba(0.96, 0.62, 0.04, 0.55)

            Label {
                id: scramWarnLabel
                anchors.fill: parent
                anchors.margins: 7
                text: qsTr("⚠ SCRAM-SHA-1 is deprecated in MongoDB 8.0+. Prefer SCRAM-SHA-256 unless legacy clients require SCRAM-SHA-1.")
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: Theme.dark ? "#fcd34d" : "#92400e"
            }
        }

        ScrollView {
            id: formScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 80
            contentWidth: availableWidth
            contentHeight: formContent.implicitHeight
            rightPadding: formContent.implicitHeight > availableHeight ? 8 : 0
            clip: true

            ColumnLayout {
                id: formContent
                width: formScroll.availableWidth
                spacing: 10

                GridLayout {
                    Layout.fillWidth: true
                    columns: Window.width < 520 ? 1 : 2
                    columnSpacing: 12
                    rowSpacing: 8

                    Label {
                        visible: dlg.allowExternalSwitch
                        text: qsTr("User Type:")
                        font.pixelSize: 12
                        color: Theme.textColor
                    }
                    RagoComboBox {
                        id: authTypeCombo
                        visible: dlg.allowExternalSwitch
                        Layout.fillWidth: true
                        model: [
                            qsTr("Password (SCRAM)"),
                            qsTr("X.509 / External ($external)")
                        ]
                        onActivated: {
                            if (currentIndex === 1 && roleDbField.text.trim() === "$external")
                                roleDbField.text = "admin"
                        }
                    }

                    Label {
                        text: dlg.effectiveExternal ? qsTr("Subject / Principal:") : qsTr("Username:")
                        font.pixelSize: 12
                        color: Theme.textColor
                    }
                    RagoTextField {
                        id: userField
                        ltr: true
                        readOnly: dlg.isEditMode
                        opacity: dlg.isEditMode ? 0.7 : 1.0
                        placeholderText: dlg.effectiveExternal
                                         ? "CN=client,OU=Engineering,O=RagoShell,C=US"
                                         : qsTr("e.g. app_service")
                        Layout.fillWidth: true
                    }

                    Label {
                        visible: !dlg.effectiveExternal
                        text: qsTr("Password:")
                        font.pixelSize: 12
                        color: Theme.textColor
                    }
                    RowLayout {
                        visible: !dlg.effectiveExternal
                        Layout.fillWidth: true
                        spacing: 6
                        RagoTextField {
                            id: passField
                            echoMode: dlg.showPassword ? TextInput.Normal : TextInput.Password
                            placeholderText: dlg.isEditMode
                                             ? qsTr("Leave empty to keep existing password")
                                             : qsTr("Enter user password")
                            Layout.fillWidth: true
                        }
                        RagoIconButton {
                            glyph: dlg.showPassword ? "◡" : "◉"
                            size: 30
                            toolTipText: dlg.showPassword ? qsTr("Hide Password") : qsTr("Show Password")
                            onClicked: dlg.showPassword = !dlg.showPassword
                        }
                    }

                    Label {
                        visible: !dlg.effectiveExternal && dlg.supportsScram256
                        text: qsTr("SCRAM Hash:")
                        font.pixelSize: 12
                        color: (!dlg.isEditMode || passField.text.length > 0)
                               ? Theme.textColor : Theme.disabledTextColor
                    }
                    RagoComboBox {
                        id: scramCombo
                        visible: !dlg.effectiveExternal && dlg.supportsScram256
                        enabled: !dlg.isEditMode || passField.text.length > 0
                        Layout.fillWidth: true
                        model: [
                            qsTr("Server Default"),
                            qsTr("SCRAM-SHA-256 (Recommended)"),
                            qsTr("SCRAM-SHA-256 + SCRAM-SHA-1"),
                            qsTr("SCRAM-SHA-1 (Legacy)")
                        ]
                    }

                    Label {
                        text: qsTr("Role Target DB:")
                        font.pixelSize: 12
                        color: dlg.canCrossDbRoles ? Theme.textColor : Theme.disabledTextColor
                    }
                    RagoTextField {
                        id: roleDbField
                        ltr: true
                        readOnly: !dlg.canCrossDbRoles
                        opacity: dlg.canCrossDbRoles ? 1.0 : 0.7
                        placeholderText: dlg.effectiveExternal ? "admin" : dlg.effectiveDb
                        Layout.fillWidth: true
                    }
                }

                // 1. Database-level Built-in Roles
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        text: qsTr("Database Roles")
                        font.pixelSize: 12
                        font.bold: true
                        color: Theme.disabledTextColor
                    }
                    Label {
                        text: qsTr("(Applied to '%1')").arg(
                                  dlg.canCrossDbRoles
                                  ? (roleDbField.text.trim() || (dlg.effectiveExternal ? "admin" : dlg.effectiveDb))
                                  : dlg.effectiveDb)
                        font.pixelSize: 11
                        color: Theme.disabledTextColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: dbRolesGrid.implicitHeight + 16
                    radius: 8
                    color: Theme.glassBackground
                    border.width: 1
                    border.color: Theme.glassBorder

                    GridLayout {
                        id: dbRolesGrid
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 8
                        columns: Window.width < 520 ? 1 : 2
                        columnSpacing: 12
                        rowSpacing: 4

                        Repeater {
                            model: dlg.databaseRoles
                            RagoCheckBox {
                                id: dbRoleCheck
                                required property string modelData
                                text: modelData
                                checked: !!dlg.selectedRoles[dbRoleCheck.modelData]
                                onToggled: dlg.toggleRole(dbRoleCheck.modelData, dbRoleCheck.checked)
                            }
                        }
                    }
                }

                // 2. Cluster & Global Built-in Roles (@admin)
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        text: qsTr("Cluster & Global Roles (@admin)")
                        font.pixelSize: 12
                        font.bold: true
                        color: Theme.disabledTextColor
                    }
                    Label {
                        text: dlg.canCrossDbRoles
                              ? qsTr("(Tip: 'clusterMonitor' grants Server Monitor access)")
                              : qsTr("(Requires creating user in 'admin' or '$external')")
                        font.pixelSize: 11
                        color: Theme.disabledTextColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: adminRolesGrid.implicitHeight + 16
                    radius: 8
                    color: Theme.glassBackground
                    border.width: 1
                    border.color: Theme.glassBorder
                    opacity: dlg.canCrossDbRoles ? 1.0 : 0.55

                    GridLayout {
                        id: adminRolesGrid
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 8
                        columns: Window.width < 520 ? 1 : 2
                        columnSpacing: 12
                        rowSpacing: 4

                        Repeater {
                            model: dlg.adminOnlyRoles
                            RagoCheckBox {
                                id: adminRoleCheck
                                required property string modelData
                                enabled: dlg.canCrossDbRoles
                                text: modelData
                                checked: !!dlg.selectedRoles[adminRoleCheck.modelData]
                                onToggled: dlg.toggleRole(adminRoleCheck.modelData, adminRoleCheck.checked)
                            }
                        }
                    }
                }

                ColumnLayout {
                    spacing: 4
                    Layout.fillWidth: true
                    Label {
                        text: dlg.canCrossDbRoles
                              ? qsTr("Additional Roles (comma-separated, e.g. customRole or read@analytics)")
                              : qsTr("Additional Custom Roles on '%1' (comma-separated)").arg(dlg.effectiveDb)
                        font.pixelSize: 11
                        color: Theme.disabledTextColor
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    RagoTextField {
                        id: customRolesField
                        ltr: true
                        placeholderText: qsTr("Optional custom roles")
                        Layout.fillWidth: true
                    }
                }
            }
        }

        Label {
            visible: dlg.errorMessage.length > 0
            text: "✕ " + dlg.errorMessage
            font.pixelSize: 12
            color: "#ff6b6b"
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }
}
