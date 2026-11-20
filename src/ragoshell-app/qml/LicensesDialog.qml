pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Displays the app's GPLv3 license, Qt's LGPLv3 license, and all third-party
// open-source copyright and attribution notices on desktop, tablets, and phones.
RagoDialog {
    id: dlg

    title: qsTr("Open-Source Licenses")
    acceptText: qsTr("Close")
    showReject: false
    width: Math.min(visibleWidth - 24, 860)
    height: Math.min(visibleHeight - 24, 680)

    property string currentDoc: "notices"
    readonly property string documentText: Backend.licenseDocument(currentDoc)

    readonly property var docTabs: [
        { key: "notices", label: qsTr("Third-Party Notices") },
        { key: "gplv3", label: "GNU GPLv3" },
        { key: "lgplv3", label: "GNU LGPLv3" }
    ]

    contentItem: ColumnLayout {
        spacing: 8

        HScrollRow {
            Layout.fillWidth: true
            margin: 0
            spacing: 6

            Repeater {
                model: dlg.docTabs
                RagoPillButton {
                    id: tabBtn
                    required property var modelData
                    text: modelData.label
                    checked: dlg.currentDoc === modelData.key
                    onClicked: {
                        dlg.currentDoc = tabBtn.modelData.key
                        licenseFlick.contentY = 0
                    }
                }
            }

            Item { Layout.fillWidth: true }

            RagoPillButton {
                // GPL §6: the licenses screen links the full source repository
                // (docs/WebAssembly.md §1.4).
                text: qsTr("Source Code")
                enabled: Brand.sourceRepoUrl.length > 0
                onClicked: Qt.openUrlExternally(Brand.sourceRepoUrl)
            }

            RagoPillButton {
                text: qsTr("Copy")
                enabled: dlg.documentText.length > 0
                onClicked: Backend.copyToClipboard(dlg.documentText)
            }
        }

        Label {
            visible: !dlg.cramped
            text: qsTr("%1 is free software under the GNU GPLv3 and uses the Qt %2 Toolkit (Copyright © The Qt Company Ltd.) under the GNU LGPLv3 / GPLv3.").arg(Brand.productName).arg(Backend.qtVersion)
            wrapMode: Text.WordWrap
            font.pixelSize: 11
            color: Theme.disabledTextColor
            Layout.fillWidth: true
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 10
            color: Theme.surfaceColor
            border.width: 1
            border.color: Theme.glassBorder
            clip: true

            Flickable {
                id: licenseFlick
                anchors.fill: parent
                anchors.margins: 10
                contentWidth: width
                contentHeight: licenseText.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                clip: true

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }

                TextEdit {
                    id: licenseText
                    width: licenseFlick.width
                    readOnly: true
                    selectByMouse: !Platform.isTouch
                    wrapMode: TextEdit.Wrap
                    textFormat: TextEdit.PlainText
                    font.family: AppSettings.codeFontFamily
                    font.pixelSize: 11
                    color: Theme.textColor
                    selectionColor: Theme.accentColor
                    selectedTextColor: "#ffffff"
                    text: dlg.documentText
                }
            }
        }
    }
}
