pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Live Server Monitor & Active Operations Manager.
// Backed by native C++ ServerMonitorController (serverStatus + currentOp + killOp).
Item {
    id: monPage
    property ServerMonitorController monitor

    property string _pendingKillOpId: ""
    property string _pendingKillNs: ""
    property string _toastText: ""
    property bool _toastError: false

    readonly property string baseServerUuid: {
        if (!monPage.monitor || !monPage.monitor.serverUuid)
            return ""
        var raw = String(monPage.monitor.serverUuid)
        var atIdx = raw.indexOf("@")
        return atIdx > 0 ? raw.substring(0, atIdx) : raw
    }
    readonly property string serverColorTag: baseServerUuid.length > 0
                                             ? ConnectionModel.colorTagForUuid(baseServerUuid)
                                             : ""

    function formatRate(val) {
        var n = Number(val || 0)
        if (n >= 100) return n.toFixed(0)
        if (n >= 10)  return n.toFixed(1)
        return n.toFixed(1)
    }

    function formatKbRate(kbps) {
        var k = Number(kbps || 0)
        if (k >= 1024)
            return (k / 1024.0).toFixed(2) + " MB/s"
        return k.toFixed(1) + " KB/s"
    }

    // Suspend auto-refresh while this tab is not shown, or the window is
    // minimized / hidden to tray.
    readonly property bool _onScreen: monPage.visible && Window.window !== null
                                      && Window.window.visibility !== Window.Minimized
                                      && Window.window.visibility !== Window.Hidden
    Binding {
        target: monPage.monitor
        property: "active"
        value: monPage._onScreen
        when: !!monPage.monitor
    }

    Connections {
        target: monPage.monitor
        function onMetricsUpdated() {
            opsCanvas.requestPaint()
            netCanvas.requestPaint()
        }
        function onOperationKilled(ok, message) {
            monPage._toastError = !ok
            monPage._toastText = message
            toastTimer.restart()
        }
    }

    Timer {
        id: toastTimer
        interval: 4500
        onTriggered: monPage._toastText = ""
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- Top Context & Refresh Control Bar -----------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 42
            color: Theme.surfaceColor
            border.width: 1
            border.color: Theme.glassBorder

            HScrollRow {
                anchors.fill: parent
                margin: 12
                spacing: 8

                RagoBadge {
                    text: "⛁ " + (monPage.monitor ? monPage.monitor.host : "")
                    badgeColor: monPage.serverColorTag.length > 0 ? monPage.serverColorTag : Theme.accentColor
                }

                RagoBadge {
                    visible: monPage.monitor ? monPage.monitor.role.length > 0 : false
                    text: monPage.monitor
                          ? (monPage.monitor.replSetName.length > 0
                             ? monPage.monitor.replSetName + " · " + monPage.monitor.role
                             : monPage.monitor.role)
                          : ""
                    badgeColor: monPage.monitor && monPage.monitor.role === "PRIMARY"
                                ? Theme.accentColor
                                : "#3b9eff"
                }

                RagoBadge {
                    visible: monPage.monitor ? monPage.monitor.version.length > 0 : false
                    text: "MongoDB " + (monPage.monitor ? monPage.monitor.version : "")
                    badgeColor: "#d49a35"
                }

                RagoBadge {
                    visible: monPage.monitor ? monPage.monitor.uptimeSec > 0 : false
                    text: qsTr("Uptime: %1").arg(monPage.monitor ? monPage.monitor.uptimeText : "—")
                    badgeColor: Theme.disabledTextColor
                }

                Item { Layout.fillWidth: true }

                BusyIndicator {
                    visible: running
                    running: monPage.monitor ? monPage.monitor.busy : false
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                }

                Label {
                    visible: monPage.monitor ? monPage.monitor.lastUpdated.length > 0 : false
                    text: monPage.monitor ? monPage.monitor.lastUpdated : ""
                    font.family: AppSettings.codeFontFamily
                    font.pixelSize: 11
                    color: Theme.disabledTextColor
                }

                Label {
                    text: qsTr("Interval:")
                    font.pixelSize: 11
                    color: Theme.disabledTextColor
                }

                RagoComboBox {
                    id: intervalCombo
                    implicitWidth: 96
                    implicitHeight: 28
                    model: ["1s", "2s", "5s", "10s", qsTr("Paused")]
                    currentIndex: {
                        // Language.effective: re-run after a language switch resets the combo
                        var lang = Language.effective
                        var ms = monPage.monitor ? monPage.monitor.refreshIntervalMs : 2000
                        if (ms === 0) return 4
                        if (ms <= 1000) return 0
                        if (ms <= 2000) return 1
                        if (ms <= 5000) return 2
                        return 3
                    }
                    onActivated: function(idx) {
                        if (!monPage.monitor) return
                        var vals = [1000, 2000, 5000, 10000, 0]
                        monPage.monitor.refreshIntervalMs = vals[idx]
                    }
                }

                RagoPillButton {
                    text: qsTr("Refresh")
                    iconText: "↻"
                    implicitHeight: 28
                    onClicked: if (monPage.monitor) monPage.monitor.refreshNow()
                }

                RagoPillButton {
                    text: qsTr("Reset Charts")
                    implicitHeight: 28
                    onClicked: if (monPage.monitor) monPage.monitor.clearHistory()
                }
            }
        }

        // ---- Optional Error / Status / Toast Banner ------------------------
        Rectangle {
            visible: (monPage.monitor && (monPage.monitor.errorText.length > 0 || monPage.monitor.statusNote.length > 0)) ||
                     monPage._toastText.length > 0
            Layout.fillWidth: true
            implicitHeight: 30
            color: (monPage.monitor && monPage.monitor.errorText.length > 0) || monPage._toastError
                   ? Qt.rgba(1, 0.32, 0.32, 0.14)
                   : Qt.rgba(0.23, 0.62, 1.0, 0.12)
            border.width: 1
            border.color: Theme.glassBorder

            Label {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                font.pixelSize: 12
                color: (monPage.monitor && monPage.monitor.errorText.length > 0) || monPage._toastError
                       ? "#ff6b6b"
                       : Theme.textColor
                text: monPage._toastText.length > 0
                      ? monPage._toastText
                      : (monPage.monitor && monPage.monitor.errorText.length > 0
                         ? monPage.monitor.errorText
                         : (monPage.monitor ? monPage.monitor.statusNote : ""))
            }
        }

        // ---- Main Split: Upper Live Metrics/Charts, Lower Current Ops ------
        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Vertical

            handle: Rectangle {
                implicitHeight: 5
                color: SplitHandle.pressed || SplitHandle.hovered
                       ? Theme.accentColor : Theme.glassBorder
            }

            // ================================================================
            // Upper Pane: 4 KPI Cards + 2 Live Time-Series Charts
            // ================================================================
            Rectangle {
                SplitView.preferredHeight: monPage.width < 720 ? 640 : 305
                SplitView.minimumHeight: 215
                color: Theme.windowColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 10

                    // 4 KPI Cards (two by two on a narrow screen)
                    GridLayout {
                        Layout.fillWidth: true
                        columns: monPage.width < 720 ? 2 : 4
                        columnSpacing: 10
                        rowSpacing: 10

                        // KPI 1: Operations / sec
                        RagoGlassCard {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            implicitHeight: 86

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 4

                                Label {
                                    text: qsTr("OPERATIONS / SEC")
                                    font.pixelSize: 10
                                    font.bold: true
                                    font.letterSpacing: Language.rtl ? 0 : 0.9
                                    color: Theme.disabledTextColor
                                }
                                Label {
                                    text: qsTr("%1 ops/s").arg(monPage.monitor ? monPage.formatRate(monPage.monitor.opsTotalRate) : "0.0")
                                    font.family: AppSettings.codeFontFamily
                                    font.pixelSize: 18
                                    font.bold: true
                                    color: Theme.accentColor
                                }
                                Label {
                                    text: monPage.monitor
                                          ? ("Q:" + monPage.formatRate(monPage.monitor.opsQueryRate) +
                                             "  I:" + monPage.formatRate(monPage.monitor.opsInsertRate) +
                                             "  U:" + monPage.formatRate(monPage.monitor.opsUpdateRate) +
                                             "  D:" + monPage.formatRate(monPage.monitor.opsDeleteRate) +
                                             "  C:" + monPage.formatRate(monPage.monitor.opsCommandRate))
                                          : "—"
                                    font.family: AppSettings.codeFontFamily
                                    font.pixelSize: 11
                                    color: Theme.disabledTextColor
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                }
                            }
                        }

                        // KPI 2: Connections
                        RagoGlassCard {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            implicitHeight: 86

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 4

                                Label {
                                    text: qsTr("CONNECTIONS")
                                    font.pixelSize: 10
                                    font.bold: true
                                    font.letterSpacing: Language.rtl ? 0 : 0.9
                                    color: Theme.disabledTextColor
                                }
                                Label {
                                    text: monPage.monitor ? String(monPage.monitor.currentConnections) : "0"
                                    font.family: AppSettings.codeFontFamily
                                    font.pixelSize: 18
                                    font.bold: true
                                    color: "#3b9eff"
                                }
                                Label {
                                    text: monPage.monitor
                                          ? qsTr("Avail: %1  ·  Active clients: %2")
                                                .arg(monPage.monitor.availableConnections)
                                                .arg(monPage.monitor.activeClients)
                                          : "—"
                                    font.family: AppSettings.codeFontFamily
                                    font.pixelSize: 11
                                    color: Theme.disabledTextColor
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                }
                            }
                        }

                        // KPI 3: Network I/O
                        RagoGlassCard {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            implicitHeight: 86

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 4

                                Label {
                                    text: qsTr("NETWORK TRAFFIC")
                                    font.pixelSize: 10
                                    font.bold: true
                                    font.letterSpacing: Language.rtl ? 0 : 0.9
                                    color: Theme.disabledTextColor
                                }
                                Label {
                                    text: monPage.monitor
                                          ? ("↓ " + monPage.formatKbRate(monPage.monitor.netInKbps) +
                                             "  ↑ " + monPage.formatKbRate(monPage.monitor.netOutKbps))
                                          : "↓ 0.0 KB/s  ↑ 0.0 KB/s"
                                    font.family: AppSettings.codeFontFamily
                                    font.pixelSize: 15
                                    font.bold: true
                                    color: Theme.textColor
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                }
                                Label {
                                    text: monPage.monitor
                                          ? qsTr("Requests: %1 / sec").arg(monPage.formatRate(monPage.monitor.netRequestsRate))
                                          : "—"
                                    font.family: AppSettings.codeFontFamily
                                    font.pixelSize: 11
                                    color: Theme.disabledTextColor
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                }
                            }
                        }

                        // KPI 4: Memory & WiredTiger Cache
                        RagoGlassCard {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            implicitHeight: 86

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 4

                                Label {
                                    text: qsTr("MEMORY & WT CACHE")
                                    font.pixelSize: 10
                                    font.bold: true
                                    font.letterSpacing: Language.rtl ? 0 : 0.9
                                    color: Theme.disabledTextColor
                                }
                                Label {
                                    text: qsTr("%1 MB Res").arg(monPage.monitor ? Math.round(monPage.monitor.residentMb) : 0)
                                    font.family: AppSettings.codeFontFamily
                                    font.pixelSize: 18
                                    font.bold: true
                                    color: "#f59e0b"
                                }
                                Label {
                                    text: {
                                        if (!monPage.monitor) return "—"
                                        if (monPage.monitor.wtCacheMaxMb > 0) {
                                            var pct = Math.round((monPage.monitor.wtCacheUsedMb / monPage.monitor.wtCacheMaxMb) * 100)
                                            return qsTr("WT: %1/%2 MB (%3%)").arg(Math.round(monPage.monitor.wtCacheUsedMb))
                                                   .arg(Math.round(monPage.monitor.wtCacheMaxMb)).arg(pct)
                                        }
                                        return qsTr("Virtual: %1 MB").arg(Math.round(monPage.monitor.virtualMb))
                                    }
                                    font.family: AppSettings.codeFontFamily
                                    font.pixelSize: 11
                                    color: Theme.disabledTextColor
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                        }
                    }

                    // 2 Live Time-Series Charts (one above the other on a narrow screen)
                    GridLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        columns: monPage.width < 720 ? 1 : 2
                        columnSpacing: 10
                        rowSpacing: 10

                        // Chart 1: Operations / sec Breakdown
                        RagoGlassCard {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: 1

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 6

                                // The legend wraps below the title on a narrow card.
                                Flow {
                                    Layout.fillWidth: true
                                    spacing: 10
                                    Label {
                                        text: qsTr("Operations / sec")
                                        font.pixelSize: 12
                                        font.bold: true
                                        color: Theme.textColor
                                    }
                                    //: Chart legend: query operations per second (MongoDB opcounters).
                                    Label { text: "● " + qsTr("Query");   font.pixelSize: 10; color: "#3b9eff" }
                                    //: Chart legend: insert operations per second (MongoDB opcounters).
                                    Label { text: "● " + qsTr("Insert");  font.pixelSize: 10; color: "#00e668" }
                                    //: Chart legend: update operations per second (MongoDB opcounters).
                                    Label { text: "● " + qsTr("Update");  font.pixelSize: 10; color: "#f59e0b" }
                                    //: Chart legend: delete operations per second (MongoDB opcounters).
                                    Label { text: "● " + qsTr("Delete");  font.pixelSize: 10; color: "#ff6b6b" }
                                    //: Chart legend: server commands per second (MongoDB opcounters), not a shell command.
                                    Label { text: "● " + qsTr("Command"); font.pixelSize: 10; color: "#a78bfa" }
                                }

                                Canvas {
                                    id: opsCanvas
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    antialiasing: true
                                    onWidthChanged: requestPaint()
                                    onHeightChanged: requestPaint()

                                    onPaint: {
                                        var ctx = getContext("2d")
                                        ctx.reset()
                                        var w = width, h = height
                                        if (w <= 10 || h <= 10) return

                                        var samples = monPage.monitor ? monPage.monitor.historySamples : []
                                        var gridCol = Theme.dark ? "rgba(255,255,255,0.07)" : "rgba(0,0,0,0.07)"
                                        ctx.strokeStyle = gridCol
                                        ctx.lineWidth = 1
                                        for (var g = 1; g <= 3; ++g) {
                                            var gy = Math.round((h * g) / 4) + 0.5
                                            ctx.beginPath()
                                            ctx.moveTo(0, gy)
                                            ctx.lineTo(w, gy)
                                            ctx.stroke()
                                        }

                                        if (!samples || samples.length < 2)
                                            return

                                        var seriesDefs = [
                                            { key: "query",   color: "#3b9eff" },
                                            { key: "insert",  color: "#00e668" },
                                            { key: "update",  color: "#f59e0b" },
                                            { key: "del",     color: "#ff6b6b" },
                                            { key: "command", color: "#a78bfa" }
                                        ]

                                        var maxVal = 5.0
                                        for (var i = 0; i < samples.length; ++i) {
                                            for (var s = 0; s < seriesDefs.length; ++s) {
                                                var v = Number(samples[i][seriesDefs[s].key] || 0)
                                                if (v > maxVal) maxVal = v
                                            }
                                        }
                                        maxVal *= 1.15

                                        var padY = 6
                                        var plotH = h - padY * 2
                                        var stepX = w / Math.max(1, 59)
                                        var startX = w - (samples.length - 1) * stepX

                                        for (var k = 0; k < seriesDefs.length; ++k) {
                                            ctx.beginPath()
                                            ctx.strokeStyle = seriesDefs[k].color
                                            ctx.lineWidth = 1.8
                                            for (var j = 0; j < samples.length; ++j) {
                                                var x = startX + j * stepX
                                                var val = Number(samples[j][seriesDefs[k].key] || 0)
                                                var y = padY + plotH - (val / maxVal) * plotH
                                                if (j === 0) ctx.moveTo(x, y)
                                                else ctx.lineTo(x, y)
                                            }
                                            ctx.stroke()
                                        }
                                    }
                                }
                            }
                        }

                        // Chart 2: Network I/O (KB/s)
                        RagoGlassCard {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: 1

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 6

                                // The legend wraps below the title on a narrow card.
                                Flow {
                                    Layout.fillWidth: true
                                    spacing: 10
                                    Label {
                                        text: qsTr("Network Throughput (KB/s)")
                                        font.pixelSize: 12
                                        font.bold: true
                                        color: Theme.textColor
                                    }
                                    //: Chart legend: network bytes received by the server.
                                    Label { text: "● " + qsTr("Net In");  font.pixelSize: 10; color: "#00e668" }
                                    //: Chart legend: network bytes sent by the server.
                                    Label { text: "● " + qsTr("Net Out"); font.pixelSize: 10; color: "#38bdf8" }
                                }

                                Canvas {
                                    id: netCanvas
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    antialiasing: true
                                    onWidthChanged: requestPaint()
                                    onHeightChanged: requestPaint()

                                    onPaint: {
                                        var ctx = getContext("2d")
                                        ctx.reset()
                                        var w = width, h = height
                                        if (w <= 10 || h <= 10) return

                                        var samples = monPage.monitor ? monPage.monitor.historySamples : []
                                        var gridCol = Theme.dark ? "rgba(255,255,255,0.07)" : "rgba(0,0,0,0.07)"
                                        ctx.strokeStyle = gridCol
                                        ctx.lineWidth = 1
                                        for (var g = 1; g <= 3; ++g) {
                                            var gy = Math.round((h * g) / 4) + 0.5
                                            ctx.beginPath()
                                            ctx.moveTo(0, gy)
                                            ctx.lineTo(w, gy)
                                            ctx.stroke()
                                        }

                                        if (!samples || samples.length < 2)
                                            return

                                        var maxVal = 10.0
                                        for (var i = 0; i < samples.length; ++i) {
                                            var vi = Number(samples[i].netInKbps || 0)
                                            var vo = Number(samples[i].netOutKbps || 0)
                                            if (vi > maxVal) maxVal = vi
                                            if (vo > maxVal) maxVal = vo
                                        }
                                        maxVal *= 1.15

                                        var padY = 6
                                        var plotH = h - padY * 2
                                        var stepX = w / Math.max(1, 59)
                                        var startX = w - (samples.length - 1) * stepX

                                        var netSeries = [
                                            { key: "netInKbps",  color: "#00e668" },
                                            { key: "netOutKbps", color: "#38bdf8" }
                                        ]
                                        for (var k = 0; k < netSeries.length; ++k) {
                                            ctx.beginPath()
                                            ctx.strokeStyle = netSeries[k].color
                                            ctx.lineWidth = 1.8
                                            for (var j = 0; j < samples.length; ++j) {
                                                var x = startX + j * stepX
                                                var val = Number(samples[j][netSeries[k].key] || 0)
                                                var y = padY + plotH - (val / maxVal) * plotH
                                                if (j === 0) ctx.moveTo(x, y)
                                                else ctx.lineTo(x, y)
                                            }
                                            ctx.stroke()
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ================================================================
            ServerMonitorOpsPane {
                monPage: monPage
            }
        }
    }
}
