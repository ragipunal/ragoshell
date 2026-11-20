pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

// Aggregation pipeline builder: stage cards on the left, a sample of the
// selected stage's output on the right (PipelineBuilder runs the previews).
RagoDialog {
    id: dlg

    title: qsTr("Aggregation Pipeline Builder")
    acceptText: qsTr("Close")
    width: Math.min(visibleWidth - 24, 1200)
    height: Math.min(visibleHeight - 24, 820)

    readonly property bool stacked: width < 700

    /// Returns false when the server is not connected or no collection is given.
    function openFor(serverUuid, database, collection) {
        if (!builder.configure(serverUuid, database, collection))
            return false
        open()
        return true
    }

    // Dev/CI helper (--pipeline): append [[op, body], ...] after the first stage.
    function addDebugStages(stages) {
        for (var i = 0; i < stages.length; ++i) {
            if (i === 0) {
                builder.setOperator(0, stages[i][0])
                builder.setBody(0, stages[i][1])
            } else {
                builder.addStage(-1, stages[i][0])
                builder.setBody(builder.count - 1, stages[i][1])
            }
        }
        builder.selectedIndex = builder.count - 1
    }

    function _openInShell(execute) {
        Backend.openShellWithScript(builder.serverUuid, builder.database, builder.pipelineScript(), execute)
        dlg.close()
    }

    /// "Export Code": the pipeline as driver code (handled by Main).
    signal exportRequested(string script, string database, string serverUuid)

    PipelineBuilder { id: builder }

    contentItem: ColumnLayout {
        spacing: 10

        // ---- Toolbar --------------------------------------------------------
        HScrollRow {
            Layout.fillWidth: true
            margin: 0
            spacing: 8

            RagoBadge { text: builder.database + "." + builder.collection }
            Item { Layout.fillWidth: true }
            Label { text: qsTr("Sample"); color: Theme.disabledTextColor; font.pixelSize: 12 }
            RagoSpinBox {
                from: 1
                to: 200
                value: builder.sampleSize
                Layout.preferredWidth: 110
                onValueModified: builder.sampleSize = value
            }
            RagoPillButton {
                text: qsTr("Add Stage")
                iconText: "+"
                onClicked: builder.addStage(-1)
            }
            RagoPillButton {
                text: qsTr("Copy Pipeline")
                onClicked: Backend.copyToClipboard(builder.pipelineScript())
            }
            RagoPillButton {
                text: qsTr("Export Code")
                onClicked: dlg.exportRequested(builder.pipelineScript(), builder.database, builder.serverUuid)
            }
            RagoPillButton {
                text: qsTr("Open in Shell")
                onClicked: dlg._openInShell(false)
            }
            RagoPillButton {
                text: qsTr("Run in Shell")
                primary: true
                onClicked: dlg._openInShell(true)
            }
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: dlg.stacked ? Qt.Vertical : Qt.Horizontal

            // ---- Stage cards ------------------------------------------------
            ListView {
                id: stageList
                SplitView.preferredWidth: dlg.stacked ? -1 : (parent ? parent.width * 0.55 : 600)
                SplitView.minimumWidth: dlg.stacked ? 0 : 320
                SplitView.preferredHeight: dlg.stacked ? (parent ? parent.height * 0.56 : 260) : -1
                SplitView.minimumHeight: dlg.stacked ? 120 : 0
                clip: true
                spacing: 8
                model: builder

                footer: Item {
                    width: stageList.width
                    height: 52
                    RagoPillButton {
                        anchors.centerIn: parent
                        text: qsTr("Add Stage")
                        iconText: "+"
                        onClicked: builder.addStage(-1)
                    }
                }

                delegate: Rectangle {
                    id: card
                    required property int index
                    required property string op
                    required property string body
                    required property bool stageEnabled
                    required property string error

                    readonly property bool selected: builder.selectedIndex === index

                    width: stageList.width - 12
                    implicitHeight: cardCol.implicitHeight + 16
                    radius: 10
                    color: Theme.surfaceColor
                    opacity: stageEnabled ? 1.0 : 0.6
                    border.width: selected ? 2 : 1
                    border.color: error.length > 0 ? Theme.dangerColor
                                                    : (selected ? Theme.accentColor : Theme.glassBorder)

                    TapHandler { onTapped: builder.selectedIndex = card.index }

                    ColumnLayout {
                        id: cardCol
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 6

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: card.width < 360 ? 3 : 6

                            Label {
                                text: String(card.index + 1)
                                font.bold: true
                                color: Theme.disabledTextColor
                                Layout.preferredWidth: 14
                            }
                            RagoCheckBox {
                                checked: card.stageEnabled
                                onToggled: builder.setStageEnabled(card.index, checked)
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Include this stage")
                            }
                            RagoComboBox {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 72
                                Layout.maximumWidth: 170
                                model: builder.operators
                                currentIndex: builder.operators.indexOf(card.op)
                                displayText: card.op
                                onActivated: builder.setOperator(card.index, currentText)
                            }
                            Item { Layout.fillWidth: true }
                            RagoIconButton {
                                glyph: "▲"
                                size: 26
                                enabled: card.index > 0
                                toolTipText: qsTr("Move up")
                                onClicked: builder.moveStage(card.index, card.index - 1)
                            }
                            RagoIconButton {
                                glyph: "▼"
                                size: 26
                                enabled: card.index < builder.count - 1
                                toolTipText: qsTr("Move down")
                                onClicked: builder.moveStage(card.index, card.index + 1)
                            }
                            RagoIconButton {
                                glyph: "+"
                                size: 26
                                toolTipText: qsTr("Add stage below")
                                onClicked: builder.addStage(card.index)
                            }
                            RagoIconButton {
                                glyph: "✕"
                                danger: true
                                size: 26
                                toolTipText: qsTr("Delete stage")
                                onClicked: builder.removeStage(card.index)
                            }
                        }

                        RagoCodeEditor {
                            id: bodyEditor
                            Layout.fillWidth: true
                            Layout.preferredHeight: 110
                            lineNumbers: false
                            folding: false
                            Component.onCompleted: text = card.body
                            onTextChanged: builder.setBody(card.index, text)
                            onActiveFocusChanged: if (activeFocus) builder.selectedIndex = card.index
                        }
                        // Operator change swaps in a template from C++.
                        Connections {
                            target: card
                            function onBodyChanged() {
                                if (bodyEditor.text !== card.body)
                                    bodyEditor.text = card.body
                            }
                        }

                        Label {
                            visible: card.error.length > 0
                            Layout.fillWidth: true
                            text: card.error
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.dangerColor
                        }
                    }
                }
                ScrollBar.vertical: ScrollBar {}
            }

            // ---- Preview ----------------------------------------------------
            Rectangle {
                SplitView.fillWidth: true
                SplitView.fillHeight: dlg.stacked
                SplitView.minimumWidth: dlg.stacked ? 0 : 260
                SplitView.minimumHeight: dlg.stacked ? 100 : 0
                color: Theme.baseColor
                radius: 10
                border.width: 1
                border.color: Theme.glassBorder

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: builder.previewTitle.length > 0 ? builder.previewTitle
                                                                  : qsTr("Select a stage to preview its output")
                            font.bold: true
                            elide: Text.ElideRight
                            color: Theme.textColor
                        }
                        BusyIndicator {
                            running: builder.previewBusy
                            visible: running
                            Layout.preferredWidth: 20
                            Layout.preferredHeight: 20
                        }
                    }
                    Label {
                        visible: !builder.previewBusy && builder.previewError.length === 0
                                 && builder.previewTitle.length > 0
                        text: builder.previewCount === 0
                              ? qsTr("No documents")
                              : qsTr("%n document(s), sample of up to %1", "", builder.previewCount).arg(builder.sampleSize)
                        font.pixelSize: 12
                        color: Theme.disabledTextColor
                    }
                    Label {
                        visible: builder.previewError.length > 0
                        Layout.fillWidth: true
                        text: builder.previewError
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                        color: Theme.dangerColor
                    }
                    RagoCodeEditor {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        readOnly: true
                        text: builder.previewText
                    }
                }
            }
        }
    }
}
