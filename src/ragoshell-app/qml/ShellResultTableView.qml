pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

Rectangle {
    id: tableContainer
    required property var page

    function forceTableLayout() {
        tableView.forceLayout()
        tableHeader.forceLayout()
    }
    color: Theme.baseColor
    focus: true

    Keys.onPressed: function(event) {
        var ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        var shift = (event.modifiers & Qt.ShiftModifier) !== 0
        var alt = (event.modifiers & Qt.AltModifier) !== 0
        if (event.key === Qt.Key_Delete ||
            (tableContainer.page.isMac && ctrl && event.key === Qt.Key_Backspace)) {
            tableContainer.page.triggerDeleteSelectedDocuments()
            event.accepted = true
        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) &&
                   tableContainer.page._editingDocIndex < 0) {
            if (tableContainer.page.shell && tableContainer.page.shell.canModifyDocuments)
                tableContainer.page.triggerEditSelectedDocument()
            else
                tableContainer.page.triggerViewSelectedDocument()
            event.accepted = true
        } else if (ctrl && shift && event.key === Qt.Key_C) {
            tableContainer.page.copySelectedValueOrDoc(true)
            event.accepted = true
        } else if (ctrl && !shift && event.key === Qt.Key_C) {
            tableContainer.page.copySelectedValueOrDoc(false)
            event.accepted = true
        } else if ((ctrl && event.key === Qt.Key_I) || event.key === Qt.Key_Insert) {
            tableContainer.page.triggerInsertDocument()
            event.accepted = true
        } else if (ctrl && !shift && !alt && event.key === Qt.Key_A && tableContainer.page._editingDocIndex < 0) {
            tableContainer.page.selectAllDocuments()
            event.accepted = true
        } else if (!ctrl && !alt && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)
                   && tableContainer.page._editingDocIndex < 0) {
            var nextRow = tableContainer.page.stepDocumentSelection(event.key === Qt.Key_Down ? 1 : -1, shift)
            if (nextRow >= 0)
                tableView.positionViewAtRow(nextRow, TableView.Contain)
            event.accepted = true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        HorizontalHeaderView {
            id: tableHeader
            Layout.fillWidth: true
            syncView: tableView
            clip: true

            delegate: Rectangle {
                id: headerCell
                required property var display
                implicitWidth: 160
                implicitHeight: Math.max(28, AppSettings.codeFontSize + 14)
                color: Theme.surfaceColor
                border.width: 1
                border.color: Theme.glassBorder

                Label {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    verticalAlignment: Text.AlignVCenter
                    text: headerCell.display !== undefined ? String(headerCell.display) : ""
                    textFormat: Text.PlainText
                    font.family: AppSettings.codeFontFamily
                    font.bold: true
                    font.pixelSize: Math.max(11, AppSettings.codeFontSize - 1)
                    color: Theme.disabledTextColor
                    elide: Text.ElideRight
                }
            }
        }

        TableView {
            id: tableView
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: tableContainer.page.shell ? tableContainer.page.shell.tableModel : null
            clip: true
            pointerNavigationEnabled: false
            resizableColumns: true

            columnWidthProvider: function(column) {
                var w = tableView.explicitColumnWidth(column)
                if (w >= 0)
                    return w
                return 180
            }

            delegate: Rectangle {
                id: tableCell
                property bool _longPressTriggered: false
                required property int row
                required property int column
                required property var model
                implicitWidth: 180
                implicitHeight: Math.max(28, AppSettings.codeFontSize + 15)

                readonly property int docIdx: (model && model.docIndex !== undefined) ? model.docIndex : row
                readonly property bool docSelected: tableContainer.page.selectedDocIndices.indexOf(docIdx) !== -1
                                                    && docIdx >= 0
                readonly property string fPath: (model && model.fieldPath !== undefined) ? model.fieldPath : ""
                readonly property string rawVal: (model && model.rawValue !== undefined) ? model.rawValue : ""
                readonly property string tName: (model && model.typeName !== undefined) ? model.typeName : ""
                readonly property bool canInlineEdit: (model && model.editableScalar === true) &&
                                                      (tableContainer.page.shell ? tableContainer.page.shell.canModifyDocuments : false)
                readonly property bool isEditingThisCell: tableContainer.page._editingDocIndex === docIdx &&
                                                          tableContainer.page._editingFieldPath === fPath &&
                                                          fPath.length > 0

                color: tableCell.docSelected
                       ? Theme.selectedColor
                       : (row % 2 === 1 ? Theme.alternateColor : Theme.baseColor)
                border.width: 1
                border.color: Theme.glassBorder

                Text {
                    visible: !tableCell.isEditingThisCell
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    verticalAlignment: Text.AlignVCenter
                    text: tableCell.model && tableCell.model.display !== undefined ? tableCell.model.display : ""
                    textFormat: Text.PlainText
                    color: Theme.textColor
                    font.family: AppSettings.codeFontFamily
                    font.pixelSize: AppSettings.codeFontSize
                    elide: Text.ElideRight
                }

                RagoTextField {
                    visible: tableCell.isEditingThisCell
                    anchors.fill: parent
                    anchors.margins: 2
                    font.family: AppSettings.codeFontFamily
                    font.pixelSize: AppSettings.codeFontSize
                    text: tableContainer.page._editingInitialValue
                    onVisibleChanged: {
                        if (visible) {
                            text = tableContainer.page._editingInitialValue
                            forceActiveFocus()
                            selectAll()
                        }
                    }
                    onAccepted: tableContainer.page.commitInPlaceEdit(text)
                    Keys.onEscapePressed: tableContainer.page.cancelInPlaceEdit()
                }

                MouseArea {
                    anchors.fill: parent
                    visible: !tableCell.isEditingThisCell
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    pressAndHoldInterval: 800
                    onPressed: function(mouse) {
                        tableCell._longPressTriggered = false
                        tableContainer.forceActiveFocus()
                        if (mouse.button === Qt.LeftButton) {
                            tableContainer.page.selectDocumentWithModifiers(tableCell.docIdx, mouse.modifiers)
                        }
                        var k = tableCell.model ? tableCell.model.key : ""
                        tableContainer.page.populateContextFromCell(
                            tableCell.docIdx, -1, k,
                            tableCell.rawVal, tableCell.fPath,
                            tableCell.tName, false,
                            mouse.button !== Qt.LeftButton)
                    }
                    onPressAndHold: function(mouse) {
                        if (mouse.source === Qt.MouseEventNotSynthesized && !Platform.isMobile)
                            return
                        tableCell._longPressTriggered = true
                        var k = tableCell.model ? tableCell.model.key : ""
                        tableContainer.page.populateContextFromCell(
                            tableCell.docIdx, -1, k,
                            tableCell.rawVal, tableCell.fPath,
                            tableCell.tName, false)
                        tableContainer.page.openResultMenuAt(tableCell, mouse.x, mouse.y)
                    }
                    onClicked: function(mouse) {
                        if (tableCell._longPressTriggered) {
                            tableCell._longPressTriggered = false
                            return
                        }
                        if (mouse.button === Qt.RightButton) {
                            var k = tableCell.model ? tableCell.model.key : ""
                            tableContainer.page.populateContextFromCell(
                                tableCell.docIdx, -1, k,
                                tableCell.rawVal, tableCell.fPath,
                                tableCell.tName, false)
                            tableContainer.page.openResultMenuAt(tableCell, mouse.x, mouse.y)
                        }
                    }
                    onDoubleClicked: function(mouse) {
                        if (mouse.button !== Qt.LeftButton)
                            return
                        if (tableCell.canInlineEdit) {
                            tableContainer.page.startInPlaceEdit(tableCell.docIdx, tableCell.fPath,
                                                  tableCell.tName, tableCell.rawVal)
                        } else if (tableCell.docIdx >= 0) {
                            if (tableContainer.page.shell && tableContainer.page.shell.canModifyDocuments)
                                tableContainer.page.triggerEditSelectedDocument()
                            else
                                tableContainer.page.triggerViewSelectedDocument()
                        }
                    }
                }
            }

            ScrollBar.vertical: ScrollBar {}
            ScrollBar.horizontal: ScrollBar {}
        }
    }

    Label {
        anchors.centerIn: parent
        visible: tableContainer.page.shell ? (!tableContainer.page.shell.busy && !tableContainer.page.shell.cancelled && tableContainer.page.shell.resultCount === 0 && tableContainer.page.shell.errorText.length === 0) : true
        text: qsTr("No tabular rows to display.")
        font.pixelSize: 12
        color: Theme.disabledTextColor
    }
}
