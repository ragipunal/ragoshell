pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Explain plan view (ShellController viewMode 4): summary counters, plan
// warnings and the stage tree of a `.explain()` result.
Rectangle {
    id: view

    property ShellController shell
    readonly property var summary: shell ? shell.explainSummary : ({})
    readonly property color warnColor: "#f59e0b"

    function count(v) { return (v === undefined || v < 0) ? "—" : Number(v).toLocaleString(Qt.locale(), "f", 0) }

    color: Theme.baseColor

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Summary ------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: summaryCol.implicitHeight + 20
            color: Theme.surfaceColor

            ColumnLayout {
                id: summaryCol
                anchors.fill: parent
                anchors.margins: 10
                anchors.leftMargin: 12
                spacing: 8

                Flow {
                    Layout.fillWidth: true
                    spacing: 8

                    RagoBadge { text: view.summary.ns || qsTr("explain"); badgeColor: Theme.accentColor }
                    RagoBadge {
                        visible: !!view.summary.hasExecutionStats
                        text: qsTr("Returned %1").arg(view.count(view.summary.nReturned))
                    }
                    RagoBadge {
                        visible: !!view.summary.hasExecutionStats
                        text: qsTr("Docs examined %1").arg(view.count(view.summary.docsExamined))
                        badgeColor: Theme.textColor
                    }
                    RagoBadge {
                        visible: !!view.summary.hasExecutionStats
                        text: qsTr("Keys examined %1").arg(view.count(view.summary.keysExamined))
                        badgeColor: Theme.textColor
                    }
                    RagoBadge {
                        visible: !!view.summary.hasExecutionStats
                        text: qsTr("%1 ms").arg(view.count(view.summary.timeMs))
                        badgeColor: Theme.textColor
                    }
                    RagoBadge {
                        readonly property var indexes: view.summary.indexes || []
                        text: indexes.length > 0 ? qsTr("Index: %1").arg(indexes.join(", "))
                                                 : qsTr("No index used")
                        badgeColor: indexes.length > 0 ? Theme.statusOnline : view.warnColor
                    }
                }

                Repeater {
                    model: view.summary.warnings || []
                    Label {
                        required property string modelData
                        Layout.fillWidth: true
                        text: "⚠ " + modelData
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                        color: view.warnColor
                    }
                }

                Label {
                    visible: view.shell ? !view.summary.hasExecutionStats : false
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Planner output only. Use .explain('executionStats') for returned/examined counters.")
                    font.pixelSize: 12
                    color: Theme.disabledTextColor
                }
            }
        }

        // ---- Column header ------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: Theme.alternateColor

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8
                //: Column header: stage of the query plan (COLLSCAN, IXSCAN, FETCH ...).
                Label { text: qsTr("Stage"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.fillWidth: true }
                Label { text: qsTr("Returned"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 80; horizontalAlignment: Text.AlignRight }
                Label { text: qsTr("Docs"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 80; horizontalAlignment: Text.AlignRight }
                //: Column header: number of index keys examined.
                Label { text: qsTr("Keys"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 80; horizontalAlignment: Text.AlignRight }
                Label { text: qsTr("ms"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 50; horizontalAlignment: Text.AlignRight }
            }
        }

        // ---- Stage tree (pre-flattened, indented by depth) ---------------
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: view.shell ? view.shell.explainStages : []

            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                width: ListView.view.width
                height: Math.max(30, AppSettings.codeFontSize + 16)
                color: index % 2 === 1 ? Theme.alternateColor : "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12 + row.modelData.depth * 18
                    anchors.rightMargin: 12
                    spacing: 8

                    Label {
                        visible: row.modelData.depth > 0
                        text: "└"
                        color: Theme.disabledTextColor
                        font.family: AppSettings.codeFontFamily
                    }
                    RagoBadge {
                        text: row.modelData.stage
                        badgeColor: row.modelData.warning ? view.warnColor : Theme.accentColor
                    }
                    Label {
                        Layout.fillWidth: true
                        text: row.modelData.detail
                        elide: Text.ElideRight
                        font.family: AppSettings.codeFontFamily
                        font.pixelSize: AppSettings.codeFontSize
                        color: Theme.textColor
                        ToolTip.visible: detailHover.hovered && truncated
                        ToolTip.text: row.modelData.detail
                        HoverHandler { id: detailHover }
                    }
                    Label { text: view.count(row.modelData.nReturned); font.family: AppSettings.codeFontFamily; font.pixelSize: AppSettings.codeFontSize; color: Theme.textColor; Layout.preferredWidth: 80; horizontalAlignment: Text.AlignRight }
                    Label { text: view.count(row.modelData.docsExamined); font.family: AppSettings.codeFontFamily; font.pixelSize: AppSettings.codeFontSize; color: Theme.textColor; Layout.preferredWidth: 80; horizontalAlignment: Text.AlignRight }
                    Label { text: view.count(row.modelData.keysExamined); font.family: AppSettings.codeFontFamily; font.pixelSize: AppSettings.codeFontSize; color: Theme.textColor; Layout.preferredWidth: 80; horizontalAlignment: Text.AlignRight }
                    Label { text: view.count(row.modelData.timeMs); font.family: AppSettings.codeFontFamily; font.pixelSize: AppSettings.codeFontSize; color: Theme.disabledTextColor; Layout.preferredWidth: 50; horizontalAlignment: Text.AlignRight }
                }
            }
            ScrollBar.vertical: ScrollBar {}
        }
    }
}
