import QtQuick

// The one list of application commands (docs/DesignSystem.md, section 6). The command palette, the
// top bar's "more" menu and the shell tab's action strip read their titles, key texts, enabled state
// and handlers from here. Handlers call the same functions of the main window as the global Shortcuts of
// Main.qml, which keep binding the keys; `shortcut` is display only, so no key is bound twice.
QtObject {
    id: actions

    required property var app

    // Bumped by refresh() (and when hardware keyboard presence changes) so bindings that read get()/list() evaluate again.
    property int revision: 0

    readonly property Connections _platformConn: Connections {
        target: Platform
        function onHardwareKeyboardChanged() { actions.revision++ }
    }

    // State of the current tab and connections, refreshed when a menu or the palette opens.
    property bool hasTab: false
    property bool hasShell: false
    property bool hasCollection: false
    property bool canInsertDoc: false
    property bool hasConnection: false
    // Documents selected in the result of the current shell tab, and whether they can be deleted.
    property int selectedDocCount: 0
    property bool canDeleteSelection: false

    function refresh() {
        var page = app.currentShellPage()
        hasTab = !!app.tabs && app.tabs.count > 0
        hasShell = !!page
        hasCollection = !!page && !!page.shell && page.shell.activeCollection.length > 0
        canInsertDoc = !!page && !!page.shell && page.shell.canInsertDocuments
        hasConnection = Backend.firstConnectedUuid().length > 0
        selectedDocCount = page ? page.selectedDocIndices.length : 0
        canDeleteSelection = !!page && !!page.shell && !page.shell.busy
                             && page.shell.canDeleteDocuments && selectedDocCount > 0
        revision++
    }

    // Section order and titles of the palette (they are the names of the former menus).
    readonly property var sections: [
        { id: "file", title: function() { return qsTr("File") } },
        //: Menu title: actions for the MongoDB shell tab (mongosh).
        { id: "shell", title: function() { return qsTr("Shell") } },
        //: Menu title: result view and layout options.
        { id: "view", title: function() { return qsTr("View") } },
        { id: "tools", title: function() { return qsTr("Tools") } },
        { id: "help", title: function() { return qsTr("Help") } }
    ]

    // Command definitions, in display order. A command may carry:
    //   separatorBefore  a separator line precedes it in a menu
    //   shortcut         key text shown next to the title: a function returning Platform.keyLabel("Ctrl+N")
    //                    (the portable spelling); it is shown only where shortcuts are (Platform.shortcutsVisible)
    //   desktopOnly      not offered on a phone or tablet (Quit)
    //   keyboardOnly     offered where a keyboard is: on desktop, or on a phone or tablet with one attached
    //   enabled          function; absent means always available
    readonly property var definitions: [
        { id: "file.new", section: "file",
          title: function() { return qsTr("New Connection…") },
          shortcut: function() { return Platform.keyLabel("Ctrl+N") },
          run: function() { app.openNewConnection() } },
        { id: "file.openScript", section: "file", separatorBefore: true, needs: "shell",
          title: function() { return qsTr("Open Script…") },
          shortcut: function() { return Platform.keyLabel("Ctrl+O") },
          enabled: function() { return hasShell },
          run: function() { app.openScript() } },
        { id: "file.saveScript", section: "file", needs: "shell",
          title: function() { return qsTr("Save Script") },
          shortcut: function() { return Platform.keyLabel("Ctrl+S") },
          enabled: function() { return hasShell },
          run: function() { app.triggerSaveCurrentScript(false) } },
        { id: "file.saveScriptAs", section: "file", needs: "shell",
          title: function() { return qsTr("Save Script As…") },
          shortcut: function() { return Platform.keyLabel("Ctrl+Shift+S") },
          enabled: function() { return hasShell },
          run: function() { app.triggerSaveCurrentScript(true) } },
        { id: "file.recover", section: "file", needs: "drafts",
          title: function() { return qsTr("Recover Unsaved Scripts…") },
          enabled: function() { return Drafts.count > 0 },
          run: function() { app.openRecoverDrafts() } },
        { id: "file.preferences", section: "file", separatorBefore: true,
          title: function() { return qsTr("Preferences…") },
          shortcut: function() { return Platform.keyLabel("Ctrl+,") },
          run: function() { app.openPreferences() } },
        { id: "file.quit", section: "file", separatorBefore: true, desktopOnly: true,
          title: function() { return qsTr("Quit") },
          shortcut: function() { return Platform.keyLabel("Ctrl+Q") },
          run: function() { app.close() } },

        { id: "shell.new", section: "shell", needs: "connection",
          title: function() { return qsTr("New Shell Tab") },
          shortcut: function() { return Platform.keyLabel("Ctrl+T") },
          enabled: function() { return hasConnection },
          run: function() { app.openNewShell() } },
        { id: "shell.duplicate", section: "shell", needs: "shell",
          title: function() { return qsTr("Duplicate Tab") },
          shortcut: function() { return Platform.keyLabel("Ctrl+Shift+T") },
          enabled: function() { return hasShell },
          run: function() { app.duplicateCurrentShell() } },
        { id: "shell.close", section: "shell", needs: "tab",
          title: function() { return qsTr("Close Tab") },
          shortcut: function() { return Platform.keyLabel("Ctrl+W") },
          enabled: function() { return hasTab },
          run: function() { app.closeCurrentTab() } },
        { id: "shell.execute", section: "shell", separatorBefore: true, needs: "shell",
          title: function() { return qsTr("Execute") },
          shortcut: function() { return "F5" },
          enabled: function() { return hasShell },
          run: function() { app.executeCurrent() } },
        { id: "shell.executeAll", section: "shell", needs: "shell",
          title: function() { return qsTr("Execute All") },
          shortcut: function() { return Platform.keyLabel("Ctrl+Shift+Enter") },
          enabled: function() { return hasShell },
          run: function() { app.executeAll() } },
        { id: "shell.stop", section: "shell", needs: "shell",
          title: function() { return qsTr("Stop") },
          shortcut: function() { return "F6" },
          enabled: function() { return hasShell },
          run: function() { app.stopCurrent() } },
        { id: "shell.format", section: "shell", separatorBefore: true, needs: "shell",
          title: function() { return qsTr("Format Script") },
          shortcut: function() { return "Alt+Shift+F" },
          enabled: function() { return hasShell },
          run: function() { app.withShellPage(function(p) { p.formatScript() }) } },
        { id: "shell.queryBar", section: "shell", needs: "shell",
          title: function() { return qsTr("Query Bar") },
          shortcut: function() { return Platform.keyLabel("Ctrl+Shift+F") },
          enabled: function() { return hasShell },
          run: function() { app.withShellPage(function(p) { p.toggleQueryBar() }) } },
        { id: "shell.insertDocument", section: "shell", needs: "collection",
          title: function() { return qsTr("Insert Document…") },
          shortcut: function() { return Platform.keyLabel("Ctrl+D") },
          enabled: function() { return canInsertDoc },
          run: function() { app.withShellPage(function(p) { p.triggerInsertDocument() }) } },
        // Asks for confirmation first, like the Delete button of the result bar.
        { id: "shell.deleteDocuments", section: "shell", needs: "selection",
          title: function() {
              return selectedDocCount > 1 ? qsTr("Delete %n Documents…", "", selectedDocCount)
                                          : qsTr("Delete Document…")
          },
          shortcut: function() { return Platform.usesCommandKey ? Platform.keyLabel("Ctrl+Backspace") : "Del" },
          enabled: function() { return canDeleteSelection },
          run: function() { app.withShellPage(function(p) { p.triggerDeleteSelectedDocuments() }) } },
        { id: "shell.history", section: "shell", separatorBefore: true,
          title: function() { return qsTr("Query History…") },
          shortcut: function() { return Platform.keyLabel("Ctrl+H") },
          run: function() { app.openHistory(false) } },
        { id: "shell.snippets", section: "shell",
          title: function() { return qsTr("Snippets…") },
          run: function() { app.openHistory(true) } },
        { id: "shell.exportQuery", section: "shell", needs: "shell",
          title: function() { return qsTr("Export Query to Language…") },
          shortcut: function() { return Platform.keyLabel("Ctrl+Shift+E") },
          enabled: function() { return hasShell },
          run: function() { app.exportCurrentQuery() } },
        { id: "shell.saveSnippet", section: "shell", needs: "shell",
          title: function() { return qsTr("Save Script as Snippet…") },
          enabled: function() { return hasShell },
          run: function() { app.saveScriptAsSnippet() } },

        { id: "view.tree", section: "view", needs: "shell",
          title: function() { return qsTr("Tree View") },
          shortcut: function() { return "F2" },
          enabled: function() { return hasShell },
          run: function() { app.setCurrentViewMode(0) } },
        { id: "view.table", section: "view", needs: "shell",
          title: function() { return qsTr("Table View") },
          shortcut: function() { return "F3" },
          enabled: function() { return hasShell },
          run: function() { app.setCurrentViewMode(1) } },
        { id: "view.text", section: "view", needs: "shell",
          title: function() { return qsTr("Text View") },
          shortcut: function() { return "F4" },
          enabled: function() { return hasShell },
          run: function() { app.setCurrentViewMode(2) } },
        { id: "view.orientation", section: "view", needs: "shell",
          title: function() { return qsTr("Toggle Result Orientation") },
          shortcut: function() { return "F10" },
          enabled: function() { return hasShell },
          run: function() { app.withShellPage(function(p) { p.toggleOrientation() }) } },
        { id: "view.explorer", section: "view", separatorBefore: true,
          title: function() { return qsTr("Explorer Panel") },
          shortcut: function() { return Platform.keyLabel("Ctrl+B") },
          run: function() { app.toggleExplorer() } },
        { id: "view.log", section: "view",
          title: function() { return qsTr("Log Panel") },
          shortcut: function() { return Platform.keyLabel("Ctrl+L") },
          run: function() { app.toggleLogPanel() } },
        { id: "view.welcome", section: "view",
          title: function() { return qsTr("Welcome Tab") },
          shortcut: function() { return Platform.keyLabel("Ctrl+Shift+W") },
          run: function() { app.reopenWelcome() } },
        { id: "view.fullScreen", section: "view", separatorBefore: true, desktopOnly: true,
          title: function() { return qsTr("Full Screen") },
          shortcut: function() { return "F11" },
          run: function() { app.toggleFullScreen() } },

        { id: "tools.palette", section: "tools",
          title: function() { return qsTr("Command Palette…") },
          shortcut: function() { return Platform.keyLabel("Ctrl+K") },
          run: function() { app.openPalette() } },
        { id: "tools.aggregation", section: "tools", needs: "collection",
          title: function() { return qsTr("Aggregation Builder…") },
          shortcut: function() { return Platform.keyLabel("Ctrl+Shift+A") },
          enabled: function() { return hasCollection },
          run: function() { app.openPipelineBuilderForCurrent() } },
        { id: "tools.schema", section: "tools", needs: "collection",
          title: function() { return qsTr("Schema Analysis…") },
          enabled: function() { return hasCollection },
          run: function() { app.openSchemaAnalysisForCurrent() } },
        { id: "tools.ai", section: "tools", needs: "collection",
          title: function() { return qsTr("AI Query Assistant…") },
          shortcut: function() { return Platform.keyLabel("Ctrl+Shift+G") },
          enabled: function() { return hasCollection },
          run: function() { app.openAiAssistantForCurrent() } },
        { id: "tools.monitor", section: "tools", needs: "connection",
          title: function() { return qsTr("Server Monitor") },
          shortcut: function() { return Platform.keyLabel("Ctrl+Shift+M") },
          enabled: function() { return hasConnection },
          run: function() { app.openMonitorForActiveServer() } },

        { id: "help.shortcuts", section: "help", keyboardOnly: true,
          title: function() { return qsTr("Keyboard Shortcuts") },
          shortcut: function() { return "F1" },
          run: function() { app.openShortcuts() } },
        { id: "help.logFolder", section: "help",
          title: function() { return Platform.isMobile ? qsTr("Share Log File…") : qsTr("Open Log Folder") },
          run: function() { Backend.openLogFolder() } },
        { id: "help.licenses", section: "help", separatorBefore: true,
          title: function() { return qsTr("Open-Source Licenses…") },
          run: function() { app.openLicenses() } },
        { id: "help.about", section: "help",
          title: function() { return qsTr("About %1").arg(Brand.productName) },
          run: function() { app.openAbout() } }
    ]

    function sectionTitle(sectionId) {
        revision
        for (var i = 0; i < sections.length; ++i) {
            if (sections[i].id === sectionId)
                return sections[i].title()
        }
        return sectionId
    }

    // Why a command is dimmed, from the condition it needs; empty when it is available.
    function reasonFor(needs) {
        switch (needs) {
        case "tab": return qsTr("No open tabs")
        case "shell": return qsTr("Needs a shell tab")
        case "collection": return qsTr("Needs an active collection")
        case "connection": return qsTr("Needs a connected server")
        case "selection": return qsTr("Select documents in a result first")
        case "drafts": return qsTr("No unsaved scripts")
        }
        return ""
    }

    // Whether the command is offered on this platform and with the input devices at hand.
    function shownHere(d) {
        var showShortcuts = Platform.shortcutsVisible
        if (d.desktopOnly && Platform.isMobile)
            return false
        if (d.keyboardOnly && !showShortcuts)
            return false
        return true
    }

    // One command with its current title, key text and enabled state, or null.
    function get(commandId) {
        revision
        var showShortcuts = Platform.shortcutsVisible
        for (var i = 0; i < definitions.length; ++i) {
            var d = definitions[i]
            if (d.id !== commandId)
                continue
            return {
                id: d.id,
                section: d.section,
                separatorBefore: !!d.separatorBefore,
                title: d.title(),
                shortcut: (showShortcuts && d.shortcut) ? d.shortcut() : "",
                shown: shownHere(d),
                enabled: d.enabled ? !!d.enabled() : true,
                needs: d.needs || "",
                disabledReason: (d.enabled && !d.enabled()) ? reasonFor(d.needs || "") : "",
                run: d.run
            }
        }
        return null
    }

    // All commands in display order, or those of one section.
    function list(sectionId) {
        revision
        var result = []
        for (var i = 0; i < definitions.length; ++i) {
            var d = definitions[i]
            if ((sectionId === undefined || d.section === sectionId) && shownHere(d))
                result.push(get(d.id))
        }
        return result
    }

    function run(commandId) {
        // The state may be older than the last click (a menu refreshes it when it opens).
        refresh()
        var c = get(commandId)
        if (c && c.enabled)
            c.run()
    }

    // Problems in the registry itself; empty when it is sound.
    function validate() {
        var problems = []
        var seen = {}
        var knownSections = {}
        for (var s = 0; s < sections.length; ++s)
            knownSections[sections[s].id] = true
        for (var i = 0; i < definitions.length; ++i) {
            var d = definitions[i]
            if (!d.id || seen[d.id])
                problems.push("duplicate or empty id: " + d.id)
            seen[d.id] = true
            if (!knownSections[d.section])
                problems.push(d.id + ": unknown section " + d.section)
            if (typeof d.run !== "function")
                problems.push(d.id + ": no handler")
            if (typeof d.title !== "function" || !d.title())
                problems.push(d.id + ": no title")
        }
        return problems
    }

    // --check-qml turns an exception here into a failure, so a broken registry stops CI.
    Component.onCompleted: {
        var problems = validate()
        if (problems.length > 0)
            throw new Error("AppActions: " + problems.join("; "))
    }
}
