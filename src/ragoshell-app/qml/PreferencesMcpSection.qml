pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

// Preferences › AI tools (MCP): the switch and limits of the read-only MCP
// server and a ready configuration per AI client (docs/McpSettingsUi.md).
// Which connections it serves is chosen in each connection's dialog.
ColumnLayout {
    id: root
    spacing: 14
    visible: McpSettings.available

    // Values shown when the dialog opened: only what the user changed is
    // written, so a change made meanwhile with ragoshell-mcp is kept.
    property var _loaded: ({})

    function syncFields() {
        McpSettings.reload()
        mcpEnabledCheck.checked = McpSettings.enabled
        mcpDocsSpin.value = McpSettings.maxDocuments
        mcpTimeSpin.value = McpSettings.maxTimeSec
        _loaded = { "enabled": mcpEnabledCheck.checked, "maxDocuments": mcpDocsSpin.value,
                    "maxTimeSec": mcpTimeSpin.value }
    }

    function commitSpinBoxes() {
        mcpDocsSpin.commitText()
        mcpTimeSpin.commitText()
    }

    function apply() {
        if (!McpSettings.available || !McpSettings.writable)
            return
        var changes = {}
        if (mcpEnabledCheck.checked !== _loaded.enabled)
            changes.enabled = mcpEnabledCheck.checked
        if (mcpDocsSpin.value !== _loaded.maxDocuments)
            changes.maxDocuments = mcpDocsSpin.value
        if (mcpTimeSpin.value !== _loaded.maxTimeSec)
            changes.maxTimeSec = mcpTimeSpin.value
        McpSettings.updateServer(changes)
    }

    readonly property var _clientHints: [
        qsTr("Run this command in a terminal, then check it with: claude mcp list"),
        qsTr("Claude Desktop: Settings › Developer › Edit Config, add this to claude_desktop_config.json and restart Claude Desktop."),
        qsTr("Cursor: add this to ~/.cursor/mcp.json (all projects) or to .cursor/mcp.json in a project."),
        qsTr("Antigravity: in the agent panel open … › MCP Servers › Manage MCP Servers › View raw config and add this."),
        qsTr("VS Code: add this to .vscode/mcp.json in a project (or MCP: Open User Configuration), then use Copilot Chat in Agent mode.")
    ]

    Label {
        text: qsTr("AI TOOLS (MCP)")
        font.bold: true
        font.pixelSize: 11
        font.letterSpacing: Language.rtl ? 0 : 1.1
        color: Theme.accentColor
    }

    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: 12
        color: Theme.disabledTextColor
        text: qsTr("AI tools such as Claude, Cursor, Antigravity or Copilot can read the connections you allow through %1, a local, read-only MCP server: list databases and collections, run find, aggregate and count, and explain queries. It never writes and opens no network port. Allow a connection in its own settings (Connection tab).")
                  .arg(Brand.productId + "-mcp")
    }

    Label {
        visible: McpSettings.error.length > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: 12
        color: Theme.dangerColor
        text: !McpSettings.writable
              ? qsTr("The MCP settings file cannot be read, so it is left unchanged and the server stays off: %1")
                    .arg(McpSettings.error)
              : qsTr("The last change to the MCP settings could not be saved, so AI tools may keep their access: %1")
                    .arg(McpSettings.error)
    }

    RagoCheckBox {
        id: mcpEnabledCheck
        Layout.fillWidth: true
        enabled: McpSettings.writable
        text: qsTr("Let AI tools use the MCP server")
    }

    GridLayout {
        Layout.fillWidth: true
        columns: Window.width < 520 ? 1 : 2
        columnSpacing: 16
        rowSpacing: 10
        enabled: McpSettings.writable && mcpEnabledCheck.checked

        Label {
            text: qsTr("Documents per result")
            font.pixelSize: 13
            color: Theme.textColor
            Layout.fillWidth: true
        }
        RagoSpinBox {
            id: mcpDocsSpin
            from: 1
            to: McpSettings.hardMaxDocuments
            Layout.preferredWidth: 200
        }

        Label {
            text: qsTr("Time limit per query (sec)")
            font.pixelSize: 13
            color: Theme.textColor
            Layout.fillWidth: true
        }
        RagoSpinBox {
            id: mcpTimeSpin
            from: 1
            to: McpSettings.hardMaxTimeSec
            Layout.preferredWidth: 200
        }
    }

    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: 12
        color: McpSettings.allowedCount === 0 && mcpEnabledCheck.checked ? Theme.warningColor
                                                                         : Theme.disabledTextColor
        text: McpSettings.allowedCount === 0
              ? qsTr("No connection is allowed yet. Tick \"Let AI tools read this connection\" in a connection's settings.")
              : qsTr("Allowed connections: %1. Turning the server off or unticking a connection takes effect at the AI tool's next request.")
                    .arg(McpSettings.allowedCount)
    }

    Label {
        Layout.topMargin: 4
        text: qsTr("Add to an AI tool")
        font.pixelSize: 13
        color: Theme.textColor
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        RagoComboBox {
            id: clientCombo
            model: McpSettings.clientNames
            Layout.fillWidth: true
            Layout.minimumWidth: 0
        }
        RagoPillButton {
            text: qsTr("Copy")
            onClicked: Backend.copyToClipboard(snippetArea.text)
        }
    }

    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        color: Theme.disabledTextColor
        text: (root._clientHints[clientCombo.currentIndex] || "")
              + (clientCombo.currentIndex > 0
                 ? " " + qsTr("If the file already lists other servers, add only the \"%1\" entry.").arg(Brand.productId)
                 : "")
    }

    Rectangle {
        Layout.fillWidth: true
        implicitHeight: snippetArea.implicitHeight + 4
        radius: 8
        color: Theme.baseColor
        border.width: 1
        border.color: Theme.glassBorder

        TextArea {
            id: snippetArea
            anchors.fill: parent
            anchors.margins: 2
            // Commands and JSON read left to right in every language.
            LayoutMirroring.enabled: false
            readOnly: true
            selectByMouse: true
            wrapMode: TextArea.WrapAnywhere
            font.family: AppSettings.codeFontFamily
            font.pixelSize: 12
            color: Theme.textColor
            text: McpSettings.snippet(clientCombo.currentIndex)
            background: null
        }
    }

    Label {
        visible: clientCombo.currentIndex === 0 && !McpSettings.commandLineSafe
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        color: Theme.warningColor
        text: qsTr("The folder of %1 contains $, ` or %, which a terminal would expand, so no command is offered. Use one of the JSON blocks, or move the app to a plain folder.")
                  .arg(McpSettings.serverPath)
    }

    Label {
        visible: !McpSettings.serverFound
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        color: Theme.warningColor
        text: qsTr("%1 was not found next to the app, so this build may not include it.")
                  .arg(McpSettings.serverPath)
    }
}
