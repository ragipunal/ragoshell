pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

ColumnLayout {
    id: diagFooter
    required property var dlg
    spacing: 10

    readonly property bool diagVisible: diagPanel.visible
    readonly property real diagPreferredHeight: diagPanel.Layout.preferredHeight
    property alias statusText: testResultLabel.text
    property alias statusColor: testResultLabel.color

    // ---- Expandable Diagnostic Steps Panel ----------------------------
    Rectangle {
        id: diagPanel
        visible: diagFooter.dlg.showDiagnostics && diagFooter.dlg.diagnosticSteps.length > 0
        Layout.fillWidth: true
        Layout.preferredHeight: diagCol.implicitHeight + 20
        radius: 9
        color: Theme.glassBackground
        border.width: 1
        border.color: Theme.glassBorder

        ColumnLayout {
            id: diagCol
            anchors.fill: parent
            anchors.margins: 10
            spacing: 5

            Repeater {
                model: diagFooter.dlg.diagnosticSteps
                RowLayout {
                    id: diagStep
                    required property var modelData
                    readonly property string status: modelData.status
                    Layout.fillWidth: true
                    spacing: 8

                    Label {
                        text: diagStep.status === "ok" ? "✓"
                              : diagStep.status === "error" ? "✕"
                              : diagStep.status === "skipped" ? "–" : "…"
                        font.pixelSize: 12
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        Layout.preferredWidth: 14
                        color: diagStep.status === "ok" ? Theme.accentColor
                               : diagStep.status === "error" ? Theme.dangerColor
                               : diagStep.status === "skipped" ? Theme.disabledTextColor : "#f59e0b"
                    }
                    Label {
                        text: diagStep.modelData.label
                        font.pixelSize: 11
                        font.bold: true
                        color: diagStep.status === "skipped" ? Theme.disabledTextColor : Theme.textColor
                        elide: Text.ElideRight
                        Layout.preferredWidth: 130
                    }
                    Label {
                        text: diagFooter.dlg.firstLine(diagStep.modelData.detail)
                        textFormat: Text.PlainText
                        font.pixelSize: 11
                        color: diagStep.status === "error" ? Theme.dangerColor : Theme.disabledTextColor
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                    }
                }
            }

            // Full error text: selectable, scrolls instead of growing.
            Rectangle {
                visible: diagFooter.dlg.testErrorText.length > 0
                Layout.fillWidth: true
                Layout.topMargin: 4
                Layout.preferredHeight: Math.min(errorTextArea.implicitHeight + 2, 100)
                radius: 7
                color: Theme.dark ? "#2a1616" : "#fdecec"
                border.width: 1
                border.color: Qt.rgba(Theme.dangerColor.r, Theme.dangerColor.g, Theme.dangerColor.b, 0.45)

                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 1
                    anchors.rightMargin: 32
                    clip: true

                    TextArea {
                        id: errorTextArea
                        text: diagFooter.dlg.testErrorText
                        textFormat: TextEdit.PlainText
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        padding: 8
                        font.family: AppSettings.codeFontFamily
                        font.pixelSize: 11
                        color: Theme.dark ? "#ffb4b4" : "#9b1c1c"
                        background: null
                    }
                }

                RagoIconButton {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 4
                    glyph: "❐"
                    size: 24
                    toolTipText: qsTr("Copy error")
                    onClicked: Backend.copyToClipboard(diagFooter.dlg.testErrorText)
                }
            }
        }
    }

    // ---- Test connection bar ------------------------------------------
    // Left out while the keyboard leaves little room (it comes back when the keyboard closes).
    Rectangle {
        visible: !diagFooter.dlg.cramped
        Layout.fillWidth: true
        Layout.preferredHeight: testBarRow.implicitHeight + 8
        radius: 10
        color: Theme.surfaceColor
        border.width: 1
        border.color: Theme.glassBorder

        RowLayout {
            id: testBarRow
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 10

            RagoPillButton {
                text: qsTr("Test Connection")
                iconText: "⚡\uFE0E"
                onClicked: {
                    if (!diagFooter.dlg.validateBeforeSave())
                        return
                    testResultLabel.text = qsTr("Testing connection…")
                    testResultLabel.color = Theme.disabledTextColor
                    diagFooter.dlg.testErrorText = ""
                    Backend.testConnection(diagFooter.dlg.collectData())
                }
            }

            // One line only; the full error lives in the details panel.
            Label {
                id: testResultLabel
                Layout.fillWidth: true
                font.pixelSize: 12
                textFormat: Text.PlainText
                wrapMode: Text.NoWrap
                maximumLineCount: 1
                elide: Text.ElideRight
                color: Theme.disabledTextColor

                ToolTip.visible: testHover.hovered && testResultLabel.text.length > 0
                ToolTip.delay: 300
                ToolTip.text: testResultLabel.text

                HoverHandler { id: testHover }
            }

            RagoPillButton {
                visible: diagFooter.dlg.diagnosticSteps.length > 0
                text: diagFooter.dlg.showDiagnostics ? qsTr("Hide Details") : qsTr("Diagnostics")
                implicitHeight: 26
                onClicked: diagFooter.dlg.showDiagnostics = !diagFooter.dlg.showDiagnostics
            }
        }
    }
}
