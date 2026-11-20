import QtQuick

// The height of a reference item (a window's overlay) that the on-screen keyboard leaves free.
// Dialogs and the command palette size and center themselves in it, so Save / Test and the rows
// they show never end up behind the keyboard.
//
// Qt on Android reports the keyboard rectangle in device pixels and does not always shrink the
// window; where the system also shrinks the window the keyboard starts at its edge and nothing is
// subtracted twice. Without a keyboard (desktop) this is just the reference height.
QtObject {
    id: area

    required property Item reference

    readonly property real left: reference ? reference.SafeArea.margins.left : 0
    readonly property real right: reference ? reference.SafeArea.margins.right : 0
    readonly property real top: reference ? reference.SafeArea.margins.top : 0
    readonly property real width: Math.max(0, (reference ? reference.width : 0) - left - right)

    readonly property real height: {
        var full = reference ? reference.height : 0
        var bottomSafe = reference ? reference.SafeArea.margins.bottom : 0
        var bottomEdge = full - bottomSafe
        if (Qt.inputMethod.visible && Qt.inputMethod.keyboardRectangle.height > 0) {
            var ratio = Qt.platform.os === "android" ? Qt.application.screens[0].devicePixelRatio : 1
            bottomEdge = Math.min(bottomEdge, Qt.inputMethod.keyboardRectangle.y / ratio)
        }
        return Math.max(0, bottomEdge - top)
    }
}
