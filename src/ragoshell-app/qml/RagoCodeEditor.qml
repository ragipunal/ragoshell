import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

// Mobile-ready & cross-platform code/JSON editor abstraction.
// On desktop builds it hosts ScintillaEditor with JS/BSON highlighting, brace
// matching, folding, inline Find bar (Ctrl+F / Cmd+F), and touch/mouse context menu.
Item {
    id: root

    // Code and JSON stay left to right in a right-to-left UI.
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true

    property alias text: sci.text
    readonly property alias textLength: sci.textLength
    readonly property alias modified: sci.modified
    readonly property alias selectedText: sci.selectedText
    readonly property alias hasSelection: sci.hasSelection
    readonly property alias caretPosition: sci.caretPosition
    property alias readOnly: sci.readOnly
    property alias lineNumbers: sci.lineNumbers
    property alias showLineNumbers: sci.lineNumbers
    property alias folding: sci.folding
    property alias overlayActive: sci.overlayActive
    /// An input method (IME) composition is shown inline, not yet committed.
    readonly property alias composing: sci.composing
    /// The Scintilla surface itself has keyboard focus (this root is a plain Item).
    readonly property bool editorHasFocus: sci.activeFocus
    property bool framed: true
    property bool showFormatAction: false
    property bool findBarVisible: false
    property bool _findNoMatch: false


    signal autocompleteRequested()
    signal executeRequested()
    signal formatRequested()
    signal charAdded(int ch)
    signal overlayKeyPressed(int key)
    signal editorKeyPressed(var event)

    function send(msg, wParam, lParam) { return sci.send(msg, wParam || 0, lParam || 0) }
    function pointFromPosition(pos) { return sci.pointFromPosition(pos) }
    function lineHeight() { return sci.lineHeight() }
    function shellCallTip() { return sci.shellCallTip() }
    function textBeforeCaret(maxChars) { return sci.textBeforeCaret(maxChars) }
    function textAfterCaret(maxChars) { return sci.textAfterCaret(maxChars) }
    function pointBeforeCaret(utf16CharsBack) { return sci.pointBeforeCaret(utf16CharsBack) }
    function replaceBeforeCaret(utf16CharsBack, insertText, caretOffsetFromEnd) {
        sci.replaceBeforeCaret(utf16CharsBack, insertText, caretOffsetFromEnd || 0)
    }
    function focusEditor() { sci.forceActiveFocus() }
    function forceEditorFocus() { sci.forceActiveFocus() }
    function setText(t) { sci.text = t }
    function setSavePoint() { sci.setSavePoint() }
    function setCaretPosition(utf16Offset) { sci.setCaretPositionUtf16(utf16Offset) }
    function replaceAllText(newText) { sci.replaceAllText(newText) }
    function toggleLineComment() { sci.toggleLineComment() }
    function foldAll(expand) { sci.foldAll(expand) }
    function copyAllOrSelection() {
        if (sci.hasSelection)
            sci.copy()
        else {
            sci.selectAll()
            sci.copy()
        }
    }

    function openFindBar() {
        findBarVisible = true
        if (sci.hasSelection && sci.selectedText.indexOf("\n") === -1)
            findInput.text = sci.selectedText
        Qt.callLater(function() {
            findInput.forceActiveFocus()
            findInput.selectAll()
        })
    }

    function closeFindBar() {
        findBarVisible = false
        _findNoMatch = false
        sci.forceActiveFocus()
    }

    function runFind(forward) {
        if (findInput.text.length === 0) {
            _findNoMatch = false
            return
        }
        _findNoMatch = !sci.findText(findInput.text, forward, matchCaseBtn.checked)
    }

    Rectangle {
        anchors.fill: parent
        radius: root.framed ? 5 : 0
        color: Theme.dark ? "#0f1612" : "#ffffff"
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: root.framed ? 1 : 0
            spacing: 0

            // Collapsible Find Bar (Ctrl+F / Cmd+F)
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                visible: root.findBarVisible
                color: Theme.surfaceColor
                border.width: 1
                border.color: Theme.glassBorder

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 6

                    Label {
                        text: qsTr("Find:")
                        font.pixelSize: 11
                        color: Theme.disabledTextColor
                    }

                    RagoTextField {
                        id: findInput
                        Layout.fillWidth: true
                        Layout.minimumWidth: 64
                        Layout.maximumWidth: 260
                        implicitHeight: 26
                        font.family: AppSettings.codeFontFamily
                        font.pixelSize: Math.max(12, Math.min(16, AppSettings.codeFontSize - 1))
                        placeholderText: qsTr("Search text…")
                        onTextChanged: root.runFind(true)
                        Keys.onPressed: function(event) {
                            if (event.key === Qt.Key_Escape) {
                                root.closeFindBar()
                                event.accepted = true
                            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                                root.runFind((event.modifiers & Qt.ShiftModifier) === 0)
                                event.accepted = true
                            } else if (event.key === Qt.Key_F3) {
                                root.runFind((event.modifiers & Qt.ShiftModifier) === 0)
                                event.accepted = true
                            }
                        }
                    }

                    Label {
                        visible: root._findNoMatch && findInput.text.length > 0
                        text: qsTr("No match")
                        font.pixelSize: 11
                        color: "#ff6b6b"
                    }

                    RagoPillButton {
                        id: matchCaseBtn
                        text: "Aa"
                        implicitHeight: 26
                        checked: false
                        onClicked: {
                            checked = !checked
                            root.runFind(true)
                        }
                    }

                    RagoIconButton {
                        glyph: "▲"
                        size: 26
                        toolTipText: Platform.withShortcut(qsTr("Previous Match (%1)"), "Shift+F3")
                        onClicked: root.runFind(false)
                    }

                    RagoIconButton {
                        glyph: "▼"
                        size: 26
                        toolTipText: Platform.withShortcut(qsTr("Next Match (%1)"), "F3")
                        onClicked: root.runFind(true)
                    }

                    Item { Layout.fillWidth: true }

                    RagoIconButton {
                        glyph: "✕"
                        size: 24
                        toolTipText: Platform.withShortcut(qsTr("Close Find Bar (%1)"), "Esc")
                        onClicked: root.closeFindBar()
                    }
                }
            }

            // Editor surface + vertical / horizontal scrollbars
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ScintillaEditor {
                    id: sci
                    anchors.fill: parent
                    // Scintilla does not wrap lines, so width changes never toggle vertical scrollMaximum.
                    anchors.rightMargin: vScroll.visible ? vScroll.width : 0
                    anchors.bottomMargin: hScroll.visible ? hScroll.height : 0
                    darkMode: Theme.dark
                    fontFamily: AppSettings.codeFontFamily
                    fontSize: AppSettings.codeFontSize
                    onAutocompleteRequested: root.autocompleteRequested()
                    onExecuteRequested: root.executeRequested()
                    onFindRequested: root.openFindBar()
                    onFormatRequested: root.formatRequested()
                    onCharAdded: function(ch) { root.charAdded(ch) }
                    onOverlayKeyPressed: function(key) { root.overlayKeyPressed(key) }
                    onContextMenuRequested: function(mx, my) {
                        ctxMenu.x = Math.min(mx, Math.max(0, width - ctxMenu.width - 8))
                        ctxMenu.y = Math.min(my, Math.max(0, height - 180))
                        ctxMenu.open()
                    }
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_F3 && root.findBarVisible) {
                            root.runFind((event.modifiers & Qt.ShiftModifier) === 0)
                            event.accepted = true
                            return
                        }
                        if (event.key === Qt.Key_Escape && root.findBarVisible && !root.overlayActive) {
                            root.closeFindBar()
                            event.accepted = true
                            return
                        }
                        root.editorKeyPressed(event)
                    }

                    // Touch long-press support for mobile/tablet readiness
                    TapHandler {
                        acceptedDevices: PointerDevice.TouchScreen | PointerDevice.Stylus
                        acceptedButtons: Qt.LeftButton
                        longPressThreshold: 0.8
                        onLongPressed: {
                            ctxMenu.x = Math.min(point.position.x, Math.max(0, sci.width - 200))
                            ctxMenu.y = Math.min(point.position.y, Math.max(0, sci.height - 180))
                            ctxMenu.open()
                        }
                    }
                }

                ScrollBar {
                    id: vScroll
                    readonly property real _range: sci.scrollPage + sci.scrollMaximum
                    readonly property real _thumbSize: _range > 0
                                                       ? Math.max(0.06, Math.min(1.0, sci.scrollPage / _range))
                                                       : 1.0
                    readonly property real _targetPos: sci.scrollMaximum > 0
                                                       ? (sci.scrollValue / sci.scrollMaximum) * (1.0 - _thumbSize)
                                                       : 0.0
                    visible: sci.scrollMaximum > 0
                    policy: ScrollBar.AlwaysOn
                    interactive: true
                    implicitWidth: Theme.touch ? 15 : 11
                    padding: 2
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: hScroll.visible ? hScroll.height : 0
                    anchors.right: parent.right
                    orientation: Qt.Vertical
                    size: _thumbSize
                    stepSize: sci.scrollMaximum > 0
                              ? Math.max((1.0 - _thumbSize) / sci.scrollMaximum, 0.01)
                              : 0.05

                    Binding {
                        target: vScroll
                        property: "position"
                        value: vScroll._targetPos
                        when: !vScroll.pressed
                        restoreMode: Binding.RestoreNone
                    }

                    onPositionChanged: {
                        if (pressed && sci.scrollMaximum > 0) {
                            var maxPos = Math.max(0.0001, 1.0 - size)
                            var ratio = Math.min(1.0, Math.max(0.0, position / maxPos))
                            sci.scrollTo(Math.round(ratio * sci.scrollMaximum))
                        }
                    }

                    background: Rectangle {
                        color: Theme.dark ? "#0c130f" : "#f3f6f4"
                        Rectangle {
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            anchors.left: parent.left
                            width: 1
                            color: Theme.dark ? "#1f2e25" : "#e1e8e4"
                        }
                    }

                    contentItem: Rectangle {
                        implicitWidth: Theme.touch ? 9 : 7
                        radius: width / 2
                        color: vScroll.pressed
                               ? Theme.accentColor
                               : (vScroll.hovered
                                  ? (Theme.dark ? "#6d9980" : "#768f82")
                                  : (Theme.dark ? "#476956" : "#a3b5ab"))
                    }
                }

                ScrollBar {
                    id: hScroll
                    readonly property real _range: sci.horizontalScrollPage + sci.horizontalScrollMaximum
                    readonly property real _thumbSize: _range > 0
                                                       ? Math.max(0.08, Math.min(1.0, sci.horizontalScrollPage / _range))
                                                       : 1.0
                    readonly property real _targetPos: sci.horizontalScrollMaximum > 0
                                                       ? (sci.horizontalScrollValue / sci.horizontalScrollMaximum) * (1.0 - _thumbSize)
                                                       : 0.0
                    visible: sci.horizontalScrollMaximum > 0
                    policy: ScrollBar.AlwaysOn
                    interactive: true
                    implicitHeight: Theme.touch ? 15 : 11
                    padding: 2
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.rightMargin: vScroll.visible ? vScroll.width : 0
                    anchors.bottom: parent.bottom
                    orientation: Qt.Horizontal
                    size: _thumbSize
                    stepSize: 0.05

                    Binding {
                        target: hScroll
                        property: "position"
                        value: hScroll._targetPos
                        when: !hScroll.pressed
                        restoreMode: Binding.RestoreNone
                    }

                    onPositionChanged: {
                        if (pressed && sci.horizontalScrollMaximum > 0) {
                            var maxPos = Math.max(0.0001, 1.0 - size)
                            var ratio = Math.min(1.0, Math.max(0.0, position / maxPos))
                            sci.scrollHorizontallyTo(Math.round(ratio * sci.horizontalScrollMaximum))
                        }
                    }

                    background: Rectangle {
                        color: Theme.dark ? "#0c130f" : "#f3f6f4"
                        Rectangle {
                            anchors.top: parent.top
                            anchors.left: parent.left
                            anchors.right: parent.right
                            height: 1
                            color: Theme.dark ? "#1f2e25" : "#e1e8e4"
                        }
                    }

                    contentItem: Rectangle {
                        implicitHeight: Theme.touch ? 9 : 7
                        radius: height / 2
                        color: hScroll.pressed
                               ? Theme.accentColor
                               : (hScroll.hovered
                                  ? (Theme.dark ? "#6d9980" : "#768f82")
                                  : (Theme.dark ? "#476956" : "#a3b5ab"))
                    }
                }

                Rectangle {
                    visible: vScroll.visible && hScroll.visible
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    width: vScroll.width
                    height: hScroll.height
                    color: Theme.dark ? "#0c130f" : "#f3f6f4"
                }

                RagoMenu {
                    id: ctxMenu
                    width: 226

                    RagoMenuItem {
                        text: qsTr("Undo")
                        keys: "Ctrl+Z"
                        enabled: !root.readOnly
                        visible: !root.readOnly
                        onTriggered: sci.undo()
                    }
                    RagoMenuItem {
                        text: qsTr("Redo")
                        keys: "Ctrl+Y"
                        enabled: !root.readOnly
                        visible: !root.readOnly
                        onTriggered: sci.redo()
                    }
                    RagoMenuSeparator {
                        visible: !root.readOnly
                    }
                    RagoMenuItem {
                        text: qsTr("Cut")
                        keys: "Ctrl+X"
                        enabled: !root.readOnly && root.hasSelection
                        visible: !root.readOnly
                        onTriggered: sci.cut()
                    }
                    RagoMenuItem {
                        text: qsTr("Copy")
                        keys: "Ctrl+C"
                        enabled: root.hasSelection || root.textLength > 0
                        onTriggered: root.copyAllOrSelection()
                    }
                    RagoMenuItem {
                        text: qsTr("Paste")
                        keys: "Ctrl+V"
                        enabled: !root.readOnly
                        visible: !root.readOnly
                        onTriggered: sci.paste()
                    }
                    RagoMenuItem {
                        text: qsTr("Select All")
                        keys: "Ctrl+A"
                        enabled: root.textLength > 0
                        onTriggered: sci.selectAll()
                    }
                    RagoMenuSeparator {}
                    RagoMenuItem {
                        text: qsTr("Find…")
                        keys: "Ctrl+F"
                        onTriggered: root.openFindBar()
                    }
                    RagoMenuItem {
                        text: qsTr("Toggle Comment")
                        keys: "Ctrl+/"
                        visible: !root.readOnly
                        onTriggered: sci.toggleLineComment()
                    }
                    RagoMenuItem {
                        text: qsTr("Format JSON / JS")
                        keys: "Alt+Shift+F"
                        visible: !root.readOnly && root.showFormatAction
                        onTriggered: root.formatRequested()
                    }
                    RagoMenuSeparator {
                        visible: root.folding
                    }
                    RagoMenuItem {
                        text: qsTr("Collapse Top Blocks")
                        visible: root.folding
                        onTriggered: sci.foldAll(false)
                    }
                    RagoMenuItem {
                        text: qsTr("Expand All Blocks")
                        visible: root.folding
                        onTriggered: sci.foldAll(true)
                    }
                }
            }
        }

        // Crisp top-layer frame border so Scintilla raster surface never paints over the border
        Rectangle {
            visible: root.framed
            z: 10
            anchors.fill: parent
            radius: 5
            color: "transparent"
            border.width: 1
            border.color: Theme.borderColor
        }
    }
}
