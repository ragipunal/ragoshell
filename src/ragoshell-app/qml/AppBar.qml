import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The one top bar (docs/DesignSystem.md, section 6): explorer toggle, logo, the command palette's
// search field, New Connection, Monitor, theme and the "more" menu. The same on every platform; below
// 720 px the search field becomes an icon and Monitor moves into the "more" menu, so no action is
// ever hidden without a place to go.
Item {
    id: bar

    required property var app
    required property var actions

    readonly property bool compact: width < 720
    readonly property bool showName: width >= 900

    height: (Theme.touch ? 42 : 46) + SafeArea.margins.top

    // Dev/CI helpers (--open-view more-menu | theme-menu).
    function openMoreMenu() { moreButton.clicked() }
    function openThemeMenu() { themeButton.clicked() }

    Rectangle {
        anchors.fill: parent
        color: Theme.surfaceColor
        border.width: 1
        border.color: Theme.glassBorder

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: Theme.dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(1, 1, 1, 0.65)
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.topMargin: bar.SafeArea.margins.top
        anchors.leftMargin: (bar.compact ? 8 : 12) + bar.SafeArea.margins.left
        anchors.rightMargin: (bar.compact ? 8 : 12) + bar.SafeArea.margins.right
        spacing: bar.compact ? 4 : 7

        RagoIconButton {
            glyph: "☰"
            size: 26
            toolTipText: Platform.withShortcut(qsTr("Toggle Explorer Panel (%1)"), "Ctrl+B")
            onClicked: bar.app.toggleExplorer()
        }

        Image {
            source: "qrc:/qml/app_icon.png"
            Layout.preferredWidth: bar.compact ? 22 : 24
            Layout.preferredHeight: bar.compact ? 22 : 24
            fillMode: Image.PreserveAspectFit
        }

        Label {
            visible: bar.showName
            text: Brand.productName
            font.bold: true
            font.pixelSize: 14
            color: Theme.textColor
            Layout.rightMargin: 4
        }

        // The command palette opens from here (Ctrl+K from anywhere).
        Rectangle {
            id: searchField
            visible: !bar.compact
            Layout.fillWidth: true
            Layout.maximumWidth: 560
            Layout.preferredHeight: 32
            radius: 7
            color: searchArea.containsMouse || activeFocus ? Theme.surfaceColor : Theme.glassBackground
            border.width: 1
            border.color: activeFocus ? Theme.accentColor
                                      : (searchArea.containsMouse ? Theme.glassBorderHover : Theme.glassBorder)
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Search commands, connections, collections…")
            Keys.onReturnPressed: bar.app.openPalette()
            Keys.onEnterPressed: bar.app.openPalette()
            Keys.onSpacePressed: bar.app.openPalette()

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8

                Label {
                    text: "⚲"
                    font.pixelSize: 14
                    color: Theme.disabledTextColor
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Search commands, connections, collections…")
                    font.pixelSize: 13
                    elide: Text.ElideRight
                    color: Theme.disabledTextColor
                }
                Label {
                    visible: !Theme.touch && Platform.shortcutsVisible
                    text: Platform.keyLabel("Ctrl+K")
                    font.pixelSize: 11
                    color: Theme.disabledTextColor
                }
            }

            MouseArea {
                id: searchArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: bar.app.openPalette()
            }
        }

        RagoIconButton {
            visible: bar.compact
            glyph: "⚲"
            size: 26
            toolTipText: qsTr("Search commands, connections, collections…")
            onClicked: bar.app.openPalette()
        }

        Item { Layout.fillWidth: bar.compact }

        RagoPillButton {
            visible: !bar.compact
            compact: true
            text: qsTr("New Connection")
            iconText: "✦"
            primary: true
            onClicked: bar.app.openNewConnection()
        }
        RagoIconButton {
            visible: bar.compact
            glyph: "✦"
            accent: true
            size: 26
            toolTipText: qsTr("New Connection")
            onClicked: bar.app.openNewConnection()
        }

        RagoPillButton {
            //: Button: open the Server Monitor (live server statistics).
            visible: !bar.compact
            compact: true
            text: qsTr("Monitor")
            iconText: "◉"
            enabled: Backend.connectedUuids.length > 0
            checked: bar.app.currentMonitor() !== null
            onClicked: bar.actions.run("tools.monitor")
        }

        Item { Layout.fillWidth: !bar.compact }

        BusyIndicator {
            visible: running
            running: Backend.connecting || Backend.transferBusy
            Layout.preferredHeight: 18
            Layout.preferredWidth: 18
        }

        RagoIconButton {
            id: themeButton
            glyph: "◐"
            size: 26
            toolTipText: qsTr("Theme")
            onClicked: themeMenu.popup(themeButton, themeButton.width - themeMenu.width, themeButton.height + 6)

            RagoMenu {
                id: themeMenu
                // Shown translated; Theme.theme keeps the English names (System, Light, Dark).
                RagoMenuItem {
                    text: qsTr("System")
                    shortcutText: Theme.theme === Theme.availableThemes[0] ? "✓" : ""
                    onTriggered: Theme.theme = Theme.availableThemes[0]
                }
                RagoMenuItem {
                    text: qsTr("Light")
                    shortcutText: Theme.theme === Theme.availableThemes[1] ? "✓" : ""
                    onTriggered: Theme.theme = Theme.availableThemes[1]
                }
                RagoMenuItem {
                    text: qsTr("Dark")
                    shortcutText: Theme.theme === Theme.availableThemes[2] ? "✓" : ""
                    onTriggered: Theme.theme = Theme.availableThemes[2]
                }
            }
        }

        RagoIconButton {
            id: moreButton
            glyph: "⋮"
            size: 26
            toolTipText: qsTr("More")
            onClicked: {
                bar.actions.refresh()
                moreMenu.popup(moreButton, moreButton.width - moreMenu.width, moreButton.height + 6)
            }

            RagoMenu {
                id: moreMenu
                CommandMenuItem { actions: bar.actions; commandId: "tools.monitor"; visible: bar.compact }
                CommandMenuItem { actions: bar.actions; commandId: "view.log" }
                RagoMenuSeparator {}
                CommandMenuItem { actions: bar.actions; commandId: "file.preferences" }
                CommandMenuItem { actions: bar.actions; commandId: "help.shortcuts" }
                CommandMenuItem { actions: bar.actions; commandId: "help.logFolder" }
                RagoMenuSeparator {}
                CommandMenuItem { actions: bar.actions; commandId: "help.about" }
            }
        }
    }
}
