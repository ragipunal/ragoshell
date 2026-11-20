pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

ColumnLayout {
    id: filterTab
    required property var dlg

    property alias partialText: partialEditor.text
    property alias collationText: collationEditor.text
    property alias wildcardText: wildcardProjField.text

    function resetFields(partialStr, collationStr, wildcardStr) {
        partialEditor.setText(partialStr || "")
        collationPresetCombo.currentIndex = 0
        collationEditor.setText(collationStr || "")
        wildcardProjField.text = wildcardStr || ""
    }
    spacing: 8

    RowLayout {
        Layout.fillWidth: true
        Label {
            text: qsTr("Partial Filter Expression (JSON)")
            font.pixelSize: 12
            font.bold: true
            color: Theme.textColor
        }
        RagoBadge {
            text: "MongoDB 3.2+"
            badgeColor: filterTab.dlg.supportsPartialFilter ? Theme.accentColor : Theme.disabledTextColor
        }
        Item { Layout.fillWidth: true }
        RagoPillButton {
            text: qsTr("Format")
            implicitHeight: 22
            enabled: filterTab.dlg.supportsPartialFilter
            onClicked: {
                var f = Backend.formatJson(partialEditor.text)
                if (f.length > 0) partialEditor.setText(f)
            }
        }
    }

    RagoCodeEditor {
        id: partialEditor
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 84
        readOnly: !filterTab.dlg.supportsPartialFilter
        folding: false
        showLineNumbers: true
    }

    Label {
        text: qsTr("Optional filter (e.g. { \"status\": \"active\", \"score\": { \"$gt\": 0 } }) to index only matching documents.")
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        color: Theme.disabledTextColor
        Layout.fillWidth: true
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        Label {
            text: qsTr("Collation (JSON)")
            font.pixelSize: 12
            font.bold: true
            color: Theme.textColor
        }
        RagoBadge {
            text: "MongoDB 3.4+"
            badgeColor: filterTab.dlg.supportsCollation ? Theme.accentColor : Theme.disabledTextColor
        }
        Item { Layout.fillWidth: true }
        RagoComboBox {
            id: collationPresetCombo
            enabled: filterTab.dlg.supportsCollation
            implicitWidth: 210
            implicitHeight: 26
            model: [
                qsTr("Preset: Custom / None"),
                qsTr("Turkish (tr, case-insensitive)"),
                qsTr("English (en, case-insensitive)"),
                qsTr("German (de, case-insensitive)"),
                qsTr("Spanish (es, case-insensitive)")
            ]
            onActivated: function(idx) {
                var locales = ["", "tr", "en", "de", "es"]
                if (idx === 0)
                    collationEditor.setText("")
                else if (idx > 0 && idx < locales.length)
                    collationEditor.setText("{\n  \"locale\": \"" + locales[idx] + "\",\n  \"strength\": 2\n}")
            }
        }
    }

    RagoCodeEditor {
        id: collationEditor
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 78
        readOnly: !filterTab.dlg.supportsCollation
        folding: false
        showLineNumbers: true
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Label {
            text: qsTr("Wildcard Projection")
            font.pixelSize: 12
            color: Theme.disabledTextColor
        }
        RagoBadge {
            text: "4.2+"
            badgeColor: filterTab.dlg.supportsWildcard ? "#3b9eff" : Theme.disabledTextColor
        }
        RagoTextField {
            id: wildcardProjField
            ltr: true
            enabled: filterTab.dlg.supportsWildcard
            Layout.fillWidth: true
            font.family: AppSettings.codeFontFamily
            font.pixelSize: AppSettings.codeFontSize
            placeholderText: "{ \"attr.tags\": 1 }  (only when key is \"$**\")"
        }
    }
}
