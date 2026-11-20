pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

ColumnLayout {
    id: textTab

    property alias defaultLang: defaultLangField.text
    property alias langOverride: langOverrideField.text
    property alias weightsText: weightsEditor.text

    function resetFields(lang, overrideField, weightsStr) {
        defaultLangField.text = lang || ""
        langOverrideField.text = overrideField || ""
        weightsEditor.setText(weightsStr || "")
    }
    spacing: 8

    ColumnLayout {
        spacing: 3
        Layout.fillWidth: true
        Label {
            text: qsTr("Default language")
            font.pixelSize: 12
            font.bold: true
            color: Theme.textColor
        }
        RagoTextField {
            id: defaultLangField
            ltr: true
            placeholderText: qsTr("e.g. turkish, english, none")
            font.family: AppSettings.codeFontFamily
            font.pixelSize: AppSettings.codeFontSize
            Layout.fillWidth: true
        }
        Label {
            text: qsTr("Determines the list of stop words and the rules for the stemmer and tokenizer (default: english).")
            wrapMode: Text.WordWrap
            font.pixelSize: 11
            color: Theme.disabledTextColor
            Layout.fillWidth: true
        }
    }

    ColumnLayout {
        spacing: 3
        Layout.fillWidth: true
        Label {
            text: qsTr("Language override field")
            font.pixelSize: 12
            font.bold: true
            color: Theme.textColor
        }
        RagoTextField {
            id: langOverrideField
            ltr: true
            placeholderText: qsTr("e.g. language")
            font.family: AppSettings.codeFontFamily
            font.pixelSize: AppSettings.codeFontSize
            Layout.fillWidth: true
        }
        Label {
            text: qsTr("Name of the document field whose value overrides the default language per document.")
            wrapMode: Text.WordWrap
            font.pixelSize: 11
            color: Theme.disabledTextColor
            Layout.fillWidth: true
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Label {
            text: qsTr("Text weights (JSON)")
            font.pixelSize: 12
            font.bold: true
            color: Theme.textColor
            Layout.fillWidth: true
        }
        RagoPillButton {
            text: qsTr("Format")
            implicitHeight: 22
            onClicked: {
                var f = Backend.formatJson(weightsEditor.text)
                if (f.length > 0) weightsEditor.setText(f)
            }
        }
    }

    RagoCodeEditor {
        id: weightsEditor
        Layout.fillWidth: true
        Layout.fillHeight: true
        folding: false
        showLineNumbers: true
    }

    Label {
        text: qsTr("Optional JSON document assigning relative significance weights to text-indexed fields (e.g. { \"title\": 10, \"body\": 2 }).")
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        color: Theme.disabledTextColor
        Layout.fillWidth: true
    }
}
