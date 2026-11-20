import QtQuick

// A menu item whose title, key text, enabled state and handler come from the command registry
// (AppActions.qml).
RagoMenuItem {
    id: item

    required property var actions
    required property string commandId

    readonly property var command: actions.get(commandId)

    visible: command ? command.shown : true
    text: command ? command.title : commandId
    shortcutText: command ? command.shortcut : ""
    enabled: command ? command.enabled : false
    onTriggered: actions.run(commandId)
}
