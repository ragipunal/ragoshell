pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

// AI query assistant: a natural-language request becomes one read-only
// find()/aggregate() for the collection. Only the sampled schema shown under
// "Data sent" leaves the machine, and only when the user presses Generate.
RagoDialog {
    id: dlg

    title: qsTr("AI Query Assistant")
    acceptText: qsTr("Close")
    width: Math.min(visibleWidth - 24, 1000)
    height: Math.min(visibleHeight - 24, 760)

    readonly property bool stacked: width < 700

    /// Returns false when the server is not connected or no collection is given.
    function openFor(serverUuid, database, collection) {
        if (!assistant.configure(serverUuid, database, collection))
            return false
        open()
        questionField.forceActiveFocus()
        return true
    }

    /// Dev/CI helper (--ai): fill in the question and generate.
    function askDebug(question) {
        questionField.text = question
        assistant.ask(question)
    }

    function _openInShell(execute) {
        Backend.openShellWithScript(assistant.serverUuid, assistant.database, assistant.script, execute)
        dlg.close()
    }

    /// Handled by Main: Preferences / Export Code dialogs.
    signal preferencesRequested()
    signal exportRequested(string script, string database, string serverUuid)

    onOpened: assistant.refreshSettings()
    onClosed: assistant.cancel()

    AiAssistant { id: assistant }
    Connections {
        target: AppSettings
        function onAiSettingsChanged() { assistant.refreshSettings() }
    }

    contentItem: ColumnLayout {
        spacing: 10

        HScrollRow {
            Layout.fillWidth: true
            margin: 0
            spacing: 8

            RagoBadge { text: assistant.database + "." + assistant.collection }
            Label {
                //: AI assistant header: provider · model · host the request goes to.
                text: !assistant.ready ? qsTr("off")
                      : assistant.providerName + " · "
                        + (assistant.activeModel.length > 0 ? assistant.activeModel : assistant.model)
                        + (assistant.endpointHost.length > 0 ? " · " + qsTr("sends to %1").arg(assistant.endpointHost) : "")
                font.pixelSize: 12
                color: Theme.disabledTextColor
            }
            BusyIndicator {
                running: assistant.busy
                visible: running
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
            }
            Label {
                visible: assistant.busy
                text: assistant.sampling ? qsTr("Sampling the collection…")
                                         : qsTr("Asking %1…").arg(assistant.activeModel)
                font.pixelSize: 12
                color: Theme.disabledTextColor
            }
            Item { Layout.fillWidth: true }
            RagoPillButton {
                id: sentToggle
                text: qsTr("Data sent")
                checkable: true
                enabled: assistant.schemaText.length > 0
            }
        }

        // ---- Off / no key -------------------------------------------------------
        Rectangle {
            visible: !assistant.ready
            Layout.fillWidth: true
            implicitHeight: offRow.implicitHeight + 24
            radius: 8
            color: Theme.alternateColor
            border.color: Theme.glassBorder

            GridLayout {
                id: offRow
                anchors.fill: parent
                anchors.margins: 12
                columns: dlg.width < 520 ? 1 : 2
                columnSpacing: 12
                rowSpacing: 8
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textColor
                    text: qsTr("The AI assistant is off or not set up for %1. When it is on, Generate "
                               + "sends your request and this collection's field names and types (no "
                               + "document values unless you allow them) to the selected provider's API "
                               + "(Anthropic Claude, Google Gemini, an OpenAI-compatible server or Ollama). Turn it "
                               + "on and set it up in Preferences.").arg(assistant.providerName)
                }
                RagoPillButton {
                    text: qsTr("Preferences…")
                    primary: true
                    onClicked: dlg.preferencesRequested()
                }
            }
        }

        // ---- Request ------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            RagoTextField {
                id: questionField
                Layout.fillWidth: true
                placeholderText: qsTr("e.g. shipped orders over 100 in the last 30 days, newest first")
                onAccepted: if (generateButton.enabled) assistant.ask(text)
            }
            RagoPillButton {
                id: generateButton
                text: qsTr("Generate")
                primary: true
                enabled: assistant.ready && !assistant.busy && questionField.text.trim().length > 0
                onClicked: assistant.ask(questionField.text)
            }
            RagoPillButton {
                visible: assistant.busy
                text: qsTr("Cancel")
                onClicked: assistant.cancel()
            }
        }

        // Other collections described for $lookup (docs/SchemaIntelligence.md).
        RowLayout {
            visible: assistant.ready && assistant.databaseCollections > 1
                     && assistant.analyzedCollections < assistant.databaseCollections
            Layout.fillWidth: true
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.disabledTextColor
                text: qsTr("%1 of %2 collections of this database are analyzed; a query that joins "
                           + "collections can use only those.")
                          .arg(assistant.analyzedCollections).arg(assistant.databaseCollections)
            }
            RagoPillButton {
                text: qsTr("Analyze the Rest")
                onClicked: assistant.analyzeDatabase()
            }
        }

        // Models skipped or replaced because of quota / rate limits / retirement.
        Repeater {
            model: assistant.notices
            delegate: Label {
                required property string modelData
                Layout.fillWidth: true
                text: "⚠ " + modelData
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.warningColor
            }
        }

        RowLayout {
            id: errorRow
            visible: assistant.error.length > 0
            Layout.fillWidth: true
            spacing: 8

            // Seconds until the rate limit that failed the request clears.
            property int waitSec: 0
            function updateWait() {
                waitSec = assistant.retryAtMs > 0
                          ? Math.max(0, Math.ceil((assistant.retryAtMs - Date.now()) / 1000)) : 0
            }
            Timer {
                interval: 1000
                repeat: true
                running: errorRow.visible && assistant.retryAtMs > 0
                triggeredOnStart: true
                onTriggered: errorRow.updateWait()
            }
            Connections {
                target: assistant
                function onStateChanged() { errorRow.updateWait() }
            }

            Label {
                Layout.fillWidth: true
                // Model and server text: never rich text (an <img> would load).
                text: assistant.error
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: Theme.dangerColor
            }
            RagoPillButton {
                visible: assistant.retryable
                text: errorRow.waitSec > 0 ? qsTr("Retry in %1 s").arg(errorRow.waitSec) : qsTr("Retry")
                enabled: !assistant.busy && errorRow.waitSec === 0
                onClicked: assistant.retry(questionField.text)
            }
            // Sends the request to another company, so only on a click.
            RagoPillButton {
                visible: assistant.alternativeProvider.length > 0
                text: qsTr("Send to %1 instead").arg(assistant.alternativeProviderName)
                enabled: !assistant.busy
                onClicked: assistant.askWithProvider(questionField.text, assistant.alternativeProvider)
            }
            RagoPillButton {
                visible: assistant.settingsError
                text: qsTr("Preferences…")
                onClicked: dlg.preferencesRequested()
            }
        }

        Label {
            visible: assistant.explanation.length > 0 && assistant.error.length === 0
            Layout.fillWidth: true
            text: assistant.explanation
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: Theme.textColor
        }

        Label {
            visible: assistant.writesData
            Layout.fillWidth: true
            text: qsTr("This pipeline writes data ($out / $merge). Review it carefully; Run is disabled.")
            wrapMode: Text.WordWrap
            color: Theme.dangerColor
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: dlg.stacked ? Qt.Vertical : Qt.Horizontal

            RagoCodeEditor {
                SplitView.fillWidth: true
                SplitView.fillHeight: true
                SplitView.minimumHeight: 80
                readOnly: true
                text: assistant.script
            }

            // Exactly what Generate sends besides the request itself.
            RagoCodeEditor {
                visible: sentToggle.checked
                SplitView.preferredWidth: dlg.stacked ? -1 : 420
                SplitView.preferredHeight: dlg.stacked ? 150 : -1
                SplitView.minimumHeight: dlg.stacked ? 70 : 0
                readOnly: true
                showLineNumbers: false
                text: assistant.schemaText
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6

            Label {
                visible: dlg.width < 680
                Layout.fillWidth: true
                text: qsTr("Check generated queries before running them on production data.")
                font.pixelSize: 12
                color: Theme.disabledTextColor
                wrapMode: Text.WordWrap
            }

            HScrollRow {
                Layout.fillWidth: true
                margin: 0
                spacing: 8

                Label {
                    visible: dlg.width >= 680
                    Layout.fillWidth: true
                    text: qsTr("Check generated queries before running them on production data.")
                    font.pixelSize: 12
                    color: Theme.disabledTextColor
                    elide: Text.ElideRight
                }
                Item {
                    visible: dlg.width < 680
                    Layout.fillWidth: true
                }
                RagoPillButton {
                    text: qsTr("Copy")
                    enabled: assistant.script.length > 0
                    onClicked: Backend.copyToClipboard(assistant.script)
                }
                RagoPillButton {
                    text: qsTr("Export Code")
                    enabled: assistant.script.length > 0
                    onClicked: dlg.exportRequested(assistant.script, assistant.database, assistant.serverUuid)
                }
                RagoPillButton {
                    text: qsTr("Open in Shell")
                    enabled: assistant.script.length > 0
                    onClicked: dlg._openInShell(false)
                }
                RagoPillButton {
                    text: qsTr("Run in Shell")
                    primary: true
                    enabled: assistant.script.length > 0 && !assistant.writesData
                    onClicked: dlg._openInShell(true)
                }
            }
        }
    }
}
