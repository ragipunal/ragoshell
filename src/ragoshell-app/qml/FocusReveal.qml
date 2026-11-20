import QtQuick

// Keeps the focused field of a scrolling form in view: when focus moves to a field, and when the
// space shrinks (the on-screen keyboard opens), the form scrolls so the field is visible.
//
//   FocusReveal { flickable: someScrollView.contentItem; space: dlg.visibleHeight }
QtObject {
    id: reveal

    // The Flickable that holds the form (a ScrollView's contentItem).
    property Flickable flickable: null
    // Anything that changes when the visible space changes; the form is checked again then.
    property real space: 0

    function reveal_() {
        if (!flickable || !flickable.contentItem || !flickable.visible)
            return
        var window = flickable.Window.window
        var item = window ? window.activeFocusItem : null
        if (!item || item.height <= 0)
            return
        // Only fields inside this form.
        var p = item
        while (p && p !== flickable.contentItem)
            p = p.parent
        if (!p)
            return
        var top = item.mapToItem(flickable.contentItem, 0, 0).y
        var bottom = top + item.height
        var margin = 8
        if (top < flickable.contentY)
            flickable.contentY = Math.max(0, top - margin)
        else if (bottom > flickable.contentY + flickable.height)
            flickable.contentY = Math.min(flickable.contentHeight - flickable.height, bottom - flickable.height + margin)
    }

    property Connections _focus: Connections {
        target: reveal.flickable ? reveal.flickable.Window.window : null
        function onActiveFocusItemChanged() { Qt.callLater(reveal.reveal_) }
    }

    onSpaceChanged: Qt.callLater(reveal_)
}
