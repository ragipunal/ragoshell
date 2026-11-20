pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

Rectangle {
    id: opsPane
    required property var monPage
    SplitView.fillHeight: true
    SplitView.minimumHeight: 180
    color: Theme.baseColor

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Filter Toolbar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 42
            color: Theme.surfaceColor
            border.width: 1
            border.color: Theme.glassBorder

            HScrollRow {
                anchors.fill: parent
                margin: 12
                spacing: 10

                Label {
                    text: qsTr("CURRENT OPERATIONS")
                    font.pixelSize: 11
                    font.bold: true
                    font.letterSpacing: Language.rtl ? 0 : 1.0
                    color: Theme.accentColor
                }

                RagoBadge {
                    text: opsPane.monPage.monitor
                          ? (opsPane.monPage.monitor.currentOps.length + " / " + opsPane.monPage.monitor.totalRawOpsCount)
                          : "0"
                    badgeColor: Theme.accentColor
                }

                RagoCheckBox {
                    text: qsTr("Active only")
                    checked: opsPane.monPage.monitor ? opsPane.monPage.monitor.activeOnly : true
                    onToggled: if (opsPane.monPage.monitor) opsPane.monPage.monitor.activeOnly = checked
                }

                RagoCheckBox {
                    text: qsTr("Hide system")
                    checked: opsPane.monPage.monitor ? opsPane.monPage.monitor.hideSystemOps : true
                    onToggled: if (opsPane.monPage.monitor) opsPane.monPage.monitor.hideSystemOps = checked
                }

                Label {
                    text: qsTr("Min ms:")
                    font.pixelSize: 11
                    color: Theme.disabledTextColor
                }

                RagoTextField {
                    Layout.preferredWidth: 64
                    implicitHeight: 26
                    font.family: AppSettings.codeFontFamily
                    font.pixelSize: AppSettings.codeFontSize
                    horizontalAlignment: TextInput.AlignHCenter
                    text: opsPane.monPage.monitor ? String(opsPane.monPage.monitor.minDurationMs) : "0"
                    onTextEdited: {
                        if (!opsPane.monPage.monitor) return
                        var v = parseInt(text, 10)
                        opsPane.monPage.monitor.minDurationMs = isNaN(v) || v < 0 ? 0 : v
                    }
                }

                RagoTextField {
                    Layout.fillWidth: true
                    implicitHeight: 28
                    font.family: AppSettings.codeFontFamily
                    font.pixelSize: AppSettings.codeFontSize
                    placeholderText: qsTr("Filter by namespace, client IP, op type, plan, or command…")
                    onTextEdited: if (opsPane.monPage.monitor) opsPane.monPage.monitor.searchFilter = text
                }
            }
        }

        // Table Header
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: Theme.surfaceColor
            border.width: 1
            border.color: Theme.glassBorder

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8

                Label { text: qsTr("OpID");      font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 86 }
                //: Column header: type of a running operation (query, insert, command ...).
                Label { text: qsTr("Type");      font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 74; visible: opsPane.monPage.width >= 560 }
                Label { text: qsTr("Namespace"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 165; visible: opsPane.monPage.width >= 440 }
                Label { text: qsTr("Duration");  font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 82; visible: opsPane.monPage.width >= 560 }
                Label { text: qsTr("Client");    font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 150; visible: opsPane.monPage.width >= 900 }
                Label { text: qsTr("Plan");      font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 110; visible: opsPane.monPage.width >= 900 }
                Label { text: qsTr("Command / Query"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.fillWidth: true }
                Label { text: qsTr("Actions");   font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 146; horizontalAlignment: Text.AlignRight }
            }
        }

        // Operations List
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: opsList
                anchors.fill: parent
                clip: true
                model: opsPane.monPage.monitor ? opsPane.monPage.monitor.currentOps : []

                delegate: Rectangle {
                    id: opRow
                    required property int index
                    required property var modelData

                    width: opsList.width
                    height: Math.max(32, AppSettings.codeFontSize + 18)
                    color: index % 2 === 1 ? Theme.alternateColor : "transparent"

                    readonly property real durMs: Number(modelData.durationMs || 0)

                    MouseArea {
                        anchors.fill: parent
                        onDoubleClicked: {
                            inspectOpDialog.get().openForView(
                                opsPane.monPage.monitor ? opsPane.monPage.monitor.host : "",
                                "currentOp",
                                "opid " + (opRow.modelData.opid || ""),
                                opRow.modelData.rawJson || "{}")
                        }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 8

                        Label {
                            text: opRow.modelData.opid || ""
                            font.family: AppSettings.codeFontFamily
                            font.pixelSize: AppSettings.codeFontSize
                            font.bold: true
                            color: Theme.textColor
                            elide: Text.ElideRight
                            Layout.preferredWidth: 86
                        }

                        Item {
                            Layout.preferredWidth: 74
                            visible: opsPane.monPage.width >= 560
                            implicitHeight: 22
                            RagoBadge {
                                anchors.verticalCenter: parent.verticalCenter
                                text: opRow.modelData.op || "op"
                                badgeColor: opRow.modelData.op === "query" ? "#3b9eff" :
                                            opRow.modelData.op === "insert" ? Theme.accentColor :
                                            opRow.modelData.op === "update" ? "#f59e0b" :
                                            opRow.modelData.op === "remove" ? "#ff6b6b" :
                                            "#a78bfa"
                            }
                        }

                        Label {
                            text: opRow.modelData.ns || "—"
                            font.family: AppSettings.codeFontFamily
                            font.pixelSize: AppSettings.codeFontSize
                            color: Theme.textColor
                            elide: Text.ElideRight
                            Layout.preferredWidth: 165
                            visible: opsPane.monPage.width >= 440
                        }

                        Label {
                            text: opRow.modelData.durationText || "0 ms"
                            font.family: AppSettings.codeFontFamily
                            font.pixelSize: AppSettings.codeFontSize
                            font.bold: opRow.durMs >= 1000
                            color: opRow.durMs >= 5000 ? "#ff6b6b"
                                 : (opRow.durMs >= 1000 ? "#f59e0b" : Theme.textColor)
                            elide: Text.ElideRight
                            Layout.preferredWidth: 82
                            visible: opsPane.monPage.width >= 560
                        }

                        Label {
                            text: opRow.modelData.client || ""
                            font.family: AppSettings.codeFontFamily
                            font.pixelSize: Math.max(11, AppSettings.codeFontSize - 1)
                            color: Theme.disabledTextColor
                            elide: Text.ElideRight
                            Layout.preferredWidth: 150
                            visible: opsPane.monPage.width >= 900   // the client and the plan make way on a narrow page
                        }

                        Label {
                            text: opRow.modelData.waitingForLock
                                  ? ("🔒 " + (opRow.modelData.planSummary || "LOCK"))
                                  : (opRow.modelData.planSummary || "—")
                            font.family: AppSettings.codeFontFamily
                            font.pixelSize: Math.max(11, AppSettings.codeFontSize - 1)
                            color: opRow.modelData.waitingForLock ? "#ff6b6b" : Theme.textColor
                            elide: Text.ElideRight
                            Layout.preferredWidth: 110
                            visible: opsPane.monPage.width >= 900
                        }

                        Label {
                            text: opRow.modelData.commandSummary || ""
                            font.family: AppSettings.codeFontFamily
                            font.pixelSize: AppSettings.codeFontSize
                            color: Theme.textColor
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                        }

                        RowLayout {
                            spacing: 4
                            Layout.preferredWidth: 146

                            Item { Layout.fillWidth: true }

                            RagoPillButton {
                                text: qsTr("JSON")
                                implicitHeight: 24
                                onClicked: {
                                    inspectOpDialog.get().openForView(
                                        opsPane.monPage.monitor ? opsPane.monPage.monitor.host : "",
                                        "currentOp",
                                        "opid " + (opRow.modelData.opid || ""),
                                        opRow.modelData.rawJson || "{}")
                                }
                            }

                            RagoPillButton {
                                text: qsTr("Kill")
                                danger: true
                                implicitHeight: 24
                                enabled: (opRow.modelData.opid || "").length > 0
                                onClicked: {
                                    opsPane.monPage._pendingKillOpId = String(opRow.modelData.opid)
                                    opsPane.monPage._pendingKillNs = String(opRow.modelData.ns || "")
                                    killOpConfirm.open()
                                }
                            }
                        }
                    }
                }

                ScrollBar.vertical: ScrollBar {}
            }

            Label {
                anchors.centerIn: parent
                width: Math.min(implicitWidth, parent.width - 24)
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                visible: opsList.count === 0
                text: qsTr("No matching active operations right now. Uncheck 'Active only' or 'Hide system' to inspect idle/background threads.")
                font.pixelSize: 12
                color: Theme.disabledTextColor
            }
        }
    }

    LazyDialog { id: inspectOpDialog; sourceComponent: Component { DocumentEditorDialog {} } }

    RagoDialog {
        id: killOpConfirm
        title: qsTr("Kill Active MongoDB Operation")
        acceptText: qsTr("Kill Operation")
        rejectText: qsTr("Cancel")
        showReject: true
        dangerAccept: true
        width: Math.min(visibleWidth - 24, 460)

        contentItem: ColumnLayout {
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                RagoBadge {
                    visible: opsPane.monPage.monitor ? opsPane.monPage.monitor.host.length > 0 : false
                    text: "⛁ " + (opsPane.monPage.monitor ? opsPane.monPage.monitor.host : "")
                    badgeColor: opsPane.monPage.serverColorTag.length > 0 ? opsPane.monPage.serverColorTag : Theme.accentColor
                }

                RagoBadge {
                    visible: opsPane.monPage._pendingKillOpId.length > 0
                    text: "opid: " + opsPane.monPage._pendingKillOpId
                    badgeColor: Theme.dangerColor
                }

                RagoBadge {
                    visible: opsPane.monPage._pendingKillNs.length > 0
                    text: opsPane.monPage._pendingKillNs
                    badgeColor: "#3b9eff"
                }

                Item { Layout.fillWidth: true }
            }

            Label {
                text: qsTr("Are you sure you want to terminate running MongoDB operation '%1'%2?")
                      .arg(opsPane.monPage._pendingKillOpId)
                      .arg(opsPane.monPage._pendingKillNs.length > 0 ? (" on " + opsPane.monPage._pendingKillNs) : "")
                wrapMode: Text.WordWrap
                font.pixelSize: 13
                color: Theme.textColor
                Layout.fillWidth: true
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: killWarnLabel.implicitHeight + 14
                radius: 8
                color: Qt.rgba(Theme.dangerColor.r, Theme.dangerColor.g, Theme.dangerColor.b, 0.12)
                border.width: 1
                border.color: Qt.rgba(Theme.dangerColor.r, Theme.dangerColor.g, Theme.dangerColor.b, 0.38)

                Label {
                    id: killWarnLabel
                    anchors.fill: parent
                    anchors.margins: 7
                    text: qsTr("⚠ Killing an operation immediately interrupts the client query, index build, or write command on the server. Non-transactional multi-document writes may remain partially applied.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.dangerColor
                }
            }
        }

        onAccepted: {
            if (opsPane.monPage.monitor && opsPane.monPage._pendingKillOpId.length > 0)
                opsPane.monPage.monitor.killOperation(opsPane.monPage._pendingKillOpId)
        }
    }
}
