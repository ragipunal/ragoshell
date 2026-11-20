pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

Item {
    id: overlays
    anchors.fill: parent
    z: 10

    required property var page
    required property var editor

    readonly property bool popupVisible: autocompletePopup.visible
    readonly property bool callTipVisible: callTip.visible

    property int _acReplaceLen: 0
    property bool awaitingLive: false
    property bool lastExplicit: false
    property int _acDocLength: -1
    property int _acCaretPos: -1
    property int _acHandledCaretPos: -1

    function hidePopup() { autocompletePopup.visible = false }
    function hideCallTip() { callTip.visible = false }
    function hideAll() {
        callTip.visible = false
        autocompletePopup.visible = false
    }

    function updateCallTip() {
        if (overlays.editor.composing || autocompletePopup.visible) {
            callTip.visible = false
            return
        }
        const tip = overlays.editor.shellCallTip()
        if (!tip.name) {
            callTip.visible = false
            return
        }
        callTip.tipName = tip.name
        callTip.tipParams = tip.params
        callTip.tipActive = tip.active
        callTip.x = Math.max(4, Math.min(tip.x, overlays.editor.width - callTip.width - 8))
        const above = tip.y - callTip.height - 4
        callTip.y = above >= 4 ? above : tip.y + overlays.editor.lineHeight() + 4
        callTip.visible = true
    }

    function debugShowCallTip() {
        overlays.editor.focusEditor()
        overlays.editor.send(2318) // SCI_DOCUMENTEND
        overlays.updateCallTip()
    }

    function triggerAutocomplete(explicitTrigger, delimiterTrigger) {
        var sh = overlays.page.shell
        if (!sh || AppSettings.autocompletionMode === 0) {
            autocompletePopup.visible = false
            return
        }
        var exp = !!explicitTrigger
        var delim = !!delimiterTrigger
        lastExplicit = exp
        var res = sh.requestCompletion(overlays.editor.textBeforeCaret(8000), exp, delim)
        _acReplaceLen = res.replaceLength || 0
        awaitingLive = !!res.pending
        _acHandledCaretPos = overlays.editor.caretPosition
        if (!res.handled)
            autocompletePopup.visible = false
    }

    function showAutocomplete() {
        var sh = overlays.page.shell
        var items = sh ? sh.autocompleteList : []
        if (items.length === 0) {
            autocompletePopup.visible = false
            return
        }
        var p = overlays.editor.pointBeforeCaret(_acReplaceLen)
        autocompletePopup.x = Math.max(4, Math.min(p.x,
                                       overlays.editor.width - autocompletePopup.width - 12))
        var belowY = p.y + overlays.editor.lineHeight() + 4
        var rowH = Math.max(26, AppSettings.codeFontSize + 13)
        var popupH = Math.min(items.length * rowH + 8, 220)
        if (belowY + popupH > overlays.editor.height - 4 && p.y - popupH - 4 >= 4)
            autocompletePopup.y = p.y - popupH - 4
        else
            autocompletePopup.y = Math.max(4, Math.min(belowY, overlays.editor.height - popupH - 8))
        _acDocLength = overlays.editor.textLength
        _acCaretPos = overlays.editor.caretPosition
        acList.currentIndex = 0
        callTip.visible = false
        autocompletePopup.visible = true
    }

    function acceptAutocomplete() {
        var sh = overlays.page.shell
        if (!sh)
            return
        var items = sh.autocompleteList
        if (acList.currentIndex < 0 || acList.currentIndex >= items.length) {
            autocompletePopup.visible = false
            return
        }
        var inserts = sh.autocompleteInserts
        var ins = inserts.length === items.length ? inserts[acList.currentIndex] : items[acList.currentIndex]
        var replaceLen = _acReplaceLen
        autocompletePopup.visible = false
        awaitingLive = false

        if (overlays.editor.textAfterCaret(1) === "("
                && (ins.endsWith("(") || ins.endsWith(")"))
                && ins.indexOf("'") === -1 && ins.indexOf("\"") === -1) {
            var parenIdx = ins.indexOf("(")
            if (parenIdx > 0)
                ins = ins.substring(0, parenIdx)
        }

        var caretOffset = (ins.endsWith("({})") || ins.endsWith("([])")) ? -2 : 0
        overlays.editor.replaceBeforeCaret(replaceLen, ins, caretOffset)
        overlays.editor.focusEditor()

        if (ins === "use ")
            overlays.triggerAutocomplete(false, true)
        else if (ins.indexOf("(") !== -1)
            overlays.updateCallTip()
    }

    function handleCharAdded(ch) {
        if (ch === 46 || ch === 36) {
            if (_acHandledCaretPos !== overlays.editor.caretPosition || !autocompletePopup.visible)
                overlays.triggerAutocomplete(false, false)
        } else if ((ch === 123 || ch === 44 || ch === 34 || ch === 39 || ch === 32) && !autocompletePopup.visible) {
            overlays.triggerAutocomplete(false, true)
        }
        if (ch === 40 || ch === 44 || ch === 41)
            overlays.updateCallTip()
    }

    function handleCaretPositionChanged() {
        if (autocompletePopup.visible
                && overlays.editor.textLength === _acDocLength
                && overlays.editor.caretPosition !== _acCaretPos) {
            autocompletePopup.visible = false
        }
        if (callTip.visible)
            overlays.updateCallTip()
    }

    function handleOverlayKey(key) {
        if (key === Qt.Key_Escape)
            autocompletePopup.visible = false
        else if (key === Qt.Key_Down)
            acList.incrementCurrentIndex()
        else if (key === Qt.Key_Up)
            acList.decrementCurrentIndex()
        else if (key === Qt.Key_Return || key === Qt.Key_Enter || key === Qt.Key_Tab)
            overlays.acceptAutocomplete()
    }

    // Call tip overlay: "find(filter, **projection**, options)"
    Rectangle {
        id: callTip
        property string tipName: ""
        property var tipParams: []
        property int tipActive: 0
        visible: false
        z: 11
        width: Math.min(callTipText.implicitWidth + 20, overlays.editor.width - 16)
        height: callTipText.implicitHeight + 10
        radius: 6
        color: Theme.baseColor
        border.color: Theme.glassBorderHover
        border.width: 1

        Text {
            id: callTipText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, callTip.width - 20)
            elide: Text.ElideRight
            textFormat: Text.StyledText
            font.family: AppSettings.codeFontFamily
            font.pixelSize: Math.max(11, AppSettings.codeFontSize - 1)
            color: Theme.disabledTextColor
            text: {
                const parts = []
                for (let i = 0; i < callTip.tipParams.length; ++i) {
                    const p = callTip.tipParams[i]
                    parts.push(i === callTip.tipActive
                               ? "<b><font color=\"" + Theme.accentColor + "\">" + p + "</font></b>"
                               : p)
                }
                return "<font color=\"" + Theme.textColor + "\">" + callTip.tipName + "</font>("
                       + parts.join(", ") + ")"
            }
        }
    }

    // Autocomplete overlay popup
    Rectangle {
        id: autocompletePopup
        visible: false
        z: 10
        width: Math.min(340, Math.max(200, overlays.editor.width - 24))
        height: Math.min(acList.contentHeight + 8, 220)
        color: Theme.baseColor
        border.color: Theme.accentColor
        border.width: 1
        radius: 10

        ListView {
            id: acList
            anchors.fill: parent
            anchors.margins: 4
            clip: true
            model: overlays.page.shell ? overlays.page.shell.autocompleteList : []
            delegate: ItemDelegate {
                id: acItem
                required property int index
                required property string modelData
                width: acList.width
                height: Math.max(26, AppSettings.codeFontSize + 13)
                focusPolicy: Qt.NoFocus
                highlighted: acList.currentIndex === acItem.index
                hoverEnabled: true
                readonly property string detail: {
                    const d = overlays.page.shell ? overlays.page.shell.autocompleteDetails : []
                    return acItem.index < d.length ? d[acItem.index] : ""
                }
                contentItem: RowLayout {
                    spacing: 8
                    Label {
                        text: acItem.modelData
                        font.family: AppSettings.codeFontFamily
                        font.pixelSize: AppSettings.codeFontSize
                        color: {
                            if (Theme.dark)
                                return (acItem.down || acItem.highlighted || acItem.hovered) ? "#4ade80" : Theme.textColor
                            return acItem.down ? "#064e3b"
                                 : ((acItem.highlighted || acItem.hovered) ? "#065f46" : Theme.textColor)
                        }
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: 8
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        visible: acItem.detail.length > 0
                        text: acItem.detail
                        font.family: AppSettings.codeFontFamily
                        font.pixelSize: Math.max(10, AppSettings.codeFontSize - 2)
                        color: Theme.disabledTextColor
                        verticalAlignment: Text.AlignVCenter
                        rightPadding: 8
                    }
                }
                background: Rectangle {
                    radius: 6
                    color: {
                        if (Theme.dark)
                            return acItem.down ? "#1f4735"
                                 : (acItem.highlighted ? "#173628" : (acItem.hovered ? "#13291f" : "transparent"))
                        return acItem.down ? "#cceee0"
                             : (acItem.highlighted ? "#e4f5ed" : (acItem.hovered ? "#eef8f3" : "transparent"))
                    }
                }
                onClicked: {
                    acList.currentIndex = acItem.index
                    overlays.acceptAutocomplete()
                }
            }
            ScrollBar.vertical: ScrollBar {}
        }
    }
}
