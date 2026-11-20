pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

// Shows a shell find()/aggregate() statement as Python / Node.js / Java / C#
// driver code, ready to copy.
RagoDialog {
    id: dlg

    title: qsTr("Export Query to Language")
    acceptText: qsTr("Close")
    width: Math.min(visibleWidth - 24, 960)
    height: Math.min(visibleHeight - 24, 720)

    property string script: ""
    property string database: ""
    property string serverUuid: ""
    property int language: 0          // kept between openings
    property var exportResult: ({ ok: false, error: "", code: "" })

    function openFor(scriptText, db, uuid) {
        script = scriptText
        database = db
        serverUuid = uuid
        refresh()
        open()
    }
    function refresh() {
        exportResult = exporter.exportScript(script, database, serverUuid, language)
    }

    CodeExporter { id: exporter }

    contentItem: ColumnLayout {
        spacing: 10

        HScrollRow {
            Layout.fillWidth: true
            margin: 0
            spacing: 6

            Repeater {
                model: exporter.languages
                RagoPillButton {
                    required property string modelData
                    required property int index
                    text: modelData
                    checked: dlg.language === index
                    onClicked: {
                        dlg.language = index
                        dlg.refresh()
                    }
                }
            }
            Item { Layout.fillWidth: true }
            RagoPillButton {
                text: qsTr("Copy Code")
                primary: true
                enabled: dlg.exportResult.ok
                onClicked: Backend.copyToClipboard(dlg.exportResult.code)
            }
        }

        Label {
            visible: !dlg.exportResult.ok
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: dlg.exportResult.error + "\n\n" +
                  qsTr("Select a single find(...) chain or aggregate([...]) statement in the editor and try again.")
            color: Theme.dangerColor
        }

        RagoCodeEditor {
            visible: dlg.exportResult.ok
            Layout.fillWidth: true
            Layout.fillHeight: true
            readOnly: true
            text: dlg.exportResult.code
        }
    }
}
