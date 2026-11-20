pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

ColumnLayout {
    id: workArea
    required property var app
    required property var actions
    spacing: 0

    function syncCurrentTab() {
        workTabs.currentIndex = workArea.app.tabs.currentIndex
    }

    function shellPageAt(idx) {
        return shellRepeater.itemAt(idx)
    }

    // Segmented Color-Coded Tab Bar
    Rectangle {
        visible: workArea.app.tabs.count > 0
        Layout.fillWidth: true
        Layout.preferredHeight: 35
        color: Theme.surfaceColor
        border.width: 1
        border.color: Theme.glassBorder

        // Full-width active tab color ribbon along the bottom edge,
        // visually connecting the active tab directly to the workspace below
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 2
            color: workArea.app.activeTabColor()
            Behavior on color { ColorAnimation { duration: 140 } }
        }

        TabBar {
            id: workTabs
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            anchors.topMargin: 4
            anchors.bottomMargin: 2
            spacing: 4
            clip: true
            background: null
            // Clicks select through the model; the model drives back
            // (new tab, closed tab, Welcome reopened).
            onCurrentIndexChanged: {
                if (currentIndex >= 0 && currentIndex < workArea.app.tabs.count)
                    workArea.app.tabs.currentIndex = currentIndex
                workTabs.revealCurrent()
            }
            // The strip is a ListView whose "keep the current tab in range" follow-up jumps to the wrong end
            // in right-to-left languages (only the current tab stayed in view). The current tab is scrolled
            // into view by hand instead, the same in both directions.
            function revealCurrent() {
                var view = workTabs.contentItem
                if (view && view.positionViewAtIndex && currentIndex >= 0)
                    view.positionViewAtIndex(currentIndex, ListView.Contain)
            }
            Component.onCompleted: {
                if (contentItem && contentItem.highlightRangeMode !== undefined)
                    contentItem.highlightRangeMode = ListView.NoHighlightRange
            }

            Connections {
                target: workArea.app.tabs
                function onCurrentChanged() {
                    if (workTabs.currentIndex !== workArea.app.tabs.currentIndex)
                        workTabs.currentIndex = workArea.app.tabs.currentIndex
                }
            }

            Repeater {
                model: workArea.app.tabs
                TabButton {
                    id: tabBtn
                    required property int index
                    required property string kind
                    required property QtObject tabObject
                    required property string tabColor
                    required property string title
                    text: resolvedTitle
                    implicitHeight: 28
                    leftPadding: 9
                    rightPadding: 6
                    implicitWidth: Math.min(200, Math.max(62, tabContentRow.implicitWidth + leftPadding + rightPadding))
                    width: implicitWidth
                    hoverEnabled: !Theme.touch

                    readonly property bool isCurrent: workArea.app.tabs.currentIndex === tabBtn.index
                    readonly property ShellController tabShell:
                        kind === "shell" ? tabObject as ShellController : null
                    readonly property ServerMonitorController tabMonitor:
                        kind === "monitor" ? tabObject as ServerMonitorController : null
                    readonly property string resolvedTitle:
                        tabShell ? tabShell.title
                                 : (tabMonitor ? tabMonitor.title : tabBtn.title)

                    readonly property color basePaletteColor:
                        Qt.color(tabBtn.tabColor.length > 0 ? tabBtn.tabColor : "#1d4ed8")
                    readonly property real fillAlpha:
                        isCurrent ? 1.0 : (tabBtn.hovered ? 0.94 : 0.84)
                    readonly property color effectiveBgColor:
                        workArea.app.blendColors(Theme.surfaceColor, basePaletteColor, fillAlpha)
                    readonly property color fgColor:
                        workArea.app.invertedTextColorFor(effectiveBgColor)

                    ToolTip.visible: tabBtn.hovered && !tabCloseMouse.containsMouse && tabBtn.resolvedTitle.length > 0
                    ToolTip.delay: 450
                    ToolTip.text: tabBtn.resolvedTitle

                    contentItem: RowLayout {
                        id: tabContentRow
                        spacing: 6

                        // Active / busy / dirty tab indicator
                        Label {
                            id: tabStateGlyph
                            readonly property bool isTabBusy: tabBtn.tabShell !== null && tabBtn.tabShell.busy
                            visible: isTabBusy || tabBtn.isCurrent || (tabBtn.tabShell !== null && tabBtn.tabShell.dirty)
                            text: isTabBusy
                                  ? "⟳"
                                  : ((tabBtn.tabShell !== null && tabBtn.tabShell.dirty) ? "●" : (Language.rtl ? "◂" : "▸"))
                            font.pixelSize: isTabBusy
                                            ? 12
                                            : ((tabBtn.tabShell !== null && tabBtn.tabShell.dirty) ? 9 : 11)
                            font.bold: true
                            color: tabBtn.fgColor
                            Layout.alignment: Qt.AlignVCenter
                            onIsTabBusyChanged: if (!isTabBusy) rotation = 0
                            onVisibleChanged: if (!visible) rotation = 0

                            RotationAnimator on rotation {
                                running: tabStateGlyph.visible && tabStateGlyph.isTabBusy
                                from: 0
                                to: 360
                                duration: 900
                                loops: Animation.Infinite
                            }
                        }

                        Label {
                            visible: tabBtn.tabMonitor !== null
                            text: "◉"
                            font.pixelSize: 10
                            color: tabBtn.tabMonitor && tabBtn.tabMonitor.errorText.length > 0
                                   ? "#fde047"
                                   : tabBtn.fgColor
                            Layout.alignment: Qt.AlignVCenter
                        }

                        Label {
                            text: tabBtn.resolvedTitle
                            textFormat: Text.PlainText
                            color: tabBtn.fgColor
                            font.pixelSize: 12
                            font.weight: tabBtn.isCurrent ? Font.Bold : Font.DemiBold
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                            horizontalAlignment: Text.AlignLeft
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.maximumWidth: 168
                        }

                        Rectangle {
                            visible: !Platform.isMobile || tabBtn.isCurrent
                            enabled: visible
                            Layout.preferredWidth: visible ? (Platform.isMobile ? 18 : 17) : 0
                            Layout.preferredHeight: visible ? (Platform.isMobile ? 18 : 17) : 0
                            radius: (Platform.isMobile ? 9 : 8.5)
                            color: tabCloseMouse.containsMouse
                                   ? Qt.rgba(tabBtn.fgColor.r, tabBtn.fgColor.g, tabBtn.fgColor.b, 0.24)
                                   : "transparent"

                            Label {
                                anchors.centerIn: parent
                                text: "✕"
                                color: tabBtn.fgColor
                                opacity: tabCloseMouse.containsMouse ? 1.0 : 0.82
                                font.bold: true
                                font.pixelSize: 9
                            }

                            MouseArea {
                                id: tabCloseMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: workArea.app.closeTabAt(tabBtn.index)
                            }
                        }
                    }

                    property bool _longPressTriggered: false

                    onClicked: {
                        if (tabBtn._longPressTriggered) {
                            tabBtn._longPressTriggered = false
                            return
                        }
                        workArea.app.tabs.currentIndex = tabBtn.index
                    }

                    TapHandler {
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.Stylus
                        acceptedButtons: Qt.MiddleButton
                        onTapped: workArea.app.closeTabAt(tabBtn.index)
                    }

                    TapHandler {
                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad | PointerDevice.Stylus
                        acceptedButtons: Qt.RightButton
                        onTapped: (point) => {
                            workArea.app._ctxTabIndex = tabBtn.index
                            tabContextMenu.popup(tabBtn, Math.max(0, point.position.x), tabBtn.height)
                        }
                    }

                    TapHandler {
                        id: tabLongPress
                        acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
                        acceptedButtons: Qt.LeftButton
                        longPressThreshold: 0.8
                        onPressedChanged: {
                            if (pressed)
                                tabBtn._longPressTriggered = false
                        }
                        onLongPressed: {
                            tabBtn._longPressTriggered = true
                            workArea.app._ctxTabIndex = tabBtn.index
                            tabContextMenu.popup(tabBtn, Math.max(0, tabLongPress.point.position.x), tabBtn.height)
                        }
                    }

                    background: Item {
                        // Inactive tabs sit slightly inset as pills; the active tab
                        // connects flush down into the 3px bottom ribbon of the same color.
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: tabBtn.isCurrent ? 0 : 2
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: tabBtn.isCurrent ? -1 : 1
                            topLeftRadius: 6
                            topRightRadius: 6
                            bottomLeftRadius: tabBtn.isCurrent ? 0 : 6
                            bottomRightRadius: tabBtn.isCurrent ? 0 : 6
                            color: tabBtn.effectiveBgColor
                            opacity: tabBtn.isCurrent ? 1.0 : (tabBtn.hovered ? 0.96 : 0.85)

                            border.width: tabBtn.isCurrent ? 1 : 1
                            border.color: tabBtn.isCurrent
                                          ? (Theme.dark
                                             ? Qt.lighter(tabBtn.basePaletteColor, 1.35)
                                             : Qt.darker(tabBtn.basePaletteColor, 1.18))
                                          : Qt.rgba(0, 0, 0, Theme.dark ? 0.35 : 0.12)

                            Behavior on color { ColorAnimation { duration: 120 } }
                        }
                    }
                }
            }
        }
    }

    Item {
        Layout.fillWidth: true
        Layout.fillHeight: true

        WelcomePage {
            anchors.fill: parent
            visible: workArea.app.tabs.currentKind === "welcome"
            onNewConnectionRequested: workArea.app.openNewConnection()
            onViewLogsRequested: workArea.app.showLog()
        }

        Repeater {
            id: shellRepeater
            model: Backend.shellModel
            ShellTabPage {
                required property int index
                required property var itemObject
                anchors.fill: parent
                visible: itemObject === workArea.app.tabs.currentObject
                shell: itemObject
                actions: workArea.actions
            }
        }

        Repeater {
            id: monitorRepeater
            model: Backend.monitorModel
            ServerMonitorPage {
                required property int index
                required property var itemObject
                anchors.fill: parent
                visible: itemObject === workArea.app.tabs.currentObject
                monitor: itemObject
            }
        }

        // Clean empty workspace view when all tabs (including Welcome) are closed
        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width - 32, 480)
            spacing: 14
            visible: workArea.app.tabs.count === 0

            Image {
                source: "qrc:/qml/app_icon.png"
                Layout.preferredWidth: 48
                Layout.preferredHeight: 48
                Layout.alignment: Qt.AlignHCenter
                fillMode: Image.PreserveAspectFit
                opacity: 0.75
            }

            Label {
                text: qsTr("No Open Tabs")
                font.pixelSize: 16
                font.bold: true
                color: Theme.textColor
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                Layout.fillWidth: true
            }

            Label {
                text: qsTr("Double-click any collection in Explorer to query, or open a new connection.")
                font.pixelSize: 12
                color: Theme.disabledTextColor
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 10

                RagoPillButton {
                    text: qsTr("New Connection")
                    iconText: "✦"
                    primary: true
                    onClicked: workArea.app.openNewConnection()
                }

                RagoPillButton {
                    text: qsTr("Welcome Guide")
                    iconText: "ⓘ"
                    onClicked: workArea.app.reopenWelcome()
                }
            }
        }
    }

    RagoMenu {
        id: tabContextMenu
        width: 215

        RagoMenuItem {
            text: qsTr("Duplicate Tab")
            enabled: workArea.app.tabs.kindAt(workArea.app._ctxTabIndex) === "shell"
            onTriggered: {
                var sIdx = Backend.shellModel.indexOf(workArea.app.tabs.objectAt(workArea.app._ctxTabIndex))
                if (sIdx < 0)
                    return
                var p = shellRepeater.itemAt(sIdx) as ShellTabPage
                Backend.duplicateShell(sIdx, p ? p.scriptText() : "")
            }
        }
        RagoMenuSeparator {}
        RagoMenuItem {
            text: qsTr("Close Tab")
            onTriggered: workArea.app.closeTabAt(workArea.app._ctxTabIndex)
        }
        RagoMenuItem {
            text: qsTr("Close Other Tabs")
            enabled: workArea.app.tabs.count > 1
            onTriggered: workArea.app.closeOtherTabsAt(workArea.app._ctxTabIndex)
        }
        RagoMenuItem {
            text: qsTr("Close Tabs to the Right")
            enabled: workArea.app._ctxTabIndex >= 0 && workArea.app._ctxTabIndex < workArea.app.tabs.count - 1
            onTriggered: workArea.app.closeTabsToRightAt(workArea.app._ctxTabIndex)
        }
    }
}
