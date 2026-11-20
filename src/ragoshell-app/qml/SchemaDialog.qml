pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

// Schema analysis of a sampled collection: one row per field path with
// presence, type mix and frequent values or min/max range.
RagoDialog {
    id: dlg

    title: qsTr("Schema Analysis")
    acceptText: qsTr("Close")
    width: Math.min(visibleWidth - 24, 1100)
    height: Math.min(visibleHeight - 24, 780)

    /// Returns false when the server is not connected or no collection is given.
    function openFor(serverUuid, database, collection) {
        if (!analyzer.configure(serverUuid, database, collection))
            return false
        open()
        return true
    }

    SchemaAnalyzer { id: analyzer }

    contentItem: ScrollView {
        id: pageScrollView
        clip: true
        contentWidth: availableWidth
        rightPadding: bodyCol.implicitHeight > availableHeight ? 8 : 0

        ColumnLayout {
            id: bodyCol
            width: pageScrollView.availableWidth

        HScrollRow {
            Layout.fillWidth: true
            margin: 0
            spacing: 8

            RagoBadge { text: analyzer.database + "." + analyzer.collection }
            Label {
                visible: !analyzer.busy && analyzer.error.length === 0
                text: qsTr("%n document(s) sampled, %1 field path(s)", "", analyzer.sampledDocuments)
                      .arg(analyzer.count)
                font.pixelSize: 12
                color: Theme.disabledTextColor
            }
            BusyIndicator {
                running: analyzer.busy
                visible: running
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
            }
            Item { Layout.fillWidth: true }
            Label { text: qsTr("Sample"); color: Theme.disabledTextColor; font.pixelSize: 12 }
            RagoSpinBox {
                from: 1
                to: 100000
                stepSize: 100
                value: analyzer.sampleSize
                Layout.preferredWidth: 130
                onValueModified: analyzer.sampleSize = value
            }
            RagoPillButton {
                text: qsTr("Analyze")
                primary: true
                enabled: !analyzer.busy
                onClicked: analyzer.analyze()
            }
        }

        Label {
            visible: analyzer.error.length > 0
            Layout.fillWidth: true
            text: analyzer.error
            wrapMode: Text.WordWrap
            color: Theme.dangerColor
        }

        // ---- Horizontally scrollable table on narrow phone screens ----------
        Flickable {
            id: tableScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: Math.max(width, 720)
            contentHeight: height
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.horizontal: ScrollBar {
                policy: ScrollBar.AsNeeded
                implicitHeight: 6
            }

            ColumnLayout {
                width: tableScroll.contentWidth
                height: tableScroll.height
                spacing: 6

                // ---- Column header -----------------------------------------
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    radius: 6
                    color: Theme.alternateColor

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10
                        Label { text: qsTr("Field"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 260 }
                        Label { text: qsTr("Presence"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 130 }
                        Label { text: qsTr("Types"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 240 }
                        Label { text: qsTr("Values / Range"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.fillWidth: true }
                    }
                }

                ListView {
                    id: fieldList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: analyzer

                    delegate: Rectangle {
                        id: row
                        required property int index
                        required property string path
                        required property string name
                        required property int depth
                        required property real presence
                        required property var presentCount
                        required property string typesText
                        required property string itemTypesText
                        required property string valuesText
                        required property string minValue
                        required property string maxValue

                        width: fieldList.width
                        implicitHeight: Math.max(34, rowLayout.implicitHeight + 12)
                        color: rowHover.hovered ? Theme.hoverColor
                                                : (index % 2 === 1 ? Theme.alternateColor : "transparent")

                        HoverHandler { id: rowHover }

                        RowLayout {
                            id: rowLayout
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 10

                            // Field name, indented by nesting depth; click copies the path.
                            Label {
                                Layout.preferredWidth: 260
                                leftPadding: row.depth * 16
                                text: (row.depth > 0 ? "└ " : "") + row.name
                                font.family: AppSettings.codeFontFamily
                                font.pixelSize: AppSettings.codeFontSize
                                font.bold: row.depth === 0
                                elide: Text.ElideRight
                                color: Theme.textColor
                                ToolTip.visible: nameHover.hovered
                                ToolTip.text: qsTr("%1  —  click to copy").arg(Language.isolate(row.path))
                                HoverHandler { id: nameHover }
                                TapHandler { onTapped: Backend.copyToClipboard(row.path) }
                            }

                            RowLayout {
                                Layout.preferredWidth: 130
                                spacing: 6
                                Rectangle {
                                    Layout.preferredWidth: 70
                                    Layout.preferredHeight: 6
                                    radius: 3
                                    color: Theme.alternateColor
                                    Rectangle {
                                        width: parent.width * row.presence
                                        height: parent.height
                                        radius: 3
                                        color: row.presence >= 0.999 ? Theme.statusOnline : Theme.accentColor
                                    }
                                }
                                Label {
                                    text: Math.round(row.presence * 100) + "%"
                                    font.pixelSize: 12
                                    color: Theme.textColor
                                }
                            }

                            Label {
                                Layout.preferredWidth: 240
                                text: row.itemTypesText.length > 0
                                      ? row.typesText + "\n" + qsTr("items: %1").arg(row.itemTypesText)
                                      : row.typesText
                                wrapMode: Text.WordWrap
                                font.pixelSize: 12
                                color: Theme.textColor
                            }

                            Label {
                                Layout.fillWidth: true
                                text: row.minValue.length > 0
                                      ? qsTr("min %1   max %2").arg(row.minValue).arg(row.maxValue)
                                      : row.valuesText
                                elide: Text.ElideRight
                                font.family: AppSettings.codeFontFamily
                                font.pixelSize: AppSettings.codeFontSize
                                color: Theme.disabledTextColor
                                ToolTip.visible: valueHover.hovered && truncated
                                ToolTip.text: text
                                HoverHandler { id: valueHover }
                            }
                        }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
            }
        }
    }
    }
}
