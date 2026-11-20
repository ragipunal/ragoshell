pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

ColumnLayout {
    id: hdr
    required property var page
    required property bool editorHasSelection
    required property int splitOrientation
    readonly property bool showKeyHints: Platform.shortcutsVisible && !Theme.touch && hdr.width >= 620
    spacing: 0

    // Minimalist segment button inside the view-mode switcher track:
    // sleek 24-26pt visual pill with full toolbar touch height.
    component ViewSegment: Item {
        id: seg
        property string text: ""
        property bool checked: false
        signal clicked()

        implicitWidth: segLabel.implicitWidth + 16
        implicitHeight: Theme.touch ? 32 : 24

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            height: Theme.touch ? 24 : 22
            radius: 5
            color: seg.checked
                   ? (Theme.dark ? Theme.selectedColor : Theme.baseColor)
                   : (segMouse.pressed ? Theme.cardBackgroundHover : "transparent")
            border.width: seg.checked ? 1 : 0
            border.color: Theme.accentColor
        }

        Label {
            id: segLabel
            anchors.centerIn: parent
            text: seg.text
            font.pixelSize: 12
            font.weight: seg.checked ? Font.Bold : Font.Medium
            color: seg.checked ? Theme.accentColor : Theme.disabledTextColor
        }

        MouseArea {
            id: segMouse
            anchors.fill: parent
            hoverEnabled: !Theme.touch
            cursorShape: Qt.PointingHandCursor
            onClicked: seg.clicked()
        }
    }

    // Execution, View Mode & Visible CRUD Toolbar
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: Theme.touch ? 36 : 34
        color: Theme.surfaceColor
        border.width: 1
        border.color: Theme.glassBorder

        HScrollRow {
            anchors.fill: parent
            margin: 8
            spacing: 6

            RagoPillButton {
                compact: true
                text: hdr.editorHasSelection
                      ? Platform.withShortcut(qsTr("Execute Selection (%1)"), hdr.showKeyHints ? "F5" : "")
                      : Platform.withShortcut(qsTr("Execute (%1)"), hdr.showKeyHints ? "F5" : "")
                iconText: "▶"
                primary: true
                enabled: hdr.page.shell ? !hdr.page.shell.busy : false
                onClicked: hdr.page.executeCurrent()
            }

            RagoPillButton {
                compact: true
                text: Platform.withShortcut(qsTr("Stop (%1)"), hdr.showKeyHints ? "F6" : "")
                iconText: "■"
                danger: true
                visible: hdr.page.shell ? hdr.page.shell.busy : false
                onClicked: hdr.page.stopCurrent()
            }

            BusyIndicator {
                visible: running
                running: hdr.page.shell ? hdr.page.shell.busy : false
                Layout.preferredHeight: 18
                Layout.preferredWidth: 18
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: 16
                color: Theme.glassBorder
            }

            // Minimalist unified segmented view switcher: Tree / Table / JSON / Summary / Explain
            Item {
                implicitWidth: segRow.implicitWidth + 4
                implicitHeight: Theme.touch ? 32 : 26

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    height: Theme.touch ? 28 : 26
                    radius: 7
                    color: Theme.glassBackground
                    border.width: 1
                    border.color: Theme.glassBorder
                }

                RowLayout {
                    id: segRow
                    anchors.fill: parent
                    anchors.leftMargin: 2
                    anchors.rightMargin: 2
                    spacing: 2

                    ViewSegment {
                        text: Platform.withShortcut(qsTr("Tree (%1)"), hdr.showKeyHints ? "F2" : "")
                        checked: hdr.page.shell ? hdr.page.shell.viewMode === 0 : true
                        onClicked: if (hdr.page.shell) hdr.page.shell.viewMode = 0
                    }
                    ViewSegment {
                        text: Platform.withShortcut(qsTr("Table (%1)"), hdr.showKeyHints ? "F3" : "")
                        checked: hdr.page.shell ? hdr.page.shell.viewMode === 1 : false
                        onClicked: if (hdr.page.shell) hdr.page.shell.viewMode = 1
                    }
                    ViewSegment {
                        text: Platform.withShortcut(qsTr("JSON (%1)"), hdr.showKeyHints ? "F4" : "")
                        checked: hdr.page.shell ? hdr.page.shell.viewMode === 2 : false
                        onClicked: if (hdr.page.shell) hdr.page.shell.viewMode = 2
                    }
                    ViewSegment {
                        visible: hdr.page.shell ? hdr.page.shell.hasStatsSummary : false
                        text: qsTr("Summary")
                        checked: hdr.page.shell ? hdr.page.shell.viewMode === 3 : false
                        onClicked: if (hdr.page.shell) hdr.page.shell.viewMode = 3
                    }
                    ViewSegment {
                        visible: hdr.page.shell ? hdr.page.shell.hasExplain : false
                        //: Button that runs MongoDB explain() and shows the query plan.
                        text: qsTr("Explain")
                        checked: hdr.page.shell ? hdr.page.shell.viewMode === 4 : false
                        onClicked: if (hdr.page.shell) hdr.page.shell.viewMode = 4
                    }
                }
            }

            Rectangle {
                visible: hdr.page.shell ? (hdr.page.shell.canInsertDocuments || hdr.page.shell.isReadOnlyResult) : false
                Layout.preferredWidth: 1
                Layout.preferredHeight: 16
                color: Theme.glassBorder
            }

            // Visible CRUD buttons (desktop + touch friendly; strictly guarded by query result type)
            RowLayout {
                spacing: 4
                visible: hdr.page.shell ? hdr.page.shell.canInsertDocuments : false

                RagoPillButton {
                    compact: true
                    //: Button: insert a new document into the collection.
                    text: qsTr("Insert")
                    iconText: "+"
                    enabled: hdr.page.shell ? (!hdr.page.shell.busy && hdr.page.shell.canInsertDocuments) : false
                    onClicked: hdr.page.triggerInsertDocument()
                }

                RagoPillButton {
                    compact: true
                    visible: hdr.page.shell ? hdr.page.shell.canModifyDocuments : false
                    //: Button: edit the selected document.
                    text: qsTr("Edit")
                    iconText: "✎"
                    enabled: hdr.page.shell
                             ? (!hdr.page.shell.busy && hdr.page.shell.canModifyDocuments && hdr.page.selectedDocIndices.length === 1)
                             : false
                    onClicked: hdr.page.triggerEditSelectedDocument()
                }

                RagoPillButton {
                    compact: true
                    visible: hdr.page.shell ? hdr.page.shell.canDeleteDocuments : false
                    text: hdr.page.selectedDocIndices.length > 1
                          ? qsTr("Delete (%1)").arg(hdr.page.selectedDocIndices.length)
                          : qsTr("Delete")
                    iconText: "✕"
                    danger: true
                    enabled: hdr.page.shell
                             ? (!hdr.page.shell.busy && hdr.page.shell.canDeleteDocuments && hdr.page.selectedDocIndices.length > 0)
                             : false
                    onClicked: hdr.page.triggerDeleteSelectedDocuments()
                }
            }

            RagoBadge {
                visible: hdr.page.shell ? hdr.page.shell.isReadOnlyResult : false
                text: qsTr("Read-only result")
                badgeColor: Theme.disabledTextColor
            }

            // Touch/mouse overflow menu button for Copy/View/Expand actions
            RagoIconButton {
                id: moreActionsBtn
                visible: hdr.page.shell ? hdr.page.shell.resultCount > 0 : false
                glyph: "⋮"
                size: 24
                toolTipText: qsTr("Document & Copy Actions")
                onClicked: {
                    if (hdr.page.selectedDocIndices.length > 0 &&
                        (hdr.page._ctxDocIndex < 0 ||
                         (hdr.page.shell && hdr.page._ctxDocIndex >= hdr.page.shell.resultCount) ||
                         !hdr.page.isDocSelected(hdr.page._ctxDocIndex))) {
                        hdr.page._ctxDocIndex = hdr.page.selectedDocIndices[0]
                    }
                    hdr.page.openResultMenuAt(moreActionsBtn, 0, moreActionsBtn.height + 2)
                }
            }

            Item { Layout.fillWidth: true }

            RagoIconButton {
                glyph: hdr.splitOrientation === Qt.Vertical ? "◫" : "⬒"
                size: 24
                toolTipText: Platform.withShortcut(qsTr("Toggle Split Orientation (%1)"), "F10")
                onClicked: hdr.page.toggleOrientation()
            }

            // Result count / timing pill
            Rectangle {
                visible: hdr.page.shell ? (hdr.page.shell.resultCount > 0 || hdr.page.shell.elapsedMs > 0) : false
                radius: 6
                implicitHeight: 22
                implicitWidth: statsText.implicitWidth + 14
                color: Theme.glassBackground
                border.width: 1
                border.color: Theme.glassBorder

                Label {
                    id: statsText
                    anchors.centerIn: parent
                    color: Theme.textColor
                    font.pixelSize: 11
                    font.bold: true
                    text: hdr.page.shell
                          ? qsTr("%n doc(s) · %1 ms", "", hdr.page.shell.resultCount).arg(hdr.page.shell.elapsedMs)
                          : ""
                }
            }
        }
    }

    // Multi-statement Result Sub-Tabs (visible when >1 statement returned output)
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 34
        visible: hdr.page.shell ? hdr.page.shell.resultTabTitles.length > 1 : false
        color: Theme.baseColor
        border.width: 1
        border.color: Theme.glassBorder

        Flickable {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            contentWidth: subTabRow.implicitWidth
            clip: true

            RowLayout {
                id: subTabRow
                height: parent.height
                spacing: 6

                Repeater {
                    model: hdr.page.shell ? hdr.page.shell.resultTabTitles : []
                    RagoPillButton {
                        id: resultPill
                        required property int index
                        required property string modelData
                        compact: true
                        text: resultPill.modelData
                        implicitHeight: 24
                        checked: hdr.page.shell ? hdr.page.shell.activeResultIndex === resultPill.index : false
                        onClicked: if (hdr.page.shell) hdr.page.shell.activeResultIndex = resultPill.index
                    }
                }
            }
        }
    }

    // Error banner
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: errLabel.implicitHeight + 16
        visible: hdr.page.shell ? hdr.page.shell.errorText.length > 0 : false
        color: Theme.dark ? "#471818" : "#fde8e8"
        border.width: 1
        border.color: "#e05252"

        RowLayout {
            anchors.fill: parent
            anchors.margins: 8
            spacing: 8

            Label {
                id: errLabel
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.family: AppSettings.codeFontFamily
                font.pixelSize: 12
                color: Theme.dark ? "#ffb4b4" : "#9b1c1c"
                text: hdr.page.shell ? hdr.page.shell.errorText : ""
            }

            RagoPillButton {
                compact: true
                text: qsTr("Copy")
                implicitHeight: 24
                onClicked: if (hdr.page.shell) hdr.page.shell.copyToClipboard(hdr.page.shell.errorText)
            }
        }
    }

    // Cancelled operation banner
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 36
        visible: hdr.page.shell ? (!hdr.page.shell.busy && hdr.page.shell.cancelled && hdr.page.shell.errorText.length === 0) : false
        color: Theme.dark ? Qt.rgba(0.96, 0.62, 0.04, 0.14) : Qt.rgba(0.96, 0.62, 0.04, 0.10)
        border.width: 1
        border.color: Qt.rgba(0.96, 0.62, 0.04, 0.45)

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 8

            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                font.pixelSize: 12
                font.weight: Font.DemiBold
                color: Theme.dark ? "#fcd34d" : "#92400e"
                text: qsTr("■ Operation cancelled by user.")
            }

            RagoPillButton {
                compact: true
                text: Platform.withShortcut(qsTr("Run Again (%1)"), hdr.showKeyHints ? "F5" : "")
                iconText: "▶"
                implicitHeight: 24
                onClicked: hdr.page.executeCurrent()
            }
        }
    }

}
