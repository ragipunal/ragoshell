pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// OS-aware Keyboard Shortcuts reference modal (F1 / Ctrl+? / Cmd+?).
RagoDialog {
    id: dlg

    // The reference is a keyboard topic: keys are always written, in this platform's spelling (Ctrl+ or the
    // Apple symbols); the command itself is hidden from the palette on a touch device without a keyboard.
    function k(sequence) { return Platform.keyLabel(sequence) }
    property string filterText: ""

    title: qsTr("Keyboard Shortcuts")
    acceptText: qsTr("Close")
    showReject: false
    width: Math.min(visibleWidth - 24, 660)
    height: Math.min(visibleHeight - 24, 560)

    readonly property var shortcutSections: [
        {
            "category": qsTr("Query Execution & IntelliShell"),
            "items": [
                { "keys": k("Ctrl+Enter / Ctrl+R / F5"), "desc": qsTr("Execute script (or selected text if highlighted)") },
                { "keys": k("Ctrl+Shift+Enter"), "desc": qsTr("Execute entire script (ignore selection)") },
                { "keys": k("F6 / Ctrl+."), "desc": qsTr("Stop / interrupt running query") },
                { "keys": k("Ctrl+Space"), "desc": qsTr("Trigger autocomplete suggestions") },
                { "keys": k("Ctrl+/"), "desc": qsTr("Toggle line comment (//)") },
                { "keys": k("Alt+Shift+F"), "desc": qsTr("Format JSON / code in editor") },
                { "keys": k("Ctrl+F"), "desc": qsTr("Find in editor") },
                { "keys": k("Ctrl+Shift+F"), "desc": qsTr("Toggle visual Collection Query Bar") }
            ]
        },
        {
            "category": qsTr("Documents & Result Views"),
            "items": [
                { "keys": k("Ctrl+D"), "desc": qsTr("Insert new document into active collection") },
                { "keys": qsTr("Enter  /  Double-Click"), "desc": qsTr("Edit scalar cell in-place or open Document Editor") },
                { "keys": k(Platform.usesCommandKey ? "Ctrl+Backspace / Del" : "Del"), "desc": qsTr("Delete selected document(s) with confirmation") },
                { "keys": k("Ctrl+C"), "desc": qsTr("Copy selected field value") },
                { "keys": k("Ctrl+Shift+C"), "desc": qsTr("Copy full document JSON") },
                { "keys": k("F2 / Alt+Shift+1"), "desc": qsTr("Switch to Tree View") },
                { "keys": k("F3 / Alt+Shift+2"), "desc": qsTr("Switch to Table View") },
                { "keys": k("F4 / Alt+Shift+3"), "desc": qsTr("Switch to Colored JSON Text View") },
                { "keys": k("F10 / Alt+Shift+O"), "desc": qsTr("Toggle horizontal / vertical editor-results split") }
            ]
        },
        {
            "category": qsTr("Connections, Tabs & Workspace"),
            "items": [
                { "keys": k("Ctrl+N"), "desc": qsTr("Open Connections / New Connection dialog") },
                { "keys": k("Ctrl+T"), "desc": qsTr("Open new Shell tab on connected server") },
                { "keys": k("Ctrl+Shift+T"), "desc": qsTr("Duplicate current Shell context in new tab") },
                { "keys": k("Ctrl+Shift+M"), "desc": qsTr("Open live Server Monitor & Current Operations") },
                { "keys": k("Ctrl+K / Ctrl+Shift+P"), "desc": qsTr("Command Palette: search every command, connection and collection") },
                { "keys": k("Ctrl+H"), "desc": qsTr("Query History & Snippets") },
                { "keys": k("Ctrl+Shift+A"), "desc": qsTr("Aggregation Builder for the current collection") },
                { "keys": k("Ctrl+Shift+E"), "desc": qsTr("Export query (selection or script) to Python / Node.js / Java / C#") },
                { "keys": k("Ctrl+O"), "desc": qsTr("Load JavaScript (.js) file into current tab") },
                { "keys": k("Ctrl+S / Ctrl+Shift+S"), "desc": qsTr("Save / Save As JavaScript (.js) file") },
                { "keys": k("Ctrl+W"), "desc": qsTr("Close active tab") },
                { "keys": k("Ctrl+Tab / Ctrl+Shift+Tab"), "desc": qsTr("Next / Previous tab") },
                { "keys": k(Platform.usesCommandKey ? "Ctrl+1" : "Alt+1") + " … " + k(Platform.usesCommandKey ? "Ctrl+9" : "Alt+9"), "desc": qsTr("Jump directly to tab 1–9") },
                { "keys": k("Ctrl+B / Ctrl+E"), "desc": qsTr("Toggle Explorer side panel") },
                { "keys": k("Ctrl+L"), "desc": qsTr("Toggle bottom Log panel") },
                { "keys": k("Ctrl+,"), "desc": qsTr("Open Preferences") },
                { "keys": k(Platform.usesCommandKey ? "Meta+Ctrl+F / F11" : "F11"), "desc": qsTr("Toggle Full Screen") },
                { "keys": "F1", "desc": qsTr("Show this Keyboard Shortcuts reference") }
            ]
        }
    ]

    onOpened: {
        searchField.text = ""
        searchField.forceActiveFocus()
    }

    contentItem: ColumnLayout {
        spacing: 10

        RagoTextField {
            id: searchField
            placeholderText: qsTr("Filter shortcuts (e.g. Execute, JSON, Delete, Tab)…")
            Layout.fillWidth: true
            onTextChanged: dlg.filterText = text.trim().toLowerCase()
        }

        ScrollView {
            id: shortcutScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            // Rows span the dialog; the content item alone would shrink to fit.
            contentWidth: availableWidth

            ColumnLayout {
                width: shortcutScroll.availableWidth
                spacing: 14

                Repeater {
                    model: dlg.shortcutSections
                    ColumnLayout {
                        id: section
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 6

                        readonly property var filteredItems: {
                            var q = dlg.filterText
                            var src = section.modelData.items || []
                            if (q.length === 0)
                                return src
                            var out = []
                            for (var i = 0; i < src.length; ++i) {
                                if (String(src[i].keys).toLowerCase().indexOf(q) >= 0 ||
                                    String(src[i].desc).toLowerCase().indexOf(q) >= 0) {
                                    out.push(src[i])
                                }
                            }
                            return out
                        }

                        visible: filteredItems.length > 0

                        Label {
                            text: section.modelData.category
                            font.pixelSize: 12
                            font.bold: true
                            color: Theme.accentColor
                        }

                        Repeater {
                            model: section.filteredItems
                            Rectangle {
                                id: shortcutRow
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: 32
                                radius: 7
                                color: Theme.surfaceColor
                                border.width: 1
                                border.color: Theme.glassBorder

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    spacing: 12

                                    Label {
                                        text: shortcutRow.modelData.desc
                                        font.pixelSize: 12
                                        color: Theme.textColor
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                    }

                                    Rectangle {
                                        radius: 5
                                        color: Theme.glassBackground
                                        border.width: 1
                                        border.color: Theme.glassBorderHover
                                        implicitWidth: keyLabel.implicitWidth + 14
                                        implicitHeight: 22

                                        Label {
                                            id: keyLabel
                                            anchors.centerIn: parent
                                            text: shortcutRow.modelData.keys
                                            font.family: AppSettings.codeFontFamily
                                            font.pixelSize: 11
                                            font.bold: true
                                            color: Theme.accentColor
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
