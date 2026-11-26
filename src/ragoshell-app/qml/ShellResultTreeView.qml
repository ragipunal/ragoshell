pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

Rectangle {
    id: treeContainer
    required property var page
    property bool _treeLayoutQueued: false

    function forceTreeLayout() {
        treeView.forceLayout()
    }

    function expandFirstRow() {
        if (treeView.rows > 0)
            treeView.expand(0)
    }

    function expandSubtree(row, expand) {
        if (row < 0 || row >= treeView.rows)
            return
        if (expand) {
            if (typeof treeView.expandRecursively === "function")
                treeView.expandRecursively(row, -1)
            else
                treeView.expand(row)
        } else {
            if (typeof treeView.collapseRecursively === "function")
                treeView.collapseRecursively(row)
            else
                treeView.collapse(row)
        }
    }
    color: Theme.baseColor
    focus: true

    property real col0Ratio: 0.38
    property real col1Ratio: 0.45
    // Below 540 dp (a phone, a tablet in portrait with the Explorer open) the three columns share the
    // width with smaller minimums instead of pushing the Type column past the edge.
    readonly property bool narrow: treeView.width < 540
    readonly property int totalTreeWidth: narrow ? Math.max(320, treeView.width) : Math.max(540, treeView.width)
    readonly property int minKey: narrow ? 110 : 160
    readonly property int minType: narrow ? 72 : 96
    readonly property int col0Width: Math.max(minKey, Math.floor(totalTreeWidth * col0Ratio))
    readonly property int col1Width: Math.max(minKey, Math.min(Math.floor(totalTreeWidth * col1Ratio),
                                                                totalTreeWidth - col0Width - minType))
    readonly property int col2Width: Math.max(minType, totalTreeWidth - col0Width - col1Width)
    function colWidth(col) {
        return col === 0 ? col0Width : (col === 1 ? col1Width : col2Width)
    }

    Keys.onPressed: function(event) {
        var ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        var shift = (event.modifiers & Qt.ShiftModifier) !== 0
        var alt = (event.modifiers & Qt.AltModifier) !== 0

        if (event.key === Qt.Key_Delete ||
            (treeContainer.page.isMac && ctrl && event.key === Qt.Key_Backspace)) {
            treeContainer.page.triggerDeleteSelectedDocuments()
            event.accepted = true
        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) &&
                   treeContainer.page._editingDocIndex < 0) {
            if (treeContainer.page.shell && treeContainer.page.shell.canModifyDocuments)
                treeContainer.page.triggerEditSelectedDocument()
            else
                treeContainer.page.triggerViewSelectedDocument()
            event.accepted = true
        } else if (ctrl && shift && event.key === Qt.Key_C) {
            treeContainer.page.copySelectedValueOrDoc(true)
            event.accepted = true
        } else if (ctrl && !shift && event.key === Qt.Key_C) {
            treeContainer.page.copySelectedValueOrDoc(false)
            event.accepted = true
        } else if ((ctrl && event.key === Qt.Key_I) || event.key === Qt.Key_Insert) {
            treeContainer.page.triggerInsertDocument()
            event.accepted = true
        } else if (alt && event.key === Qt.Key_Right && treeContainer.page._activeTreeRow >= 0) {
            treeContainer.page.expandTreeSubtree(treeContainer.page._activeTreeRow, true)
            event.accepted = true
        } else if (alt && event.key === Qt.Key_Left && treeContainer.page._activeTreeRow >= 0) {
            treeContainer.page.expandTreeSubtree(treeContainer.page._activeTreeRow, false)
            event.accepted = true
        } else if (ctrl && !shift && !alt && event.key === Qt.Key_A && treeContainer.page._editingDocIndex < 0) {
            treeContainer.page.selectAllDocuments()
            event.accepted = true
        } else if (!ctrl && !alt && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)
                   && treeContainer.page._editingDocIndex < 0) {
            var nextDoc = treeContainer.page.stepDocumentSelection(event.key === Qt.Key_Down ? 1 : -1, shift)
            if (nextDoc >= 0 && treeView.model) {
                var topIdx = treeView.model.index(nextDoc, 0)
                var visRow = treeView.rowAtIndex(topIdx)
                if (visRow >= 0) {
                    treeContainer.page._activeTreeRow = visRow
                    treeView.positionViewAtRow(visRow, TableView.Contain)
                }
            }
            event.accepted = true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Resizable Tree Column Header Bar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: Theme.surfaceColor

            Rectangle {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: Theme.glassBorder
            }

            Row {
                anchors.fill: parent

                Item {
                    width: treeContainer.col0Width
                    height: parent.height
                    Label {
                        anchors.fill: parent
                        leftPadding: 12
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("Key")
                        font.bold: true
                        font.pixelSize: 11
                        color: Theme.disabledTextColor
                    }
                    Rectangle {
                        anchors.right: parent.right
                        width: 4
                        height: parent.height
                        color: col0Drag.containsMouse || col0Drag.pressed
                               ? Theme.accentColor : Theme.glassBorder
                        MouseArea {
                            id: col0Drag
                            anchors.fill: parent
                            anchors.leftMargin: -3
                            anchors.rightMargin: -3
                            hoverEnabled: true
                            cursorShape: Qt.SplitHCursor
                            property real startX: 0
                            property real startRatio: 0
                            onPressed: function(mouse) {
                                startX = mapToItem(treeContainer, mouse.x, 0).x
                                startRatio = treeContainer.col0Ratio
                            }
                            onPositionChanged: function(mouse) {
                                if (!pressed) return
                                var curX = mapToItem(treeContainer, mouse.x, 0).x
                                var delta = (curX - startX) / Math.max(1, treeContainer.totalTreeWidth)
                                treeContainer.col0Ratio = Math.max(0.18, Math.min(0.65, startRatio + delta))
                                treeView.forceLayout()
                            }
                        }
                    }
                }

                Item {
                    width: treeContainer.col1Width
                    height: parent.height
                    Label {
                        anchors.fill: parent
                        leftPadding: 8
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("Value")
                        font.bold: true
                        font.pixelSize: 11
                        color: Theme.disabledTextColor
                    }
                    Rectangle {
                        anchors.right: parent.right
                        width: 4
                        height: parent.height
                        color: col1Drag.containsMouse || col1Drag.pressed
                               ? Theme.accentColor : Theme.glassBorder
                        MouseArea {
                            id: col1Drag
                            anchors.fill: parent
                            anchors.leftMargin: -3
                            anchors.rightMargin: -3
                            hoverEnabled: true
                            cursorShape: Qt.SplitHCursor
                            property real startX: 0
                            property real startRatio: 0
                            onPressed: function(mouse) {
                                startX = mapToItem(treeContainer, mouse.x, 0).x
                                startRatio = treeContainer.col1Ratio
                            }
                            onPositionChanged: function(mouse) {
                                if (!pressed) return
                                var curX = mapToItem(treeContainer, mouse.x, 0).x
                                var delta = (curX - startX) / Math.max(1, treeContainer.totalTreeWidth)
                                var maxR = 0.84 - treeContainer.col0Ratio
                                treeContainer.col1Ratio = Math.max(0.18, Math.min(maxR, startRatio + delta))
                                treeView.forceLayout()
                            }
                        }
                    }
                }

                Label {
                    width: treeContainer.col2Width
                    height: parent.height
                    leftPadding: 8
                    verticalAlignment: Text.AlignVCenter
                    //: Column header: BSON type of a field (String, Int32, ObjectId ...).
                    text: qsTr("Type")
                    font.bold: true
                    font.pixelSize: 11
                    color: Theme.disabledTextColor
                }
            }
        }

        TreeView {
            id: treeView
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: treeContainer.page.shell ? treeContainer.page.shell.treeModel : null
            clip: true
            pointerNavigationEnabled: false
            selectionModel: ItemSelectionModel {}
            onWidthChanged: {
                if (!treeContainer._treeLayoutQueued) {
                    treeContainer._treeLayoutQueued = true
                    Qt.callLater(function() {
                        treeContainer._treeLayoutQueued = false
                        treeView.forceLayout()
                    })
                }
            }

            columnWidthProvider: function(column) {
                return treeContainer.colWidth(column)
            }

            delegate: Item {
                id: treeDelegate
                property bool _longPressTriggered: false
                implicitWidth: treeContainer.colWidth(column)
                implicitHeight: Math.max(28, AppSettings.codeFontSize + 15)

                readonly property real indentation: 18
                readonly property real padding: 10
                required property TreeView treeView
                required property bool isTreeNode
                required property bool expanded
                required property bool hasChildren
                required property int depth
                required property int row
                required property int column
                required property bool current
                required property var model

                readonly property int docIdx: (model && model.docIndex !== undefined) ? model.docIndex : -1
                readonly property bool docSelected: treeContainer.page.selectedDocIndices.indexOf(docIdx) !== -1
                                                    && docIdx >= 0
                readonly property string fPath: (model && model.fieldPath !== undefined) ? model.fieldPath : ""
                readonly property string rawVal: (model && model.rawValue !== undefined) ? model.rawValue : ""
                readonly property string tName: (model && model.typeName !== undefined) ? model.typeName : ""
                readonly property bool canInlineEdit: (model && model.editableScalar === true) &&
                                                      (treeContainer.page.shell ? treeContainer.page.shell.canModifyDocuments : false)
                readonly property bool isEditingThisCell: treeDelegate.column === 1 &&
                                                          treeContainer.page._editingDocIndex === docIdx &&
                                                          treeContainer.page._editingFieldPath === fPath &&
                                                          fPath.length > 0

                Rectangle {
                    anchors.fill: parent
                    color: treeDelegate.docSelected
                           ? (treeDelegate.depth === 0
                              ? Theme.selectedColor
                              : Qt.rgba(Theme.accentColor.r,
                                        Theme.accentColor.g,
                                        Theme.accentColor.b,
                                        Theme.dark ? 0.06 : 0.045))
                           : (treeDelegate.row % 2 === 1 ? Theme.alternateColor : "transparent")
                }

                Rectangle {
                    visible: treeDelegate.column === 0 && treeDelegate.docSelected
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 3
                    color: Theme.accentColor
                }

                // Column 0: key with expand arrow
                Row {
                    id: keyRow
                    visible: treeDelegate.column === 0
                    x: treeDelegate.padding + treeDelegate.depth * treeDelegate.indentation
                    spacing: 6
                    anchors.verticalCenter: parent.verticalCenter

                    Text {
                        id: expandArrow
                        text: treeDelegate.hasChildren
                              ? (treeDelegate.expanded ? "▾" : (Language.rtl ? "◂" : "▸")) : ""
                        color: Theme.disabledTextColor
                        font.pixelSize: 11
                        width: 12
                    }
                    Text {
                        text: treeDelegate.model && treeDelegate.model.key !== undefined
                              ? treeDelegate.model.key : ""
                        textFormat: Text.PlainText
                        color: Theme.textColor
                        font.family: AppSettings.codeFontFamily
                        font.pixelSize: AppSettings.codeFontSize
                        font.bold: treeDelegate.depth === 0
                        // Keep long keys inside the column so they never run into the value cell.
                        elide: Text.ElideRight
                        width: Math.max(0, treeDelegate.width - keyRow.x - expandArrow.width
                                           - keyRow.spacing - treeDelegate.padding)
                    }
                }

                // Column 1: Value (or inline editor when double-clicked)
                Text {
                    visible: treeDelegate.column === 1 && !treeDelegate.isEditingThisCell
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: treeDelegate.model && treeDelegate.model.value !== undefined
                          ? treeDelegate.model.value : ""
                    textFormat: Text.PlainText
                    color: Theme.textColor
                    font.family: AppSettings.codeFontFamily
                    font.pixelSize: AppSettings.codeFontSize
                    elide: Text.ElideRight
                    width: Math.max(40, treeContainer.col1Width - 16)
                }

                RagoTextField {
                    id: treeInlineEditor
                    visible: treeDelegate.isEditingThisCell
                    anchors.fill: parent
                    anchors.margins: 2
                    font.family: AppSettings.codeFontFamily
                    font.pixelSize: AppSettings.codeFontSize
                    text: treeContainer.page._editingInitialValue
                    onVisibleChanged: {
                        if (visible) {
                            text = treeContainer.page._editingInitialValue
                            forceActiveFocus()
                            selectAll()
                        }
                    }
                    onAccepted: treeContainer.page.commitInPlaceEdit(text)
                    Keys.onEscapePressed: treeContainer.page.cancelInPlaceEdit()
                }

                // Column 2: Type badge
                RagoBadge {
                    visible: treeDelegate.column === 2 && treeDelegate.tName.length > 0
                    x: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: treeDelegate.tName
                    badgeColor: treeDelegate.tName === "ObjectId" ? Theme.accentColor :
                                treeDelegate.tName === "String" ? "#3b9eff" :
                                treeDelegate.tName === "Int32" || treeDelegate.tName === "Int64" ||
                                treeDelegate.tName === "Double" || treeDelegate.tName === "Decimal128" ? "#d49a35" :
                                treeDelegate.tName === "Boolean" ? "#b462d1" :
                                treeDelegate.tName === "Date" || treeDelegate.tName === "Timestamp" ? "#e06c75" :
                                Theme.disabledTextColor
                }

                MouseArea {
                    anchors.fill: parent
                    visible: !treeDelegate.isEditingThisCell
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    pressAndHoldInterval: 800
                    onPressed: function(mouse) {
                        treeDelegate._longPressTriggered = false
                        treeContainer.forceActiveFocus()
                        treeContainer.page._activeTreeRow = treeDelegate.row
                        if (mouse.button === Qt.LeftButton) {
                            treeContainer.page.selectDocumentWithModifiers(treeDelegate.docIdx, mouse.modifiers)
                        }
                        var k = treeDelegate.model ? treeDelegate.model.key : ""
                        treeContainer.page.populateContextFromCell(
                            treeDelegate.docIdx, treeDelegate.row, k,
                            treeDelegate.rawVal, treeDelegate.fPath,
                            treeDelegate.tName, treeDelegate.hasChildren,
                            mouse.button !== Qt.LeftButton)
                    }
                    onPressAndHold: function(mouse) {
                        if (mouse.source === Qt.MouseEventNotSynthesized && !Platform.isMobile)
                            return
                        treeDelegate._longPressTriggered = true
                        var k = treeDelegate.model ? treeDelegate.model.key : ""
                        treeContainer.page.populateContextFromCell(
                            treeDelegate.docIdx, treeDelegate.row, k,
                            treeDelegate.rawVal, treeDelegate.fPath,
                            treeDelegate.tName, treeDelegate.hasChildren)
                        treeContainer.page.openResultMenuAt(treeDelegate, mouse.x, mouse.y)
                    }
                    onClicked: function(mouse) {
                        if (treeDelegate._longPressTriggered) {
                            treeDelegate._longPressTriggered = false
                            return
                        }
                        if (mouse.button === Qt.RightButton) {
                            var k = treeDelegate.model ? treeDelegate.model.key : ""
                            treeContainer.page.populateContextFromCell(
                                treeDelegate.docIdx, treeDelegate.row, k,
                                treeDelegate.rawVal, treeDelegate.fPath,
                                treeDelegate.tName, treeDelegate.hasChildren)
                            treeContainer.page.openResultMenuAt(treeDelegate, mouse.x, mouse.y)
                            return
                        }
                        if (mouse.button === Qt.LeftButton &&
                            (mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)) === 0) {
                            var arrowBound = treeDelegate.padding + treeDelegate.depth * treeDelegate.indentation + 24
                            if (treeDelegate.column === 0 && treeDelegate.hasChildren &&
                                (mouse.x < arrowBound || Platform.isMobile)) {
                                treeView.toggleExpanded(treeDelegate.row)
                            }
                        }
                    }
                    onDoubleClicked: function(mouse) {
                        if (mouse.button !== Qt.LeftButton)
                            return
                        if (treeDelegate.hasChildren) {
                            treeView.toggleExpanded(treeDelegate.row)
                        } else if (treeDelegate.column === 1 && treeDelegate.canInlineEdit) {
                            treeContainer.page.startInPlaceEdit(treeDelegate.docIdx, treeDelegate.fPath,
                                                  treeDelegate.tName, treeDelegate.rawVal)
                        } else if (treeDelegate.docIdx >= 0) {
                            if (treeContainer.page.shell && treeContainer.page.shell.canModifyDocuments)
                                treeContainer.page.triggerEditSelectedDocument()
                            else
                                treeContainer.page.triggerViewSelectedDocument()
                        }
                    }
                }
            }

            ScrollBar.vertical: ScrollBar {}
            ScrollBar.horizontal: ScrollBar {}
        }
    }

    // Background right-click when collection is empty (to allow Insert Document)
    MouseArea {
        anchors.fill: parent
        z: -1
        acceptedButtons: Qt.RightButton
        onClicked: function(mouse) {
            treeContainer.page.populateContextFromCell(-1, -1, "", "", "", "", false)
            treeContainer.page.openResultMenuAt(treeContainer, mouse.x, mouse.y)
        }
    }

    Label {
        anchors.centerIn: parent
        visible: treeContainer.page.shell ? (!treeContainer.page.shell.busy && !treeContainer.page.shell.cancelled && treeContainer.page.shell.resultCount === 0 && treeContainer.page.shell.errorText.length === 0) : true
        text: Platform.shortcutsVisible ? qsTr("No documents to display. Press F5 or click Execute to run the query.") : qsTr("No documents to display. Run the query with Execute.")
        font.pixelSize: 12
        color: Theme.disabledTextColor
    }
}
