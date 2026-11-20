pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

RagoDialog {
    id: dlg
    title: qsTr("Update Required")
    closable: false
    showFooter: false
    visible: !Brand.isStoreBuild && UpdateService.mandatory
    width: Math.min(visibleWidth - 24, 480)

    contentItem: Flickable {
        id: mandFlick
        implicitHeight: mandCol.implicitHeight
        contentWidth: width
        contentHeight: mandCol.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
        }

        ColumnLayout {
            id: mandCol
            width: mandFlick.width
            spacing: dlg.cramped ? 10 : 14

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: warnCol.implicitHeight + 24
                radius: 10
                color: Qt.rgba(Theme.warningColor.r, Theme.warningColor.g, Theme.warningColor.b, 0.12)
                border.width: 1
                border.color: Qt.rgba(Theme.warningColor.r, Theme.warningColor.g, Theme.warningColor.b, 0.45)

                ColumnLayout {
                    id: warnCol
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    RagoBadge {
                        text: qsTr("MANDATORY UPDATE")
                        badgeColor: Theme.warningColor
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        color: Theme.textColor
                        text: qsTr("%1 %2 is no longer supported. Please update to version %3 to continue.").arg(Brand.productName)
                                  .arg(Backend.appVersion)
                                  .arg(UpdateService.latestVersion)
                    }
                }
            }

            ColumnLayout {
                visible: UpdateService.releaseNotes.length > 0
                Layout.fillWidth: true
                spacing: 6

                Label {
                    text: qsTr("RELEASE NOTES")
                    font.bold: true
                    font.pixelSize: 11
                    font.letterSpacing: Language.rtl ? 0 : 1.1
                    color: Theme.accentColor
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: Theme.disabledTextColor
                    text: UpdateService.releaseNotes
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                RagoPillButton {
                    Layout.fillWidth: true
                    stretch: true
                    primary: true
                    iconText: "⬇"
                    text: qsTr("Download for %1").arg(UpdateService.downloadLabel)
                    onClicked: UpdateService.openDownload()
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: (mandCol.width < 380 || Platform.isMobile) ? 1 : 2
                    columnSpacing: 10
                    rowSpacing: 8

                    RagoPillButton {
                        Layout.fillWidth: true
                        stretch: true
                        text: UpdateService.linkCopied ? qsTr("Link Copied") : qsTr("Copy Link")
                        onClicked: UpdateService.copyDownloadUrl()
                    }

                    RagoPillButton {
                        visible: !Platform.isMobile
                        Layout.fillWidth: true
                        stretch: true
                        text: qsTr("Exit")
                        onClicked: Qt.quit()
                    }
                }
            }
        }
    }
}
