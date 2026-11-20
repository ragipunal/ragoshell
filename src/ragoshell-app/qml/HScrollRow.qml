import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// A toolbar row: its controls keep their natural widths, and when the space is too narrow (a phone, a split
// window, a long translation) the row scrolls sideways instead of squeezing the buttons until their text is cut
// (docs/UiMatrix.md). Children go in like those of a RowLayout; a spacer with Layout.fillWidth still pushes the
// controls after it to the right edge when there is room.
Flickable {
    id: strip

    default property alias content: row.data
    property alias spacing: row.spacing
    /// Space before the first and after the last control.
    property int margin: 10

    clip: true
    implicitHeight: row.implicitHeight
    contentWidth: row.width + strip.margin * 2
    contentHeight: height
    flickableDirection: Flickable.HorizontalFlick
    boundsBehavior: Flickable.StopAtBounds
    // The mouse wheel scrolls the row sideways when it overflows.
    ScrollBar.horizontal: ScrollBar {
        policy: Theme.touch ? ScrollBar.AlwaysOff : ScrollBar.AsNeeded
        implicitHeight: 6
    }

    RowLayout {
        id: row
        x: strip.margin
        height: strip.height
        width: Math.max(strip.width - strip.margin * 2, implicitWidth)
        spacing: 8
    }
}
