import QtQuick

// A button of the shell tab's action strip (docs/DesignSystem.md, section 6) that runs a command of the
// registry (AppActions.qml): a pill with a label, or only the icon when the strip is narrow.
Item {
    id: root

    required property var actions
    required property string commandId
    property string iconText: ""
    property string label: ""
    property bool showLabel: true

    readonly property var command: actions.get(commandId)

    implicitWidth: showLabel ? pill.implicitWidth : iconButton.implicitWidth
    implicitHeight: Theme.touch ? 32 : 24

    RagoPillButton {
        id: pill
        visible: root.showLabel
        anchors.fill: parent
        compact: true
        text: root.label
        iconText: root.iconText
        onClicked: root.actions.run(root.commandId)
    }

    RagoIconButton {
        id: iconButton
        visible: !root.showLabel
        anchors.centerIn: parent
        glyph: root.iconText
        size: 24
        toolTipText: root.command && root.command.shortcut.length > 0
                     ? root.label + " (" + root.command.shortcut + ")" : root.label
        onClicked: root.actions.run(root.commandId)
    }
}
