import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

RagoDialog {
    id: dlg
    title: qsTr("About %1").arg(Brand.productName)
    acceptText: qsTr("Close")
    showReject: false
    width: Math.min(visibleWidth - 24, 440)

    signal openLicensesRequested()

    contentItem: Flickable {
        id: aboutFlick
        implicitHeight: aboutCol.implicitHeight
        contentWidth: width
        contentHeight: aboutCol.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
        }

        ColumnLayout {
            id: aboutCol
            width: aboutFlick.width
            spacing: dlg.cramped ? 8 : 12

            Item {
                visible: !dlg.cramped
                Layout.preferredHeight: 2
            }

            Image {
                visible: !dlg.cramped
                source: "qrc:/qml/app_icon.png"
                Layout.preferredWidth: 64
                Layout.preferredHeight: 64
                Layout.alignment: Qt.AlignHCenter
                fillMode: Image.PreserveAspectFit
            }

            ColumnLayout {
                spacing: 6
                Layout.alignment: Qt.AlignHCenter

                Label {
                    text: Brand.productName + " " + Backend.appVersion
                    font.bold: true
                    font.pixelSize: 18
                    color: Theme.textColor
                    Layout.alignment: Qt.AlignHCenter
                }

                RagoBadge {
                    text: "MongoDB C++ Driver · Qt Quick / QML"
                    badgeColor: Theme.accentColor
                    Layout.alignment: Qt.AlignHCenter
                }
            }

            Label {
                text: qsTr("A modern, shell-centric MongoDB GUI with native BSON inspection, SSH tunneling, TLS/SSL support, and an embedded JavaScript query shell.")
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 13
                color: Theme.textColor
                Layout.fillWidth: true
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.glassBorder
            }

            Label {
                text: qsTr("Copyright © 2014–2026 Robomongo and %1 contributors.").arg(Brand.productName)
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 11
                color: Theme.disabledTextColor
                Layout.fillWidth: true
            }

            Label {
                text: qsTr("Licensed under GNU General Public License v3.0 (GPLv3). Built with Qt %1 under GNU LGPLv3 / GPLv3.").arg(Backend.qtVersion)
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 11
                color: Theme.disabledTextColor
                Layout.fillWidth: true
            }

            Label {
                text: qsTr("MongoDB is a registered trademark of MongoDB, Inc. %1 is not affiliated with or endorsed by MongoDB, Inc.").arg(Brand.productName)
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 11
                color: Theme.disabledTextColor
                Layout.fillWidth: true
            }

            Label {
                visible: !Brand.isStoreBuild && UpdateService.statusMessage.length > 0
                text: UpdateService.statusMessage
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 12
                color: UpdateService.statusIsError
                       ? Theme.warningColor
                       : (UpdateService.updateAvailable ? Theme.accentColor : Theme.disabledTextColor)
                Layout.fillWidth: true
            }

            GridLayout {
                id: actionGrid
                Layout.fillWidth: true
                columns: aboutCol.width < 380 ? 1 : 2
                columnSpacing: 10
                rowSpacing: 8

                RagoPillButton {
                    visible: !Brand.isStoreBuild
                    Layout.fillWidth: true
                    stretch: true
                    enabled: !UpdateService.checking
                    primary: UpdateService.updateAvailable
                    text: UpdateService.updateAvailable
                          ? qsTr("Download for %1").arg(UpdateService.downloadLabel)
                          : (UpdateService.checking ? qsTr("Checking…") : qsTr("Check for Updates"))
                    Accessible.name: text
                    onClicked: {
                        if (UpdateService.updateAvailable)
                            UpdateService.openDownload()
                        else
                            UpdateService.checkNow()
                    }
                }

                RagoPillButton {
                    Layout.fillWidth: true
                    stretch: true
                    Layout.columnSpan: Brand.isStoreBuild ? Math.min(2, actionGrid.columns) : 1
                    text: qsTr("Open-Source Licenses…")
                    Accessible.name: text
                    onClicked: {
                        dlg.close()
                        dlg.openLicensesRequested()
                    }
                }

                RagoPillButton {
                    visible: Brand.productUrl.length > 0
                    Layout.fillWidth: true
                    stretch: true
                    text: qsTr("Website")
                    Accessible.name: text
                    onClicked: {
                        if (Brand.isSafeHttpsUrl(Brand.productUrl))
                            Qt.openUrlExternally(Brand.productUrl)
                    }
                }

                RagoPillButton {
                    visible: Brand.privacyUrl.length > 0
                    Layout.fillWidth: true
                    stretch: true
                    text: qsTr("Privacy Policy")
                    Accessible.name: text
                    onClicked: {
                        if (Brand.isSafeHttpsUrl(Brand.privacyUrl))
                            Qt.openUrlExternally(Brand.privacyUrl)
                    }
                }

                RagoPillButton {
                    visible: Brand.sourceRepoUrl.length > 0
                    Layout.fillWidth: true
                    stretch: true
                    Layout.columnSpan: (Platform.isMobile || Brand.donateUrl.indexOf("https://") !== 0)
                                       ? Math.min(2, actionGrid.columns)
                                       : 1
                    text: qsTr("Source Code")
                    Accessible.name: text
                    onClicked: {
                        if (Brand.isSafeHttpsUrl(Brand.sourceRepoUrl))
                            Qt.openUrlExternally(Brand.sourceRepoUrl)
                    }
                }

                // Mobile store guidelines (Apple App Store 3.1.1 / 3.1.3 & Google Play Billing)
                // prohibit external donation links inside mobile builds; show on desktop only
                // (DONATE.md).
                RagoPillButton {
                    visible: !Platform.isMobile && Brand.donateUrl.indexOf("https://") === 0
                    Layout.fillWidth: true
                    stretch: true
                    iconText: "♥"
                    //: Button in the About dialog: opens the voluntary open-source support/donation page (%1 = product name).
                    text: qsTr("Support %1").arg(Brand.productName)
                    Accessible.name: text
                    onClicked: {
                        if (!Platform.isMobile && Brand.donateUrl.indexOf("https://") === 0
                                && Brand.isSafeHttpsUrl(Brand.donateUrl))
                            Qt.openUrlExternally(Brand.donateUrl)
                    }
                }
            }
        }
    }
}
