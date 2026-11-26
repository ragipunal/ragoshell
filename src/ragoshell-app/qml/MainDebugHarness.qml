pragma ComponentBehavior: Bound

import QtQuick
import RagoShell

QtObject {
    id: harness
    required property var app
    required property var appBar
    required property var connectionsPane
    required property var commandPalette
    required property var hostKeyDialog
    required property var codeExportDialog

    // Dev/CI visual verification helper invoked by --open-view <name>
    function openDebugView(viewName) {
        if (viewName === "conn")
            harness.app.openNewConnection()
        else if (viewName === "prefs")
            harness.app.openPreferences()
        else if (viewName === "prefs-mcp")
            harness.app.openPreferences("mcp")
        else if (viewName === "prefs-account-code") {
            CloudService.previewDebugState(1)
            harness.app.openPreferences("account")
        }
        else if (viewName === "prefs-account-pro") {
            CloudService.previewDebugState(2)
            harness.app.openPreferences("account")
        }
        else if (viewName === "prefs-account-sync") {
            CloudService.previewDebugState(3)
            harness.app.openPreferences("account")
        }
        else if (viewName === "cloud-sync") {
            CloudService.previewDebugState(0)
            harness.app.openCloudSync(false)
        }
        else if (viewName === "cloud-sync-code") {
            CloudService.previewDebugState(1)
            harness.app.openCloudSync(false)
        }
        else if (viewName === "cloud-sync-pro") {
            CloudService.previewDebugState(2)
            harness.app.openCloudSync(true)
        }
        else if (viewName === "cloud-sync-active") {
            CloudService.previewDebugState(3)
            harness.app.openCloudSync(false)
        }
        else if (viewName === "sync-menu") {
            CloudService.previewDebugState(3)
            harness.appBar.openSyncMenuDebug()
        }
        else if (viewName === "about")
            harness.app.openAbout()
        else if (viewName === "licenses")
            harness.app.openLicenses()
        else if (viewName === "shortcuts")
            harness.app.openShortcuts()
        else if (viewName === "history")
            harness.app.openHistory(false)
        else if (viewName === "more-menu")
            harness.appBar.openMoreMenu()
        else if (viewName === "theme-menu")
            harness.appBar.openThemeMenu()
        else if (viewName.indexOf("palette") === 0) {
            harness.commandPalette.openPalette()
            if (viewName.length > 8)
                harness.commandPalette.setQuery(viewName.substring(8))
        }
        else if (viewName === "recover")
            harness.app.openRecoverDrafts()
        else if (viewName === "hostkey" || viewName === "hostkey-changed")
            harness.hostKeyDialog.get().ask(0, "db.example.com", 22, "ssh-ed25519",
                                            "SHA256:mICXHQmntMibzPZGboPShWVsNNpXTBvo0K2yhzeABq8",
                                            viewName === "hostkey-changed")
        else if (viewName.indexOf("export-") === 0) {
            var d = harness.codeExportDialog.get()
            d.language = ["python", "node", "java", "csharp"].indexOf(viewName.substring(7))
            d.openFor("db.getCollection('zips').find({ state: 'NY', city: /^BRO/i, pop: { $gt: NumberLong(1000) } },"
                      + " { city: 1, pop: 1 }).sort({ pop: -1 }).limit(10)", "samples", "")
        }
        else if (viewName === "log")
            harness.app.showLog()
        else if (viewName === "log-window")
            harness.app.setLogDetached(true)
        else if (viewName === "doc") {
            if (harness.app.narrow)
                harness.app.toggleExplorer()
            harness.connectionsPane.openDebugDocEditor()
        }
        else if (viewName === "create-collection" || viewName === "index" || viewName === "user" || viewName === "users") {
            if (harness.app.narrow)
                harness.app.toggleExplorer()
            harness.connectionsPane.openDebugDialog(viewName)
        }
        else if (viewName === "explorer")
            harness.app.toggleExplorer()
        else if (viewName === "calltip") {
            const page = harness.app.currentShellPage()
            if (page)
                page.debugShowCallTip()
        }
        else if (viewName === "tabs")
            harness.app.tabs.showDebugTabs(["users", "orders", "products", "events", "Monitor: rs0"])
        else if (viewName === "update-banner")
            UpdateService.previewDebugUpdate(false)
        else if (viewName === "mandatory-update")
            UpdateService.previewDebugUpdate(true)
    }

    // Layout check (--check-geometry): visible items reaching past window/dialog bounds.
    function geometryReport() {
        var out = []
        var hex = /\(0x[0-9a-f]+\)/
        function name(item) { return String(item).replace(hex, "").replace("QMLTYPE_", "") }
        function walk(item, depth, left, right, free) {
            if (!item.visible || item.opacity === 0 || depth > 80)
                return
            if (String(item).indexOf("PopupItem") >= 0 && item.width > 0) {
                left = Math.max(left, Math.round(item.mapToItem(null, 0, 0).x))
                right = Math.min(right, Math.round(item.mapToItem(null, item.width, 0).x))
            }
            if (item.placeholderText !== undefined && item.lineCount !== undefined)
                return
            var scrolled = free || (item.parent && item.parent.contentItem === item)
            if (!scrolled && item.width > 0 && item.height > 0) {
                var l = Math.round(item.mapToItem(null, 0, 0).x)
                var r = Math.round(item.mapToItem(null, item.width, 0).x)
                var label = item.text !== undefined ? "  \"" + String(item.text).slice(0, 30) + "\"" : ""
                if (r > right + 1)
                    out.push(name(item) + "  right=" + r + " > " + right + label)
                else if (l < left - 1)
                    out.push(name(item) + "  left=" + l + " < " + left + label)
            }
            if (item.truncated === true && item.parent && item.parent.parent
                    && String(item.parent.parent).indexOf("RagoPillButton") >= 0)
                out.push(name(item.parent.parent) + "  cut=\"" + String(item.text) + "\"")
            if (item.implicitWidth > right - left - 24 && item.children.length > 1 && String(item).indexOf("Layout") >= 0) {
                var wide = []
                for (var j = 0; j < item.children.length; ++j)
                    if (item.children[j].visible)
                        wide.push(name(item.children[j]) + "=" + Math.round(item.children[j].implicitWidth))
                out.push("  wants " + Math.round(item.implicitWidth) + ": " + name(item) + " [" + wide.join(", ") + "]")
            }
            if (item.contentX !== undefined && item.contentWidth > item.width + 1)
                free = true
            for (var i = 0; i < item.children.length; ++i)
                walk(item.children[i], depth + 1, left, right, free)
        }
        walk(harness.app.contentItem, 0, 0, harness.app.width, false)
        return out
    }
}
