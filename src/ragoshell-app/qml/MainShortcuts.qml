pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

Item {
    id: shortcutsHost
    required property var app
    required property var explorerPane
    readonly property bool wsEnabled: !UpdateService.mandatory
    visible: false
    width: 0
    height: 0

    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: ["Ctrl+K", "Ctrl+Shift+P"]
        onActivated: shortcutsHost.app.openPalette()
    }
    // ---- Cross-Platform Global Shortcuts (StandardKey + OS-aware) -----------
    // Backend.keySequences de-duplicates: a key listed twice never fires.
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: Backend.keySequences(StandardKey.New, ["Ctrl+N"])
        onActivated: shortcutsHost.app.openNewConnection()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: Backend.keySequences(StandardKey.Open, ["Ctrl+O"])
        onActivated: shortcutsHost.app.openScript()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequence: "Ctrl+H"
        onActivated: shortcutsHost.app.openHistory(false)
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequence: "Ctrl+Shift+A"
        onActivated: shortcutsHost.app.openPipelineBuilderForCurrent()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequence: "Ctrl+Shift+E"
        onActivated: shortcutsHost.app.exportCurrentQuery()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequence: "Ctrl+Shift+G"
        onActivated: shortcutsHost.app.openAiAssistantForCurrent()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: Backend.keySequences(StandardKey.Save, ["Ctrl+S"])
        onActivated: shortcutsHost.app.triggerSaveCurrentScript(false)
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: Backend.keySequences(StandardKey.SaveAs, ["Ctrl+Shift+S"])
        onActivated: shortcutsHost.app.triggerSaveCurrentScript(true)
    }
    Shortcut {
        sequences: Backend.keySequences(StandardKey.Quit, ["Ctrl+Q"])
        onActivated: shortcutsHost.app.close()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: Backend.keySequences(StandardKey.Preferences, ["Ctrl+,"])
        onActivated: shortcutsHost.app.openPreferences()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: ["F1", "Ctrl+Shift+/"]
        onActivated: shortcutsHost.app.openShortcuts()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequence: "Ctrl+T"
        onActivated: shortcutsHost.app.openNewShell()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: Backend.keySequences(StandardKey.Close, ["Ctrl+W"])
        onActivated: shortcutsHost.app.closeTabAt(shortcutsHost.app.tabs.currentIndex)
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequence: "Ctrl+Shift+T"
        onActivated: shortcutsHost.app.duplicateCurrentShell()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequence: "Ctrl+Shift+W"
        onActivated: shortcutsHost.app.reopenWelcome()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequence: "Ctrl+Shift+M"
        onActivated: shortcutsHost.app.openMonitorForActiveServer()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: ["Ctrl+R", "F5"]
        onActivated: shortcutsHost.app.executeCurrent()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: ["Ctrl+Shift+Return", "Ctrl+Shift+Enter"]
        onActivated: shortcutsHost.app.executeAll()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: ["F6", "Ctrl+."]
        onActivated: shortcutsHost.app.stopCurrent()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequence: "Ctrl+Shift+F"
        onActivated: shortcutsHost.app.withShellPage(function(p) { p.toggleQueryBar() })
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequence: "Alt+Shift+F"
        onActivated: shortcutsHost.app.withShellPage(function(p) { p.formatScript() })
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequence: "Ctrl+D"
        onActivated: shortcutsHost.app.withShellPage(function(p) { p.triggerInsertDocument() })
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: ["Ctrl+B", "Ctrl+E"]
        onActivated: shortcutsHost.app.toggleExplorer()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequence: "Ctrl+L"
        onActivated: shortcutsHost.app.toggleLogPanel()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: ["F2", "Alt+Shift+1"]
        onActivated: shortcutsHost.app.setCurrentViewMode(0)
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: ["F3", "Alt+Shift+2"]
        onActivated: shortcutsHost.app.setCurrentViewMode(1)
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: ["F4", "Alt+Shift+3"]
        onActivated: shortcutsHost.app.setCurrentViewMode(2)
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: ["F10", "Alt+Shift+O"]
        onActivated: shortcutsHost.app.withShellPage(function(p) { p.toggleOrientation() })
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: Backend.keySequences(StandardKey.FullScreen, ["F11"])
        onActivated: shortcutsHost.app.toggleFullScreen()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequence: "Ctrl+M"
        onActivated: shortcutsHost.app.showMinimized()
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: ["Ctrl+Tab", "Ctrl+PgDown"]
        onActivated: {
            if (shortcutsHost.app.tabs.count > 1)
                shortcutsHost.app.tabs.currentIndex = (shortcutsHost.app.tabs.currentIndex + 1) % shortcutsHost.app.tabs.count
        }
    }
    Shortcut {
        enabled: shortcutsHost.wsEnabled
        sequences: ["Ctrl+Shift+Tab", "Ctrl+PgUp"]
        onActivated: {
            if (shortcutsHost.app.tabs.count > 1)
                shortcutsHost.app.tabs.currentIndex =
                    (shortcutsHost.app.tabs.currentIndex - 1 + shortcutsHost.app.tabs.count) % shortcutsHost.app.tabs.count
        }
    }
    // Alt+1..9 (Win/Linux) and Cmd+1..9 (macOS): jump directly to tab N.
    Instantiator {
        model: 9
        Shortcut {
            id: jumpShortcut
            required property int index
            enabled: shortcutsHost.wsEnabled
            sequences: shortcutsHost.app.isMac
                       ? ["Ctrl+" + (index + 1), "Alt+" + (index + 1)]
                       : ["Alt+" + (index + 1)]
            onActivated: {
                if (jumpShortcut.index < shortcutsHost.app.tabs.count)
                    shortcutsHost.app.tabs.currentIndex = jumpShortcut.index
            }
        }
    }
}
