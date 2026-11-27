import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

Item {
    id: welcome

    signal newConnectionRequested()
    signal viewLogsRequested()
    signal openPlaygroundShellRequested()
    signal downloadDesktopRequested()

    Component.onCompleted: UpdateService.onWelcomeShown()

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: Math.max(height, column.implicitHeight + 56)
        clip: true

        ColumnLayout {
            id: column
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: welcome.width < 460 ? 16 : 32
            anchors.rightMargin: welcome.width < 460 ? 16 : 32
            anchors.topMargin: welcome.width < 460 ? 18 : 28
            spacing: welcome.width < 460 ? 14 : 20

            // Brand banner
            Image {
                source: "qrc:/qml/welcome_splash.png"
                Layout.preferredWidth: Math.min(column.width, 680)
                Layout.preferredHeight: Layout.preferredWidth / 4
                Layout.alignment: Qt.AlignHCenter
                fillMode: Image.PreserveAspectFit
            }

            Label {
                text: qsTr("Welcome to %1 %2").arg(Brand.productName).arg(Backend.appVersion)
                font.pixelSize: 24
                font.bold: true
                color: Theme.accentColor
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                Layout.preferredWidth: 0
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignHCenter
            }

            Label {
                text: qsTr("Shell-centric MongoDB IDE powered by the official MongoDB C++ driver and an embedded JavaScript query engine.")
                wrapMode: Text.WordWrap
                font.pixelSize: 13
                color: Theme.textColor
                Layout.preferredWidth: 0
                Layout.maximumWidth: 620
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignHCenter
                horizontalAlignment: Text.AlignHCenter
            }

            // Quick actions
            // Side by side when there is room, stacked on a narrow page.
            GridLayout {
                Layout.alignment: Qt.AlignHCenter
                columns: column.width < 560 ? 1 : 2
                columnSpacing: 12
                rowSpacing: 8

                RagoPillButton {
                    visible: !Platform.isWasmPlayground
                    Layout.alignment: Qt.AlignHCenter
                    text: Platform.withShortcut(qsTr("New Connection (%1)"), (!Theme.touch && welcome.width >= 520) ? "Ctrl+N" : "")
                    iconText: "✦"
                    primary: true
                    onClicked: welcome.newConnectionRequested()
                }

                RagoPillButton {
                    visible: Platform.isWasmPlayground
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Open Playground Shell")
                    iconText: ">_"
                    primary: true
                    onClicked: welcome.openPlaygroundShellRequested()
                }

                RagoPillButton {
                    visible: Platform.isWasmPlayground && Brand.downloadUrl.length > 0
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Download Desktop App")
                    iconText: "⬇"
                    onClicked: welcome.downloadDesktopRequested()
                }

                RagoPillButton {
                    visible: !Platform.isWasmPlayground || Brand.downloadUrl.length === 0
                    Layout.alignment: Qt.AlignHCenter
                    text: Platform.withShortcut(qsTr("View Logs (%1)"), (!Theme.touch && welcome.width >= 520) ? "Ctrl+L" : "")
                    iconText: "▤"
                    onClicked: welcome.viewLogsRequested()
                }
            }

            // Unobtrusive optional update notification card
            RagoGlassCard {
                visible: UpdateService.bannerVisible
                hoverable: false
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: Math.min(column.width, 680)
                implicitHeight: updateCol.implicitHeight + 36
                radius: 16
                border.color: Theme.accentColor

                ColumnLayout {
                    id: updateCol
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 10

                    RagoBadge {
                        text: qsTr("NEW VERSION")
                        badgeColor: Theme.accentColor
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.bold: true
                        font.pixelSize: 14
                        color: Theme.textColor
                        text: qsTr("%1 %2 is available").arg(Brand.productName).arg(UpdateService.latestVersion)
                    }

                    Label {
                        visible: UpdateService.releaseNotes.length > 0
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                        color: Theme.disabledTextColor
                        text: UpdateService.releaseNotes
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: updateCol.width < 420 ? 1 : 2
                        columnSpacing: 10
                        rowSpacing: 8

                        RagoPillButton {
                            Layout.fillWidth: updateCol.width < 420
                            stretch: updateCol.width < 420
                            primary: true
                            iconText: "⬇"
                            text: qsTr("Download for %1").arg(UpdateService.downloadLabel)
                            onClicked: UpdateService.openDownload()
                        }

                        RagoPillButton {
                            Layout.fillWidth: updateCol.width < 420
                            stretch: updateCol.width < 420
                            text: qsTr("Later")
                            onClicked: UpdateService.dismissOptionalUpdate()
                        }
                    }
                }
            }

            // Getting started glass card (static informational surface)
            RagoGlassCard {
                hoverable: false
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: Math.min(column.width, 680)
                implicitHeight: cardCol.implicitHeight + 44
                radius: 16

                ColumnLayout {
                    id: cardCol
                    anchors.fill: parent
                    anchors.margins: 22
                    spacing: 16

                    Label {
                        text: qsTr("GETTING STARTED")
                        font.bold: true
                        font.pixelSize: 11
                        font.letterSpacing: Language.rtl ? 0 : 1.2
                        color: Theme.accentColor
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Label {
                            text: Platform.isWasmPlayground
                                  ? qsTr("✦ In-Browser MongoDB Playground")
                                  : qsTr("✦ Connect to MongoDB")
                            font.bold: true
                            font.pixelSize: 13
                            color: Theme.textColor
                        }
                        Label {
                            text: Platform.isWasmPlayground
                                  ? qsTr("Explore the sample 'catalog' database directly in browser memory — no server setup required. Use 'Reset Demo' in the top bar anytime, or download the desktop app to connect to your own MongoDB servers.")
                                  : (Platform.shortcutsVisible
                                     ? Platform.macKeys(qsTr("Click 'New Connection' in the top bar or press Ctrl+N to configure Direct Connections, Replica Sets, SSH Tunnels, and TLS/SSL certificates."))
                                     : qsTr("Use 'New Connection' in the top bar to configure Direct Connections, Replica Sets, SSH Tunnels, and TLS/SSL certificates."))
                            wrapMode: Text.WordWrap
                            Layout.preferredWidth: 0
                            Layout.fillWidth: true
                            font.pixelSize: 12
                            color: Theme.disabledTextColor
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Label {
                            text: qsTr("✦ Interactive Query Shell")
                            font.bold: true
                            font.pixelSize: 13
                            color: Theme.textColor
                        }
                        Label {
                            text: Platform.shortcutsVisible
                                  ? Platform.macKeys(qsTr("Double-click any collection in the left Explorer or press Ctrl+T to open a shell. Execute with F5 (or Ctrl+R), stop with F6, autocomplete with Ctrl+Space, and switch between Tree (F2), Table (F3), and Text (F4) views."))
                                  : qsTr("Open any collection in the Explorer to start a shell. Run it with the Run button and switch between Tree, Table, and Text views.")
                            wrapMode: Text.WordWrap
                            Layout.preferredWidth: 0
                            Layout.fillWidth: true
                            font.pixelSize: 12
                            color: Theme.disabledTextColor
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Label {
                            text: Platform.shortcutsVisible ? qsTr("✦ Keyboard-First Workspace") : qsTr("✦ Your Workspace")
                            font.bold: true
                            font.pixelSize: 13
                            color: Theme.textColor
                        }
                        Label {
                            text: Platform.shortcutsVisible
                                  ? Platform.macKeys(qsTr("Toggle the Explorer sidebar with Ctrl+E, split orientation with F10, diagnostic logs with Ctrl+L, and switch Dark/Light/System themes live from the header."))
                                  : qsTr("Open the Explorer sidebar from the top bar, switch Dark/Light/System themes live from the header, and read the diagnostic logs with View Logs.")
                            wrapMode: Text.WordWrap
                            Layout.preferredWidth: 0
                            Layout.fillWidth: true
                            font.pixelSize: 12
                            color: Theme.disabledTextColor
                        }
                    }
                }
            }

            // Unobtrusive voluntary open-source support footer (desktop only; hidden on iOS/Android
            // per Apple App Store 3.1.1 / 3.1.3 and Google Play Billing rules — see DONATE.md).
            ColumnLayout {
                visible: !Platform.isMobile && Brand.donateUrl.indexOf("https://") === 0
                Layout.alignment: Qt.AlignHCenter
                Layout.fillWidth: true
                Layout.maximumWidth: 620
                spacing: 8

                Label {
                    //: Quiet footer note on the Welcome page (%1 = product name).
                    text: qsTr("%1 is free and open-source software (GPLv3).").arg(Brand.productName)
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: 12
                    color: Theme.disabledTextColor
                    Layout.preferredWidth: 0
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                }

                RagoPillButton {
                    compact: true
                    Layout.alignment: Qt.AlignHCenter
                    iconText: "♥"
                    //: Quiet footer button on the Welcome page: opens the voluntary support/donation page (%1 = product name).
                    text: qsTr("Support %1").arg(Brand.productName)
                    onClicked: {
                        if (!Platform.isMobile && Brand.donateUrl.indexOf("https://") === 0
                                && Brand.isSafeHttpsUrl(Brand.donateUrl))
                            Qt.openUrlExternally(Brand.donateUrl)
                    }
                }
            }
        }

        ScrollBar.vertical: ScrollBar {}
    }
}
