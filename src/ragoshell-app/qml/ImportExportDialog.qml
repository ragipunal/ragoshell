import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs as QtDialogs

// Native C++ Collection Export & Import modal (JSON Array, NDJSON, CSV),
// with explicit danger warning + confirmation guard when dropping target collection before import.
RagoDialog {
    id: dlg

    property string serverUuid: ""
    property string serverLabel: ""
    property string serverColorTag: ""
    property string dbName: ""
    property string collName: ""
    property bool isImportMode: false
    property string statusMsg: ""
    property bool statusIsError: false
    // The path a running export writes to; on WASM a generated MEMFS name that
    // gets browser-downloaded when the transfer finishes (docs/WebStorage.md §3).
    property string exportPath: ""

    title: isImportMode ? qsTr("Import Data into Collection") : qsTr("Export Collection Data")
    acceptText: isImportMode
                ? (dropBeforeCheck.checked ? qsTr("Drop & Start Import…") : qsTr("Start Import"))
                : qsTr("Start Export")
    dangerAccept: isImportMode && dropBeforeCheck.checked
    rejectText: Backend.transferBusy ? qsTr("Cancel Transfer") : qsTr("Close")
    showReject: true
    width: Math.min(visibleWidth - 24, 580)
    height: Math.min(visibleHeight - 24, 495)

    function extensionForFormat(fmtIdx) {
        if (fmtIdx === 1) return "ndjson"
        if (fmtIdx === 2) return "csv"
        return "json"
    }

    function _loadServerMeta(uuid) {
        serverUuid = uuid || ""
        serverLabel = Backend.serverAddress(serverUuid)
        var _rev = ConnectionModel.revision
        serverColorTag = ConnectionModel.colorTagForUuid(serverUuid)
    }

    function openExport(uuid, db, coll, initialFilter) {
        _loadServerMeta(uuid)
        dbName = db
        collName = coll
        isImportMode = false
        statusMsg = ""
        statusIsError = false
        formatCombo.currentIndex = 0
        filePathField.text = ""
        filterField.text = initialFilter && initialFilter.length > 0 ? initialFilter : "{}"
        limitField.text = ""
        open()
    }

    function openImport(uuid, db, coll) {
        _loadServerMeta(uuid)
        dbName = db
        collName = coll
        isImportMode = true
        statusMsg = ""
        statusIsError = false
        formatCombo.currentIndex = 0
        filePathField.text = ""
        dropBeforeCheck.checked = false
        stopOnErrorCheck.checked = false
        open()
    }

    function _executeImportNow() {
        var path = filePathField.text.trim()
        statusIsError = false
        statusMsg = ""
        Backend.startImportCollection(serverUuid, dbName, collName, path,
                                      formatCombo.currentIndex,
                                      dropBeforeCheck.checked,
                                      stopOnErrorCheck.checked)
    }

    onRejected: {
        if (Backend.transferBusy)
            Backend.cancelTransfer()
    }

    beforeAccept: function() {
        if (Backend.transferBusy)
            return false

        var path = filePathField.text.trim()
        if (path.length === 0) {
            if (Qt.platform.os === "wasm" && !isImportMode) {
                // No real filesystem to pick from: export to MEMFS, then the
                // finished transfer downloads it (onTransferFinished below).
                path = "/tmp/" + collName + "." + extensionForFormat(formatCombo.currentIndex)
            } else {
                statusIsError = true
                statusMsg = qsTr("Please choose a file path first.")
                return false
            }
        }

        if (!isImportMode) {
            let flt = filterField.text.trim()
            if (flt.length > 0 && flt !== "{}") {
                let fErr = Backend.validateJson(flt, true)
                if (fErr.length > 0) {
                    statusIsError = true
                    statusMsg = qsTr("Invalid Filter JSON: %1").arg(Language.isolate(fErr))
                    return false
                }
            }
            let lim = parseInt(limitField.text.trim(), 10) || 0
            statusIsError = false
            statusMsg = ""
            dlg.exportPath = path
            Backend.startExportCollection(serverUuid, dbName, collName, path,
                                          formatCombo.currentIndex, flt, lim)
        } else {
            if (dropBeforeCheck.checked) {
                dropImportConfirm.open()
                return false
            }
            _executeImportNow()
        }
        // Keep dialog open while transfer runs; close on finish or let user inspect count
        return false
    }

    Connections {
        target: Backend
        function onTransferFinished(success, count, message) {
            if (!dlg.visible)
                return
            dlg.statusIsError = !success
            dlg.statusMsg = message
            // WASM export: the file sits in MEMFS — hand it to the browser.
            if (success && !dlg.isImportMode && Qt.platform.os === "wasm"
                    && dlg.exportPath.length > 0)
                Backend.downloadFile(dlg.exportPath)
        }
    }

    QtDialogs.FileDialog {
        id: exportFileDialog
        fileMode: QtDialogs.FileDialog.SaveFile
        defaultSuffix: dlg.extensionForFormat(formatCombo.currentIndex)
        nameFilters: [
            qsTr("JSON Array (*.json)"),
            qsTr("Newline-Delimited JSON (*.ndjson *.jsonl)"),
            qsTr("CSV (*.csv)"),
            qsTr("All files (*)")
        ]
        onAccepted: filePathField.text = Backend.urlToLocalFile(selectedFile)
    }

    QtDialogs.FileDialog {
        id: importFileDialog
        fileMode: QtDialogs.FileDialog.OpenFile
        nameFilters: [
            qsTr("Data files (*.json *.ndjson *.jsonl *.csv)"),
            qsTr("JSON files (*.json *.ndjson *.jsonl)"),
            qsTr("CSV files (*.csv)"),
            qsTr("All files (*)")
        ]
        onAccepted: {
            var p = Backend.urlToLocalFile(selectedFile)
            filePathField.text = p
            var lower = p.toLowerCase()
            if (lower.endsWith(".csv"))
                formatCombo.currentIndex = 2
            else if (lower.endsWith(".ndjson") || lower.endsWith(".jsonl"))
                formatCombo.currentIndex = 1
            else if (lower.endsWith(".json"))
                formatCombo.currentIndex = 0
        }
    }

    RagoDialog {
        id: dropImportConfirm
        title: qsTr("Confirm Drop Collection & Import")
        //: Permanently deletes it from the server (MongoDB drop). Not drag and drop.
        acceptText: qsTr("Drop Collection & Import")
        rejectText: qsTr("Cancel")
        showReject: true
        dangerAccept: true
        width: Math.min(visibleWidth - 24, 460)

        contentItem: ColumnLayout {
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                RagoBadge {
                    visible: dlg.serverLabel.length > 0
                    text: "⛁ " + dlg.serverLabel
                    badgeColor: dlg.serverColorTag.length > 0
                                ? dlg.serverColorTag
                                : Theme.disabledTextColor
                }
                RagoBadge {
                    text: dlg.dbName + "." + dlg.collName
                    badgeColor: "#ef4444"
                }
                Item { Layout.fillWidth: true }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: dropConfirmWarn.implicitHeight + 18
                radius: 8
                color: Theme.dark ? Qt.rgba(0.94, 0.27, 0.27, 0.14) : Qt.rgba(0.94, 0.27, 0.27, 0.09)
                border.width: 1
                border.color: Qt.rgba(0.94, 0.27, 0.27, 0.55)

                Label {
                    id: dropConfirmWarn
                    anchors.fill: parent
                    anchors.margins: 9
                    text: qsTr("⚠ Target collection '%1.%2' — including ALL existing documents and custom indexes — will be permanently dropped before importing the file. This action cannot be undone.")
                          .arg(Language.isolate(dlg.dbName)).arg(Language.isolate(dlg.collName))
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: Theme.textColor
                }
            }
        }

        onAccepted: dlg._executeImportNow()
    }

    contentItem: ColumnLayout {
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            RagoBadge {
                visible: dlg.serverLabel.length > 0
                text: "⛁ " + dlg.serverLabel
                badgeColor: dlg.serverColorTag.length > 0
                            ? dlg.serverColorTag
                            : Theme.disabledTextColor
            }

            RagoBadge {
                text: dlg.dbName + "." + dlg.collName
                badgeColor: Theme.accentColor
            }

            Item { Layout.fillWidth: true }

            RagoPillButton {
                text: dlg.isImportMode ? qsTr("Switch to Export") : qsTr("Switch to Import")
                implicitHeight: 26
                enabled: !Backend.transferBusy
                onClicked: {
                    dlg.isImportMode = !dlg.isImportMode
                    dlg.statusMsg = ""
                }
            }
        }

        ScrollView {
            id: formScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 80
            contentWidth: availableWidth
            contentHeight: formContent.height
            rightPadding: formContent.implicitHeight > availableHeight ? 8 : 0
            clip: true

            ColumnLayout {
                id: formContent
                width: formScroll.availableWidth
                height: Math.max(implicitHeight, formScroll.availableHeight)
                spacing: 10

                GridLayout {
                    Layout.fillWidth: true
                    columns: Window.width < 520 ? 1 : 2
                    columnSpacing: 12
                    rowSpacing: 10
                    enabled: !Backend.transferBusy

            Label {
                text: qsTr("Format:")
                font.pixelSize: 12
                color: Theme.textColor
            }
            RagoComboBox {
                id: formatCombo
                Layout.fillWidth: true
                model: [
                    qsTr("JSON Array (.json)"),
                    qsTr("NDJSON / JSON Lines (.ndjson)"),
                    qsTr("CSV (.csv)")
                ]
            }

            Label {
                text: dlg.isImportMode ? qsTr("Source File:") : qsTr("Target File:")
                font.pixelSize: 12
                color: Theme.textColor
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                RagoTextField {
                    id: filePathField
                    ltr: true
                    placeholderText: dlg.isImportMode
                                     ? qsTr("Select .json, .ndjson, or .csv file to import")
                                     : qsTr("Choose output file path")
                    Layout.fillWidth: true
                }
                RagoPillButton {
                    text: qsTr("Browse…")
                    onClicked: {
                        if (dlg.isImportMode)
                            importFileDialog.open()
                        else
                            exportFileDialog.open()
                    }
                }
            }

            // Export-only options
            Label {
                visible: !dlg.isImportMode
                text: qsTr("Filter JSON:")
                font.pixelSize: 12
                color: Theme.textColor
            }
            RagoTextField {
                id: filterField
                ltr: true
                visible: !dlg.isImportMode
                placeholderText: "{}"
                Layout.fillWidth: true
            }

            Label {
                visible: !dlg.isImportMode
                text: qsTr("Limit:")
                font.pixelSize: 12
                color: Theme.textColor
            }
            RagoTextField {
                id: limitField
                ltr: true
                visible: !dlg.isImportMode
                placeholderText: qsTr("Optional max documents (0 or empty = all)")
                Layout.fillWidth: true
            }
        }

        // Import-only options
        ColumnLayout {
            visible: dlg.isImportMode
            enabled: !Backend.transferBusy
            spacing: 6
            Layout.fillWidth: true

            RagoCheckBox {
                id: dropBeforeCheck
                text: qsTr("Drop target collection before importing")
            }

            Rectangle {
                visible: dropBeforeCheck.checked
                Layout.fillWidth: true
                implicitHeight: inlineDropWarn.implicitHeight + 14
                radius: 8
                color: Theme.dark ? Qt.rgba(0.94, 0.27, 0.27, 0.14) : Qt.rgba(0.94, 0.27, 0.27, 0.09)
                border.width: 1
                border.color: Qt.rgba(0.94, 0.27, 0.27, 0.50)

                Label {
                    id: inlineDropWarn
                    anchors.fill: parent
                    anchors.margins: 7
                    text: qsTr("⚠ Warning: Dropping '%1.%2' permanently deletes all existing documents and custom indexes before import.")
                          .arg(Language.isolate(dlg.dbName)).arg(Language.isolate(dlg.collName))
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: "#ff6b6b"
                }
            }

            RagoCheckBox {
                id: stopOnErrorCheck
                text: qsTr("Ordered insert — stop on first duplicate / validation error")
            }
        }
        }
        }

        // Live progress & status card
        Rectangle {
            visible: Backend.transferBusy || dlg.statusMsg.length > 0
            Layout.fillWidth: true
            implicitHeight: statusRow.implicitHeight + 18
            radius: 9
            color: Theme.surfaceColor
            border.width: 1
            border.color: dlg.statusIsError ? "#ff6b6b" : Theme.glassBorder

            RowLayout {
                id: statusRow
                anchors.fill: parent
                anchors.margins: 9
                spacing: 10

                BusyIndicator {
                    visible: Backend.transferBusy
                    running: Backend.transferBusy
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20
                }

                Label {
                    text: Backend.transferBusy
                          ? Backend.transferStatus
                          : ((dlg.statusIsError ? "✕ " : "✓ ") + dlg.statusMsg)
                    font.pixelSize: 12
                    color: dlg.statusIsError && !Backend.transferBusy
                           ? "#ff6b6b" : Theme.accentColor
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                RagoPillButton {
                    visible: Backend.transferBusy
                    text: qsTr("Stop")
                    danger: true
                    implicitHeight: 26
                    onClicked: Backend.cancelTransfer()
                }
            }
        }
    }
}
