pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

// Centralized MongoDB Users & Roles manager dialog.
// Keeps authentication administration out of per-database Explorer folders while
// supporting 'admin', '$external' (X.509 / LDAP / Kerberos), and per-database users.
RagoDialog {
    id: dlg
    required property var pane

    property string serverUuid: ""
    property string serverLabel: ""
    property string serverColorTag: ""
    property string serverVerText: ""
    property var dbNames: ["admin", "$external"]
    property string currentDb: "admin"
    property var users: []
    property bool loading: false
    property string errorMessage: ""
    readonly property bool compactCards: dlg.width < 520

    title: qsTr("Users & Roles")
    acceptText: qsTr("Close")
    showReject: false
    width: Math.min(visibleWidth - 24, 640)
    height: Math.min(visibleHeight - 24, 560)

    function _syncDbCombo() {
        var idx = dlg.dbNames.indexOf(dlg.currentDb)
        if (idx >= 0 && dbCombo.currentIndex !== idx)
            dbCombo.currentIndex = idx
    }

    function selectDatabase(db) {
        var target = (db && db.length > 0) ? db : "admin"
        if (dlg.dbNames.indexOf(target) < 0) {
            var next = dlg.dbNames.slice()
            next.push(target)
            dlg.dbNames = next
        }
        dlg.currentDb = target
        dlg._syncDbCombo()
        dlg.errorMessage = ""
        dlg.users = Backend.cachedDatabaseUsers(dlg.serverUuid, target)
        dlg.loading = dlg.serverUuid.length > 0
        if (dlg.serverUuid.length > 0)
            Backend.requestDatabaseUsers(dlg.serverUuid, target)
    }

    function openForDatabase(uuid, initialDb) {
        dlg.serverUuid = uuid || ""
        dlg.serverLabel = Backend.serverAddress(dlg.serverUuid)
        dlg.serverVerText = Backend.serverVersionString(dlg.serverUuid)
        var _rev = ConnectionModel.revision
        dlg.serverColorTag = ConnectionModel.colorTagForUuid(dlg.serverUuid)

        var list = Backend.serverDatabaseNames(dlg.serverUuid)
        if (!list || list.length === 0)
            list = ["admin", "$external"]
        var target = (initialDb && initialDb.length > 0) ? initialDb : "admin"
        if (list.indexOf(target) < 0)
            list.push(target)
        dlg.dbNames = list

        dlg.selectDatabase(target)
        dlg.open()
    }

    Connections {
        target: Backend

        function onDatabaseUsersLoading(uuid, dbName) {
            if (dlg.opened && uuid === dlg.serverUuid && dbName === dlg.currentDb)
                dlg.loading = true
        }

        function onDatabaseUsersLoaded(uuid, dbName, userList, err) {
            if (!dlg.opened || uuid !== dlg.serverUuid)
                return
            if (dbName !== dlg.currentDb) {
                // If the user just created an $external user while viewing 'admin', switch to it.
                if (err.length === 0 && dbName === "$external") {
                    dlg.currentDb = dbName
                    dlg._syncDbCombo()
                    dlg.loading = false
                    dlg.errorMessage = ""
                    dlg.users = userList
                }
                return
            }
            dlg.loading = false
            dlg.errorMessage = err || ""
            if (dlg.errorMessage.length === 0)
                dlg.users = userList || []
        }
    }

    contentItem: ScrollView {
        id: pageScrollView
        clip: true
        contentWidth: availableWidth
        rightPadding: bodyCol.implicitHeight > availableHeight ? 8 : 0

        ColumnLayout {
            id: bodyCol
            width: pageScrollView.availableWidth

        // Server & MongoDB auth architecture context badges
        Flow {
            Layout.fillWidth: true
            spacing: 6

            RagoBadge {
                visible: dlg.serverLabel.length > 0
                text: "⛁ " + dlg.serverLabel
                badgeColor: dlg.serverColorTag.length > 0
                            ? Qt.color(dlg.serverColorTag)
                            : Theme.disabledTextColor
            }
            RagoBadge {
                visible: dlg.serverVerText.length > 0
                text: "MongoDB v" + dlg.serverVerText
                badgeColor: Theme.accentColor
            }
            RagoBadge {
                text: qsTr("Auth DB: %1").arg(dlg.currentDb)
                badgeColor: "#38bdf8"
            }
        }

        // Database selector + Refresh + Shell + Add User toolbar
        Flow {
            Layout.fillWidth: true
            spacing: 8

            RowLayout {
                spacing: 8
                width: Math.min(parent.width, 280)

                Label {
                    text: qsTr("Auth Source:")
                    font.pixelSize: 12
                    color: Theme.disabledTextColor
                }

                RagoComboBox {
                    id: dbCombo
                    Layout.fillWidth: true
                    model: dlg.dbNames
                    onActivated: function(idx) {
                        if (idx >= 0 && idx < dlg.dbNames.length)
                            dlg.selectDatabase(String(dlg.dbNames[idx]))
                    }
                }
            }

            RagoPillButton {
                text: qsTr("Refresh")
                iconText: "↻"
                enabled: !dlg.loading && dlg.serverUuid.length > 0
                onClicked: dlg.selectDatabase(dlg.currentDb)
            }

            RagoPillButton {
                text: qsTr("db.getUsers()")
                enabled: dlg.serverUuid.length > 0 && dlg.currentDb !== "$external"
                onClicked: {
                    Backend.openShellWithScript(dlg.serverUuid, dlg.currentDb, "db.getUsers()", true)
                    dlg.close()
                }
            }

            RagoPillButton {
                text: qsTr("Add User…")
                iconText: "+"
                primary: true
                enabled: dlg.serverUuid.length > 0
                onClicked: dlg.pane.openAddUser(dlg.serverUuid, dlg.currentDb)
            }
        }

        // Error / permission banner (e.g. non-admin user lacking viewUser privilege)
        Rectangle {
            visible: dlg.errorMessage.length > 0
            Layout.fillWidth: true
            implicitHeight: errBannerLabel.implicitHeight + 16
            radius: 8
            color: Theme.dark ? Qt.rgba(0.94, 0.27, 0.27, 0.14) : Qt.rgba(0.94, 0.27, 0.27, 0.08)
            border.width: 1
            border.color: Qt.rgba(0.94, 0.27, 0.27, 0.45)

            Label {
                id: errBannerLabel
                anchors.fill: parent
                anchors.margins: 8
                //: %1 is the database name, %2 is the server error message.
                text: qsTr("Cannot list users on '%1': %2").arg(dlg.currentDb).arg(dlg.errorMessage)
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.dark ? "#fca5a5" : "#b91c1c"
            }
        }

        // Main users list container
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 160
            radius: 10
            color: Theme.glassBackground
            border.width: 1
            border.color: Theme.glassBorder
            clip: true

            ListView {
                id: usersList
                anchors.fill: parent
                anchors.margins: 8
                spacing: 6
                clip: true
                visible: dlg.users.length > 0
                model: dlg.users

                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Rectangle {
                    id: userCard
                    required property var modelData
                    readonly property string userName: String(modelData.name || "")
                    readonly property var userRoles: modelData.roles || []
                    readonly property var userMechs: modelData.mechanisms || []

                    width: ListView.view ? ListView.view.width - (usersList.ScrollBar.vertical.visible ? 10 : 0) : 200
                    implicitHeight: cardCol.implicitHeight + 16
                    radius: 8
                    color: Theme.surfaceColor
                    border.width: 1
                    border.color: Theme.glassBorder

                    function viewUserInShell() {
                        Backend.openShellWithScript(
                            dlg.serverUuid, dlg.currentDb,
                            "db.getUser(\"" + dlg.pane.escapeDoubleQuotes(userCard.userName) + "\")",
                            true)
                        dlg.close()
                    }

                    function confirmDropUser() {
                        dlg.pane._ctxUuid = dlg.serverUuid
                        dlg.pane._ctxDb = dlg.currentDb
                        dlg.pane._ctxRawName = userCard.userName
                        dlg.pane.openDestructiveConfirm({
                            action: "dropUser",
                            uuid: dlg.serverUuid,
                            ns: dlg.currentDb,
                            title: qsTr("Drop Database User"),
                            acceptText: qsTr("Drop User"),
                            msg: qsTr("Permanently drop user '%1' from database '%2'?")
                                 .arg(Language.isolate(userCard.userName))
                                 .arg(Language.isolate(dlg.currentDb)),
                            consequence: qsTr("Services or clients authenticating as '%1' on '%2' will immediately lose database access. This action cannot be undone.")
                                         .arg(Language.isolate(userCard.userName))
                                         .arg(Language.isolate(dlg.currentDb))
                        })
                    }

                    ColumnLayout {
                        id: cardCol
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 6

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6

                            Label {
                                text: "◉"
                                font.pixelSize: 12
                                color: "#38bdf8"
                            }

                            Label {
                                text: userCard.userName
                                font.bold: true
                                font.pixelSize: 13
                                color: Theme.textColor
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                                Layout.minimumWidth: 40
                            }

                            RagoBadge {
                                visible: userCard.userMechs.length > 0
                                text: userCard.userMechs.join(" · ")
                                badgeColor: Theme.disabledTextColor
                            }

                            RagoIconButton {
                                visible: !dlg.compactCards
                                glyph: "⎘"
                                size: 26
                                toolTipText: qsTr("Copy Username")
                                onClicked: Backend.copyToClipboard(userCard.userName)
                            }

                            RagoIconButton {
                                visible: !dlg.compactCards && dlg.currentDb !== "$external"
                                glyph: "❯_"
                                size: 26
                                toolTipText: qsTr("View User in Shell")
                                onClicked: userCard.viewUserInShell()
                            }

                            RagoPillButton {
                                visible: !dlg.compactCards
                                text: qsTr("Edit…")
                                onClicked: dlg.pane.openEditUser(dlg.serverUuid, dlg.currentDb, userCard.modelData)
                            }

                            RagoPillButton {
                                visible: !dlg.compactCards
                                //: Permanently deletes the user from MongoDB (dropUser).
                                text: qsTr("Drop…")
                                danger: true
                                onClicked: userCard.confirmDropUser()
                            }
                        }

                        Flow {
                            visible: userCard.userRoles.length > 0
                            Layout.fillWidth: true
                            spacing: 5

                            Repeater {
                                model: userCard.userRoles
                                RagoBadge {
                                    required property string modelData
                                    text: modelData
                                    badgeColor: Theme.accentColor
                                }
                            }
                        }

                        RowLayout {
                            visible: dlg.compactCards
                            Layout.fillWidth: true
                            spacing: 6

                            RagoIconButton {
                                glyph: "⎘"
                                size: 30
                                toolTipText: qsTr("Copy Username")
                                onClicked: Backend.copyToClipboard(userCard.userName)
                            }

                            RagoIconButton {
                                visible: dlg.currentDb !== "$external"
                                glyph: "❯_"
                                size: 30
                                toolTipText: qsTr("View User in Shell")
                                onClicked: userCard.viewUserInShell()
                            }

                            Item { Layout.fillWidth: true }

                            RagoPillButton {
                                text: qsTr("Edit…")
                                onClicked: dlg.pane.openEditUser(dlg.serverUuid, dlg.currentDb, userCard.modelData)
                            }

                            RagoPillButton {
                                //: Permanently deletes the user from MongoDB (dropUser).
                                text: qsTr("Drop…")
                                danger: true
                                onClicked: userCard.confirmDropUser()
                            }
                        }
                    }
                }
            }

            // Loading / Empty state
            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width - 32, 440)
                spacing: 10
                visible: dlg.users.length === 0

                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    font.pixelSize: 13
                    font.bold: true
                    color: Theme.textColor
                    text: dlg.loading
                          ? qsTr("Loading users from '%1'…").arg(dlg.currentDb)
                          : qsTr("No users defined in '%1'.").arg(dlg.currentDb)
                }

                Label {
                    visible: !dlg.loading && dlg.errorMessage.length === 0
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: Theme.disabledTextColor
                    text: dlg.currentDb !== "admin"
                          ? qsTr("MongoDB typically stores SCRAM credentials centrally in 'admin' (and X.509 / LDAP principals in '$external') with roles scoped to application databases.")
                          : qsTr("Click 'Add User…' above to create a SCRAM or X.509 user and assign database or cluster roles.")
                }

                RagoPillButton {
                    visible: !dlg.loading && dlg.currentDb !== "admin"
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Switch to 'admin'")
                    onClicked: dlg.selectDatabase("admin")
                }
            }
        }
    }
    }
}
