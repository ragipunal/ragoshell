pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// MongoDB Version-Aware Create Collection / View / Time Series / Clustered modal:
//  - Standard & Capped Collections (all versions)
//  - Read-Only Views (`viewOn` + `pipeline`)            — MongoDB 3.4+
//  - Collation locale presets & custom JSON              — MongoDB 3.4+
//  - Document Validator (`$jsonSchema` starter template) — MongoDB 3.2+ / 3.6+
//  - Time Series Collections (`timeseries` + TTL)        — MongoDB 5.0+
//  - Clustered Collections (`clusteredIndex` on `_id`)   — MongoDB 5.3+
//  - Change Stream Pre- and Post-Images                  — MongoDB 6.0+
RagoDialog {
    id: dlg

    property string serverUuid: ""
    property string serverLabel: ""
    property string serverColorTag: ""
    property double serverVer: 0.0
    property string serverVerText: ""
    property string dbName: ""
    property string collectionMode: "standard" // "standard" | "capped" | "view" | "timeseries" | "clustered"
    property string errorMessage: ""

    readonly property bool supportsViews: serverVer <= 0.0 || serverVer >= 3.4
    readonly property bool supportsCollation: serverVer <= 0.0 || serverVer >= 3.4
    readonly property bool supportsJsonSchema: serverVer <= 0.0 || serverVer >= 3.6
    readonly property bool supportsTimeSeries: serverVer <= 0.0 || serverVer >= 5.0
    readonly property bool supportsClustered: serverVer <= 0.0 || serverVer >= 5.3
    readonly property bool supportsPrePostImages: serverVer <= 0.0 || serverVer >= 6.0

    title: collectionMode === "view"
           ? qsTr("Create Read-Only View")
           : (collectionMode === "timeseries"
              ? qsTr("Create Time Series Collection")
              : (collectionMode === "clustered"
                 ? qsTr("Create Clustered Collection")
                 : qsTr("Create Collection")))
    acceptText: collectionMode === "view" ? qsTr("Create View") : qsTr("Create")
    rejectText: qsTr("Cancel")
    showReject: true
    width: Math.min(visibleWidth - 24, 660)
    height: Math.min(visibleHeight - 24, 590)

    function openForDatabase(uuid, db, initialMode) {
        serverUuid = uuid || ""
        dbName = db || ""
        serverLabel = Backend.serverAddress(serverUuid)
        serverVer = Backend.serverVersion(serverUuid)
        serverVerText = Backend.serverVersionString(serverUuid)
        var _rev = ConnectionModel.revision
        serverColorTag = ConnectionModel.colorTagForUuid(serverUuid)

        var reqMode = initialMode || "standard"
        if (reqMode === "view" && !supportsViews) reqMode = "standard"
        if (reqMode === "timeseries" && !supportsTimeSeries) reqMode = "standard"
        if (reqMode === "clustered" && !supportsClustered) reqMode = "standard"
        collectionMode = reqMode

        errorMessage = ""
        collTabBar.currentIndex = 0
        nameField.text = ""
        sizeField.text = "1048576"
        maxDocsField.text = ""
        viewOnField.text = ""
        viewPipelineEditor.setText("[\n  { \"$match\": {} }\n]")
        tsTimeField.text = "timestamp"
        tsMetaField.text = "metadata"
        tsGranularityCombo.currentIndex = 0
        expireSecondsField.text = ""
        prePostImagesCheck.checked = false
        valLevelCombo.currentIndex = 0
        valActionCombo.currentIndex = 0
        validatorEditor.setText("")
        collationPresetCombo.currentIndex = 0
        collationEditor.setText("")
        storageEditor.setText("")
        extraEditor.setText("")
        open()
        Qt.callLater(function() { nameField.forceActiveFocus() })
    }

    function applyCollationPreset(idx) {
        var presets = [
            "",
            "{\n  \"locale\": \"en\",\n  \"strength\": 2\n}",
            "{\n  \"locale\": \"tr\",\n  \"strength\": 2\n}",
            "{\n  \"locale\": \"de\",\n  \"strength\": 2\n}",
            "{\n  \"locale\": \"fr\",\n  \"strength\": 2\n}",
            "{\n  \"locale\": \"es\",\n  \"strength\": 2\n}"
        ]
        if (idx >= 0 && idx < presets.length)
            collationEditor.setText(presets[idx])
    }

    function insertJsonSchemaTemplate() {
        validatorEditor.setText(
            "{\n" +
            "  \"$jsonSchema\": {\n" +
            "    \"bsonType\": \"object\",\n" +
            "    \"required\": [ \"name\", \"createdAt\" ],\n" +
            "    \"properties\": {\n" +
            "      \"name\": {\n" +
            "        \"bsonType\": \"string\",\n" +
            "        \"description\": \"Required string field\"\n" +
            "      },\n" +
            "      \"createdAt\": {\n" +
            "        \"bsonType\": \"date\",\n" +
            "        \"description\": \"Required UTC timestamp\"\n" +
            "      }\n" +
            "    }\n" +
            "  }\n" +
            "}")
    }

    beforeAccept: function() {
        var nameTrim = nameField.text.trim()
        if (nameTrim.length === 0) {
            collTabBar.currentIndex = 0
            errorMessage = qsTr("Name is required.")
            return false
        }

        var isCapped = (collectionMode === "capped")
        var sizeBytes = 0
        var maxDocs = 0
        if (isCapped) {
            sizeBytes = parseInt(sizeField.text.trim(), 10) || 0
            if (sizeBytes <= 0) {
                collTabBar.currentIndex = 0
                errorMessage = qsTr("Capped collections require a positive size in bytes.")
                return false
            }
            if (maxDocsField.text.trim().length > 0)
                maxDocs = parseInt(maxDocsField.text.trim(), 10) || 0
        }

        // Start from optional user-supplied extra JSON object
        var extraObj = ({})
        var eText = extraEditor.text.trim()
        if (eText.length > 0 && eText !== "{}") {
            let eErr = Backend.validateJson(eText, true)
            if (eErr.length > 0) {
                collTabBar.currentIndex = 2
                errorMessage = qsTr("Extra Options JSON error: %1").arg(Language.isolate(eErr))
                return false
            }
            try {
                extraObj = JSON.parse(Backend.formatJson(eText) || eText)
            } catch (ex) {
                collTabBar.currentIndex = 2
                errorMessage = qsTr("Extra Options must be strict JSON when combined with mode options.")
                return false
            }
        }

        if (collectionMode === "view") {
            let srcColl = viewOnField.text.trim()
            if (srcColl.length === 0) {
                collTabBar.currentIndex = 0
                errorMessage = qsTr("Source collection (viewOn) is required for a View.")
                return false
            }
            let pipeRaw = viewPipelineEditor.text.trim()
            if (pipeRaw.length === 0)
                pipeRaw = "[]"
            // Validate pipeline by wrapping in a document { pipeline: [...] }
            let pipeWrap = "{ \"viewOn\": " + JSON.stringify(srcColl) + ", \"pipeline\": " + pipeRaw + " }"
            let pErr = Backend.validateJson(pipeWrap, true)
            if (pErr.length > 0) {
                collTabBar.currentIndex = 0
                errorMessage = qsTr("View pipeline JSON error: %1").arg(Language.isolate(pErr))
                return false
            }
            try {
                let parsedPipe = JSON.parse(Backend.formatJson(pipeWrap) || pipeWrap)
                extraObj.viewOn = parsedPipe.viewOn
                extraObj.pipeline = parsedPipe.pipeline
            } catch (ex2) {
                extraObj.viewOn = srcColl
                extraObj.pipeline = []
            }
        } else if (collectionMode === "timeseries") {
            let tf = tsTimeField.text.trim()
            if (tf.length === 0) {
                collTabBar.currentIndex = 0
                errorMessage = qsTr("Time Series collections require a 'timeField' name (e.g. timestamp).")
                return false
            }
            let tsSpec = { timeField: tf, granularity: tsGranularityCombo.currentText }
            let mf = tsMetaField.text.trim()
            if (mf.length > 0)
                tsSpec.metaField = mf
            extraObj.timeseries = tsSpec
        } else if (collectionMode === "clustered") {
            extraObj.clusteredIndex = {
                key: { _id: 1 },
                unique: true
            }
        }

        if ((collectionMode === "timeseries" || collectionMode === "clustered") &&
            expireSecondsField.text.trim().length > 0) {
            let expSec = parseInt(expireSecondsField.text.trim(), 10)
            if (isNaN(expSec) || expSec < 0) {
                collTabBar.currentIndex = 0
                errorMessage = qsTr("Expire After Seconds must be a non-negative integer.")
                return false
            }
            extraObj.expireAfterSeconds = expSec
        }

        if (supportsPrePostImages && collectionMode !== "view" && prePostImagesCheck.checked) {
            extraObj.changeStreamPreAndPostImages = { enabled: true }
        }

        var collText = collationEditor.text.trim()
        if (supportsCollation && collText.length > 0 && collText !== "{}") {
            let cErr = Backend.validateJson(collText, true)
            if (cErr.length > 0) {
                collTabBar.currentIndex = 2
                errorMessage = qsTr("Collation JSON error: %1").arg(Language.isolate(cErr))
                return false
            }
            try {
                extraObj.collation = JSON.parse(Backend.formatJson(collText) || collText)
            } catch (ex3) {
                collTabBar.currentIndex = 2
                errorMessage = qsTr("Invalid Collation JSON object.")
                return false
            }
        }

        var vText = (collectionMode === "view") ? "" : validatorEditor.text.trim()
        if (vText.length > 0 && vText !== "{}") {
            let vErr = Backend.validateJson(vText, true)
            if (vErr.length > 0) {
                collTabBar.currentIndex = 1
                errorMessage = qsTr("Validator JSON error: %1").arg(Language.isolate(vErr))
                return false
            }
        }

        var sText = (collectionMode === "view") ? "" : storageEditor.text.trim()
        if (sText.length > 0 && sText !== "{}") {
            let sErr = Backend.validateJson(sText, true)
            if (sErr.length > 0) {
                collTabBar.currentIndex = 2
                errorMessage = qsTr("Storage Engine JSON error: %1").arg(Language.isolate(sErr))
                return false
            }
        }

        var finalExtraJson = Object.keys(extraObj).length > 0
                             ? JSON.stringify(extraObj)
                             : ""

        errorMessage = ""
        return Backend.createCollectionAdvanced(
            serverUuid, dbName, nameTrim,
            isCapped, sizeBytes, maxDocs,
            vText, valLevelCombo.currentText, valActionCombo.currentText,
            sText, finalExtraJson)
    }

    contentItem: ColumnLayout {
        spacing: 10

        // Context & Connected MongoDB Version Header Strip
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
                text: "DB: " + dlg.dbName
                badgeColor: "#3b9eff"
            }
            RagoBadge {
                visible: dlg.serverVerText.length > 0
                text: "MongoDB v" + dlg.serverVerText
                badgeColor: Theme.accentColor
            }
            Item { Layout.fillWidth: true }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            radius: 9
            color: Theme.surfaceColor
            border.width: 1
            border.color: Theme.glassBorder

            TabBar {
                id: collTabBar
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4
                background: null

                Repeater {
                    model: [
                        qsTr("General & Type"),
                        qsTr("Validator (3.2+)"),
                        qsTr("Collation & Advanced")
                    ]
                    TabButton {
                        id: segBtn
                        required property int index
                        required property string modelData
                        text: modelData
                        implicitHeight: 30
                        width: (collTabBar.width - collTabBar.spacing * (collTabBar.count - 1)) / collTabBar.count
                        enabled: !(index === 1 && dlg.collectionMode === "view")
                        opacity: enabled ? 1.0 : 0.45
                        contentItem: Label {
                            text: segBtn.text
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            font.bold: collTabBar.currentIndex === segBtn.index
                            color: collTabBar.currentIndex === segBtn.index
                                   ? Theme.textColor : Theme.disabledTextColor
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: 6
                            color: collTabBar.currentIndex === segBtn.index
                                   ? (Theme.dark ? Qt.rgba(1, 1, 1, 0.12) : "#ffffff")
                                   : (segBtn.hovered ? Theme.cardBackgroundHover : "transparent")
                        }
                    }
                }
            }
        }

        ScrollView {
            id: pageScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 60
            contentWidth: availableWidth
            contentHeight: stack.height
            rightPadding: stack.implicitHeight > availableHeight ? 8 : 0
            clip: true

            StackLayout {
                id: stack
                width: pageScroll.availableWidth
                height: Math.max(implicitHeight, pageScroll.availableHeight)
                Layout.minimumWidth: 0
                currentIndex: collTabBar.currentIndex

            // ---- Tab 0: Name + Version-Aware Collection Type ---------------
            ColumnLayout {
                spacing: 10

                ColumnLayout {
                    spacing: 4
                    Layout.fillWidth: true
                    Label {
                        text: dlg.collectionMode === "view"
                              ? qsTr("View Name")
                              : qsTr("Collection Name")
                        font.pixelSize: 12
                        color: Theme.disabledTextColor
                    }
                    RagoTextField {
                        id: nameField
                        placeholderText: dlg.collectionMode === "view"
                                         ? qsTr("e.g. active_orders_view")
                                         : (dlg.collectionMode === "timeseries"
                                            ? qsTr("e.g. sensor_metrics")
                                            : qsTr("e.g. audit_logs"))
                        Layout.fillWidth: true
                    }
                }

                // Version-Aware Collection Type Pills
                ColumnLayout {
                    spacing: 4
                    Layout.fillWidth: true

                    Label {
                        text: qsTr("Collection Type (adapted to connected MongoDB version)")
                        font.pixelSize: 12
                        color: Theme.disabledTextColor
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: 6

                        Repeater {
                            model: [
                                { key: "standard",   label: qsTr("Standard"),    minVer: 0.0, badge: "" },
                                //: Kind of collection: a fixed-size MongoDB capped collection.
                                { key: "capped",     label: qsTr("Capped"),      minVer: 0.0, badge: "" },
                                //: Kind of collection: a read-only MongoDB view defined by an aggregation pipeline.
                                { key: "view",       label: qsTr("View"),        minVer: 3.4, badge: "3.4+" },
                                { key: "timeseries", label: qsTr("Time Series"), minVer: 5.0, badge: "5.0+" },
                                { key: "clustered",  label: qsTr("Clustered"),   minVer: 5.3, badge: "5.3+" }
                            ]
                            delegate: RagoPillButton {
                                required property var modelData
                                readonly property bool supported: dlg.serverVer <= 0.0 || dlg.serverVer >= modelData.minVer
                                text: modelData.badge.length > 0
                                      ? (modelData.label + " (" + modelData.badge + ")")
                                      : modelData.label
                                primary: dlg.collectionMode === modelData.key
                                enabled: supported
                                implicitHeight: 28
                                onClicked: dlg.collectionMode = modelData.key
                                ToolTip.visible: hovered && !supported
                                ToolTip.text: qsTr("Requires MongoDB %1 or newer (connected: v%2)")
                                              .arg(modelData.badge)
                                              .arg(dlg.serverVerText)
                            }
                        }
                    }
                }

                // Mode-specific configuration area
                Rectangle {
                    visible: dlg.collectionMode !== "standard"
                    Layout.fillWidth: true
                    Layout.fillHeight: dlg.collectionMode === "view"
                    implicitHeight: dlg.collectionMode === "view" ? 210 : (modeContentCol.implicitHeight + 22)
                    radius: 10
                    color: Theme.glassBackground
                    border.width: 1
                    border.color: Theme.glassBorder

                    ColumnLayout {
                        id: modeContentCol
                        anchors.fill: parent
                        anchors.margins: 11
                        spacing: 8

                        // Capped
                        GridLayout {
                            visible: dlg.collectionMode === "capped"
                            Layout.fillWidth: true
                            columns: Window.width < 520 ? 1 : 2
                            columnSpacing: 12
                            rowSpacing: 8

                            Label {
                                text: qsTr("Max Size (bytes):")
                                font.pixelSize: 12
                                color: Theme.textColor
                            }
                            RagoTextField {
                                id: sizeField
                                ltr: true
                                placeholderText: "1048576"
                                Layout.fillWidth: true
                            }

                            Label {
                                text: qsTr("Max Documents:")
                                font.pixelSize: 12
                                color: Theme.textColor
                            }
                            RagoTextField {
                                id: maxDocsField
                                ltr: true
                                placeholderText: qsTr("Optional document count limit")
                                Layout.fillWidth: true
                            }
                        }

                        // Read-Only View (3.4+)
                        ColumnLayout {
                            visible: dlg.collectionMode === "view"
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Label {
                                    text: qsTr("Source Collection (viewOn):")
                                    font.pixelSize: 12
                                    color: Theme.textColor
                                }
                                RagoTextField {
                                    id: viewOnField
                                    ltr: true
                                    placeholderText: qsTr("e.g. orders")
                                    Layout.fillWidth: true
                                }
                            }

                            Label {
                                text: qsTr("Aggregation Pipeline Array ([ { \"$match\": ... }, ... ]):")
                                font.pixelSize: 11
                                color: Theme.disabledTextColor
                            }

                            RagoCodeEditor {
                                id: viewPipelineEditor
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                folding: true
                                showLineNumbers: true
                            }
                        }

                        // Time Series (5.0+)
                        GridLayout {
                            visible: dlg.collectionMode === "timeseries"
                            Layout.fillWidth: true
                            columns: Window.width < 520 ? 1 : 2
                            columnSpacing: 12
                            rowSpacing: 8

                            Label {
                                text: qsTr("Time Field (required):")
                                font.pixelSize: 12
                                color: Theme.textColor
                            }
                            RagoTextField {
                                id: tsTimeField
                                ltr: true
                                placeholderText: qsTr("e.g. timestamp")
                                Layout.fillWidth: true
                            }

                            Label {
                                text: qsTr("Meta Field (optional):")
                                font.pixelSize: 12
                                color: Theme.textColor
                            }
                            RagoTextField {
                                id: tsMetaField
                                ltr: true
                                placeholderText: qsTr("e.g. sensorId or metadata")
                                Layout.fillWidth: true
                            }

                            Label {
                                text: qsTr("Bucket Granularity:")
                                font.pixelSize: 12
                                color: Theme.textColor
                            }
                            RagoComboBox {
                                id: tsGranularityCombo
                                model: ["seconds", "minutes", "hours"]
                                Layout.fillWidth: true
                            }
                        }

                        // Clustered (5.3+)
                        Label {
                            visible: dlg.collectionMode === "clustered"
                            text: qsTr("Stores documents ordered directly by '_id' ({ clusteredIndex: { key: { _id: 1 }, unique: true } }) without a separate _id index — reducing storage size and speeding up _id range lookups (MongoDB 5.3+).")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.textColor
                            Layout.fillWidth: true
                        }

                        // Shared TTL for Time Series (5.0+) & Clustered (5.3+)
                        RowLayout {
                            visible: dlg.collectionMode === "timeseries" || dlg.collectionMode === "clustered"
                            Layout.fillWidth: true
                            spacing: 12

                            Label {
                                text: qsTr("Expire After (sec):")
                                font.pixelSize: 12
                                color: Theme.textColor
                            }
                            RagoTextField {
                                id: expireSecondsField
                                ltr: true
                                placeholderText: qsTr("Optional automatic TTL expiration in seconds (e.g. 86400)")
                                Layout.fillWidth: true
                            }
                        }
                    }
                }

                // MongoDB 6.0+ Change Stream Pre- and Post-Images
                RowLayout {
                    visible: dlg.collectionMode !== "view"
                    Layout.fillWidth: true
                    spacing: 8
                    opacity: dlg.supportsPrePostImages ? 1.0 : 0.5

                    RagoCheckBox {
                        id: prePostImagesCheck
                        enabled: dlg.supportsPrePostImages
                        text: qsTr("Enable Change Stream Pre- and Post-Images")
                    }
                    RagoBadge {
                        text: "6.0+"
                        badgeColor: dlg.supportsPrePostImages
                                    ? Theme.accentColor
                                    : Theme.disabledTextColor
                    }
                    Item { Layout.fillWidth: true }
                }

                Item {
                    visible: dlg.collectionMode !== "view"
                    Layout.fillHeight: true
                }
            }

            // ---- Tab 1: Document Validator ($jsonSchema 3.6+) --------------
            ColumnLayout {
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Label {
                        text: qsTr("Level:")
                        font.pixelSize: 12
                        color: Theme.disabledTextColor
                    }
                    RagoComboBox {
                        id: valLevelCombo
                        model: ["strict", "moderate", "off"]
                        Layout.fillWidth: true
                    }

                    Label {
                        text: qsTr("Action:")
                        font.pixelSize: 12
                        color: Theme.disabledTextColor
                    }
                    RagoComboBox {
                        id: valActionCombo
                        model: ["error", "warn"]
                        Layout.fillWidth: true
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Label {
                        text: qsTr("Validator Rule JSON")
                        font.pixelSize: 12
                        color: Theme.disabledTextColor
                        Layout.fillWidth: true
                    }

                    RagoPillButton {
                        visible: dlg.supportsJsonSchema
                        text: qsTr("Insert $jsonSchema Template (3.6+)")
                        implicitHeight: 26
                        onClicked: dlg.insertJsonSchemaTemplate()
                    }

                    RagoPillButton {
                        text: qsTr("Format")
                        implicitHeight: 26
                        onClicked: {
                            var f = Backend.formatJson(validatorEditor.text)
                            if (f.length > 0) validatorEditor.setText(f)
                        }
                    }
                }

                RagoCodeEditor {
                    id: validatorEditor
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    folding: true
                    showLineNumbers: true
                }
            }

            // ---- Tab 2: Collation (3.4+), Storage Engine & Extra Options ---
            ColumnLayout {
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    opacity: dlg.supportsCollation ? 1.0 : 0.5

                    Label {
                        text: qsTr("Collation (3.4+):")
                        font.pixelSize: 12
                        color: Theme.textColor
                    }
                    RagoComboBox {
                        id: collationPresetCombo
                        enabled: dlg.supportsCollation
                        Layout.fillWidth: true
                        model: [
                            qsTr("Server Default (Binary)"),
                            qsTr("English — Case-Insensitive (en, strength: 2)"),
                            qsTr("Turkish — Case-Insensitive (tr, strength: 2)"),
                            qsTr("German — Case-Insensitive (de, strength: 2)"),
                            qsTr("French — Case-Insensitive (fr, strength: 2)"),
                            qsTr("Spanish — Case-Insensitive (es, strength: 2)")
                        ]
                        onActivated: function(idx) { dlg.applyCollationPreset(idx) }
                    }
                }

                RagoCodeEditor {
                    id: collationEditor
                    visible: dlg.supportsCollation
                    Layout.fillWidth: true
                    Layout.preferredHeight: 86
                    folding: true
                    showLineNumbers: true
                }

                Label {
                    visible: dlg.collectionMode !== "view"
                    text: qsTr("Storage Engine Options JSON (optional, e.g. { \"wiredTiger\": {} })")
                    font.pixelSize: 12
                    color: Theme.disabledTextColor
                }
                RagoCodeEditor {
                    id: storageEditor
                    visible: dlg.collectionMode !== "view"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    folding: true
                    showLineNumbers: true
                }

                Label {
                    text: qsTr("Additional 'create' Command Options JSON (optional)")
                    font.pixelSize: 12
                    color: Theme.disabledTextColor
                }
                RagoCodeEditor {
                    id: extraEditor
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    folding: true
                    showLineNumbers: true
                }
            }
        }
        }

        Label {
            visible: dlg.errorMessage.length > 0
            text: "✕ " + dlg.errorMessage
            font.pixelSize: 12
            color: "#ff6b6b"
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }
}
