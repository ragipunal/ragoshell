import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Command palette (docs/DesignSystem.md, section 6): one search field over every command of the
// registry (AppActions.qml) and over the saved connections and the databases and collections the
// explorer has loaded. Opens with Ctrl+K (Cmd+K), Ctrl+Shift+P or the Tools menu.
Popup {
    id: pal

    required property var app
    required property var actions

    readonly property real edgeMargin: 16
    readonly property int maxRecent: 8
    readonly property int maxResults: 40
    readonly property int rowHeight: Theme.touch ? Theme.menuItemHeight + 8 : 36

    // Flat list shown: { header: "title" } and { type: "command" | "target", ... } entries.
    property var rows: []
    property int current: -1

    parent: Overlay.overlay
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0
    // Capped to what safe areas and the on-screen keyboard leave of the window, so it never hides the rows.
    readonly property real visibleWidth: visibleArea.width
    readonly property real visibleHeight: visibleArea.height
    VisibleArea { id: visibleArea; reference: pal.parent }
    width: Math.min(720, visibleWidth - 2 * edgeMargin)
    height: Math.min(560, visibleHeight - 2 * edgeMargin)
    x: Math.round(visibleArea.left + Math.max(0, (visibleWidth - width) / 2))
    y: Math.round(visibleArea.top + (Theme.touch ? edgeMargin : Math.max(edgeMargin, Math.min(80, (visibleHeight - height) / 2))))

    Overlay.modal: Rectangle {
        color: Theme.dark ? "#99000000" : "#55000000"
    }

    background: Rectangle {
        radius: 16
        color: Theme.baseColor
        border.width: 1
        border.color: Theme.glassBorder
    }

    // ---- data ---------------------------------------------------------------

    property var _commands: []
    property var _targets: []

    function openPalette() {
        actions.refresh()
        _commands = actions.list().filter(function(c) { return c.id !== "tools.palette" })
        _targets = Backend.explorerTargets()
        search.text = ""
        _filter()
        open()
    }

    // Lower case without accents, so "ctrl" finds "Çalıştır"-style titles in any language, and a
    // Turkish dotless i matches an i.
    function _fold(text) {
        return String(text).normalize("NFD")
                .replace(/[̀-ًͯ-ٟ]/g, "")
                .toLowerCase()
                .replace(/ı/g, "i")
                .replace(/ß/g, "ss")
    }

    // Every word of the query must occur in the text; a word at the start of the text or of a
    // word of it counts more. A negative result means no match.
    function _score(text, tokens) {
        var total = 0
        for (var i = 0; i < tokens.length; ++i) {
            var at = text.indexOf(tokens[i])
            if (at < 0) {
                // Letters in order, anywhere ("nsh" finds "New Shell Tab").
                var from = 0
                var atWordStart = 0
                for (var c = 0; c < tokens[i].length; ++c) {
                    from = text.indexOf(tokens[i][c], from)
                    if (from < 0)
                        return -1
                    if (from === 0 || text[from - 1] === " ")
                        atWordStart++
                    from++
                }
                // Letters that start words ("nsh" for "New Shell Tab") count most.
                total += 1 + atWordStart * 10
                continue
            }
            var before = at === 0 ? " " : text[at - 1]
            total += at === 0 ? 100 : ((before === " " || before === "." || before === "-" || before === "_") ? 80 : 50)
            total -= at * 0.1
        }
        return total - text.length * 0.01
    }

    function _recentIds() {
        var saved = Backend.uiState("recentCommands")
        return saved ? String(saved).split(",").filter(function(id) { return id.length > 0 }) : []
    }

    function _remember(commandId) {
        var ids = _recentIds().filter(function(id) { return id !== commandId })
        ids.unshift(commandId)
        Backend.setUiState("recentCommands", ids.slice(0, maxRecent).join(","))
    }

    function _commandEntry(c) {
        return { type: "command", id: c.id, title: c.title, shortcut: c.shortcut, enabled: c.enabled,
                 note: c.enabled ? "" : c.disabledReason, section: actions.sectionTitle(c.section) }
    }

    function _targetEntry(t) {
        var detail
        if (t.kind === "connection")
            detail = t.connected ? qsTr("Open shell") : qsTr("Connect")
        else
            detail = qsTr("Query")
        var kind = t.kind === "connection" ? qsTr("Connection")
                   : (t.kind === "database" ? qsTr("Database") : qsTr("Collection"))
        return { type: "target", kind: t.kind, title: t.title, subtitle: t.subtitle, uuid: t.uuid, db: t.db,
                 collection: t.collection, connected: t.connected, shortcut: "", enabled: true,
                 note: t.subtitle, section: kind, detail: detail }
    }

    function _filter() {
        var query = search.text.trim()
        var list = []
        if (query.length === 0) {
            var byId = {}
            _commands.forEach(function(c) { byId[c.id] = c })
            var recent = _recentIds().filter(function(id) { return !!byId[id] }).slice(0, maxRecent)
            if (recent.length > 0) {
                list.push({ header: qsTr("Recent") })
                recent.forEach(function(id) { list.push(_commandEntry(byId[id])) })
            }
            var connections = _targets.filter(function(t) { return t.kind === "connection" })
            if (connections.length > 0) {
                list.push({ header: qsTr("Connections") })
                connections.slice(0, 8).forEach(function(t) { list.push(_targetEntry(t)) })
            }
            for (var s = 0; s < actions.sections.length; ++s) {
                var section = actions.sections[s].id
                var inSection = _commands.filter(function(c) { return c.section === section })
                if (inSection.length === 0)
                    continue
                list.push({ header: actions.sectionTitle(section) })
                inSection.forEach(function(c) { list.push(_commandEntry(c)) })
            }
        } else {
            var tokens = _fold(query).split(/\s+/).filter(function(t) { return t.length > 0 })
            var scored = []
            _commands.forEach(function(c) {
                var score = _score(_fold(c.title + " " + actions.sectionTitle(c.section)), tokens)
                if (score >= 0)
                    scored.push({ score: score + 5 + (c.enabled ? 3 : 0), entry: _commandEntry(c) })
            })
            _targets.forEach(function(t) {
                var score = _score(_fold(t.title + " " + t.subtitle), tokens)
                if (score >= 0)
                    scored.push({ score: score, entry: _targetEntry(t) })
            })
            scored.sort(function(a, b) { return b.score - a.score })
            scored.slice(0, maxResults).forEach(function(r) { list.push(r.entry) })
        }
        rows = list
        current = _step(-1, 1)
    }

    // The next selectable row after `from` in direction `dir` (1 or -1), or -1.
    function _step(from, dir) {
        var i = from + dir
        while (i >= 0 && i < rows.length) {
            if (!rows[i].header && rows[i].enabled)
                return i
            i += dir
        }
        return -1
    }

    function _move(dir) {
        var next = _step(current, dir)
        if (next >= 0) {
            current = next
            _reveal(next)
        }
    }

    // Scrolls the list so the row is fully visible.
    function _reveal(index) {
        var item = rowRepeater.itemAt(index)
        if (!item)
            return
        if (item.y < scroller.contentY)
            scroller.contentY = item.y
        else if (item.y + item.height > scroller.contentY + scroller.height)
            scroller.contentY = item.y + item.height - scroller.height
    }

    function activate(index) {
        var row = rows[index]
        if (!row || row.header || !row.enabled)
            return
        close()
        if (row.type === "command") {
            _remember(row.id)
            // After the popup has given the focus back, so shell tabs and dialogs take it properly.
            Qt.callLater(function() { actions.run(row.id) })
        } else {
            Qt.callLater(function() {
                if (row.kind === "connection" && !row.connected)
                    Backend.connectToServer(row.uuid)
                else if (row.kind === "collection")
                    Backend.openShell(row.uuid, row.db, row.collection)
                else
                    Backend.openShell(row.uuid, row.db, "")
            })
        }
    }

    // Dev/CI helper (--open-view palette:<text>).
    function setQuery(text) { search.text = text }

    onOpened: search.forceActiveFocus()

    // ---- view ----------------------------------------------------------------

    contentItem: ColumnLayout {
        spacing: 0

        RagoTextField {
            id: search
            Layout.fillWidth: true
            Layout.margins: 12
            placeholderText: qsTr("Search commands, connections, collections…")
            font.pixelSize: Theme.touch ? 16 : 14
            // The on-screen keyboard shows a "go" key instead of "done", which hides the keyboard only.
            EnterKey.type: Qt.EnterKeyGo
            onTextChanged: pal._filter()
            // Return and Enter (also from the keyboard's go key) arrive as accepted(), once.
            onAccepted: pal.activate(pal.current)
            Keys.onPressed: function(event) {
                switch (event.key) {
                case Qt.Key_Down: pal._move(1); event.accepted = true; break
                case Qt.Key_Up: pal._move(-1); event.accepted = true; break
                case Qt.Key_Tab: event.accepted = true; break
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.glassBorder
        }

        Flickable {
            id: scroller
            visible: pal.rows.length > 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: rowColumn.height
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            Column {
              id: rowColumn
              width: scroller.width
              Repeater {
               id: rowRepeater
               model: pal.rows

               delegate: Item {
                id: row
                required property int index
                required property var modelData
                readonly property bool isHeader: !!modelData.header
                readonly property bool selected: index === pal.current

                width: rowColumn.width
                height: isHeader ? 30 : pal.rowHeight

                Label {
                    visible: row.isHeader
                    anchors.left: parent.left
                    anchors.leftMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.isHeader ? row.modelData.header.toUpperCase() : ""
                    font.pixelSize: 11
                    font.letterSpacing: 1
                    color: Theme.disabledTextColor
                }

                Rectangle {
                    visible: !row.isHeader
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    radius: 8
                    color: row.selected ? Theme.selectedColor : (hover.hovered && row.modelData.enabled ? Theme.hoverColor : "transparent")
                }

                RowLayout {
                    visible: !row.isHeader
                    anchors.fill: parent
                    anchors.leftMargin: 18
                    anchors.rightMargin: 18
                    spacing: 12
                    opacity: !row.isHeader && row.modelData.enabled ? 1.0 : 0.45

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            text: row.isHeader ? "" : row.modelData.title
                            font.pixelSize: Theme.touch ? 15 : 13
                            elide: Text.ElideRight
                            color: Theme.textColor
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: !row.isHeader && row.modelData.note.length > 0
                            text: row.isHeader ? "" : row.modelData.note
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            color: Theme.disabledTextColor
                        }
                    }

                    Label {
                        visible: !row.isHeader && row.modelData.type === "target"
                        text: row.isHeader ? "" : row.modelData.section
                        font.pixelSize: 11
                        color: Theme.disabledTextColor
                    }
                    Label {
                        visible: !row.isHeader && row.modelData.shortcut.length > 0 && Platform.shortcutsVisible && !Theme.touch
                        text: row.isHeader ? "" : row.modelData.shortcut
                        font.pixelSize: 11
                        color: Theme.disabledTextColor
                    }
                }

                HoverHandler {
                    id: hover
                    enabled: !row.isHeader
                    onHoveredChanged: {
                        if (hovered && !Theme.touch && row.modelData.enabled)
                            pal.current = row.index
                    }
                }
                TapHandler {
                    enabled: !row.isHeader
                    onTapped: pal.activate(row.index)
                }
               }
              }
            }

        }

        Label {
            visible: pal.rows.length === 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            text: qsTr("No matches")
            color: Theme.disabledTextColor
        }
    }
}
