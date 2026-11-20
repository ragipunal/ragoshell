pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

ColumnLayout {
    id: root
    spacing: 14

    property alias modeIndex: schemaModeCombo.currentIndex
    property alias sampleSize: schemaSampleSpin.value
    property alias maxCollections: schemaMaxCollSpin.value
    property alias unmeteredOnly: schemaUnmeteredCheck.checked

    property int _modeStash: 1

    Connections {
        target: Language
        function onLanguageAboutToChange() {
            root._modeStash = schemaModeCombo.currentIndex
        }
        function onLanguageChanged() {
            schemaModeCombo.currentIndex = root._modeStash
        }
    }

    function commitSpinBoxes() {
        schemaSampleSpin.commitText()
        schemaMaxCollSpin.commitText()
    }

    Label {
        text: qsTr("SCHEMA ANALYSIS")
        font.bold: true
        font.pixelSize: 11
        font.letterSpacing: Language.rtl ? 0 : 1.1
        color: Theme.accentColor
    }

    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: 12
        color: Theme.disabledTextColor
        text: qsTr("Analyzes the collections of the databases you work with in the background, one at a time with a small sample, so the AI assistant knows their fields. Frequent values are kept only when the AI option above allows sending them. A connection can override this setting.")
    }

    GridLayout {
        Layout.fillWidth: true
        columns: Window.width < 520 ? 1 : 2
        columnSpacing: 16
        rowSpacing: 10

        Label {
            text: qsTr("Background analysis")
            font.pixelSize: 13
            color: Theme.textColor
            Layout.fillWidth: true
        }
        RagoComboBox {
            id: schemaModeCombo
            model: [qsTr("Off"), qsTr("Databases in use"), qsTr("All databases")]
            Layout.preferredWidth: 200
        }

        Label {
            text: qsTr("Documents per collection")
            font.pixelSize: 13
            color: Theme.textColor
            Layout.fillWidth: true
        }
        RagoSpinBox {
            id: schemaSampleSpin
            from: 50
            to: 10000
            enabled: schemaModeCombo.currentIndex !== 0
            Layout.preferredWidth: 200
        }

        Label {
            text: qsTr("Collections per database")
            font.pixelSize: 13
            color: Theme.textColor
            Layout.fillWidth: true
        }
        RagoSpinBox {
            id: schemaMaxCollSpin
            from: 1
            to: 1000
            enabled: schemaModeCombo.currentIndex !== 0
            Layout.preferredWidth: 200
        }

        RagoCheckBox {
            id: schemaUnmeteredCheck
            Layout.columnSpan: Math.min(2, parent.columns)
            Layout.fillWidth: true
            enabled: schemaModeCombo.currentIndex !== 0
            text: qsTr("Wait while the connection is metered (mobile data, phone hotspot)")
        }
    }
}
