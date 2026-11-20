pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Full-featured, MongoDB-version-aware Add / Edit Index dialog.
// Restores all classic AddEditIndexDialog context & helper descriptions and adds
// server-version-aware options: Partial Filter (3.2+), Collation (3.4+),
// Wildcard Projection & Hybrid Background notice (4.2+), and Hidden Indexes (4.4+).
RagoDialog {
    id: dlg

    property string serverUuid: ""
    property string serverLabel: ""
    property string serverVerText: ""
    property real serverVer: 0.0
    property string dbName: ""
    property string collName: ""
    property string oldIndexName: ""
    property var oldMeta: ({})
    property string errorMessage: ""

    readonly property bool supportsPartialFilter: serverVer <= 0.0 || serverVer >= 3.2
    readonly property bool supportsCollation: serverVer <= 0.0 || serverVer >= 3.4
    readonly property bool supportsWildcard: serverVer <= 0.0 || serverVer >= 4.2
    readonly property bool backgroundDeprecated: serverVer >= 4.2
    readonly property bool supportsHidden: serverVer <= 0.0 || serverVer >= 4.4

    title: oldIndexName.length > 0 ? qsTr("Edit Index") : qsTr("Add Index")
    acceptText: oldIndexName.length > 0 ? qsTr("Save Index") : qsTr("Create Index")
    rejectText: qsTr("Cancel")
    showReject: true
    width: Math.min(visibleWidth - 24, 640)
    height: Math.min(visibleHeight - 24, 600)

    function _refreshServerContext(uuid) {
        serverVer = Backend.serverVersion(uuid)
        serverVerText = Backend.serverVersionString(uuid)
        serverLabel = Backend.serverAddress(uuid)
    }

    function suggestIndexNameFromKeys() {
        var raw = keysEditor.text.trim()
        if (raw.length === 0)
            return
        var parts = []
        var re = /['"]?([A-Za-z0-9_$.*]+)['"]?\s*:\s*(['"]?[A-Za-z0-9_-]+['"]?)/g
        var m
        while ((m = re.exec(raw)) !== null) {
            var k = m[1].replace(/\$\*\*/g, "wildcard")
            var v = m[2].replace(/['"]/g, "")
            parts.push(k + "_" + v)
        }
        if (parts.length > 0)
            nameField.text = parts.join("_")
    }

    function openForAdd(uuid, db, coll) {
        serverUuid = uuid
        dbName = db
        collName = coll
        _refreshServerContext(uuid)
        oldIndexName = ""
        oldMeta = ({})
        errorMessage = ""
        idxTabBar.currentIndex = 0
        nameField.text = ""
        keysEditor.setText("{\n  \"field\": 1\n}")
        uniqueCheck.checked = false
        sparseCheck.checked = false
        ttlCheck.checked = false
        ttlField.text = "3600"
        hiddenCheck.checked = false
        bgCheck.checked = false
        filterCollationTab.resetFields("", "", "")
        textSearchTab.resetFields("", "", "")
        open()
        nameField.forceActiveFocus()
    }

    function openForEdit(uuid, db, coll, meta) {
        serverUuid = uuid
        dbName = db
        collName = coll
        _refreshServerContext(uuid)
        var m = meta || ({})
        oldIndexName = m.name || ""
        oldMeta = m
        errorMessage = ""
        idxTabBar.currentIndex = 0
        nameField.text = m.name || ""
        keysEditor.setText(Backend.formatJson(m.keys || "{}") || (m.keys || "{}"))
        uniqueCheck.checked = !!m.unique
        sparseCheck.checked = !!m.sparse
        var hasTtl = (m.ttl !== undefined && m.ttl >= 0)
        ttlCheck.checked = hasTtl
        ttlField.text = hasTtl ? String(m.ttl) : "3600"
        hiddenCheck.checked = !!m.hidden
        bgCheck.checked = (!backgroundDeprecated && !!m.background)
        filterCollationTab.resetFields(
            m.partialFilterExpression ? (Backend.formatJson(m.partialFilterExpression) || m.partialFilterExpression) : "",
            m.collation ? (Backend.formatJson(m.collation) || m.collation) : "",
            m.wildcardProjection ? String(m.wildcardProjection).trim() : "")
        textSearchTab.resetFields(
            m.defaultLanguage || "",
            m.languageOverride || "",
            m.textWeights ? (Backend.formatJson(m.textWeights) || m.textWeights) : "")
        open()
        nameField.forceActiveFocus()
    }

    beforeAccept: function() {
        var nameTrim = nameField.text.trim()
        if (nameTrim.length === 0) {
            idxTabBar.currentIndex = 0
            errorMessage = qsTr("Index name is required.")
            return false
        }
        var keysTrim = keysEditor.text.trim()
        if (keysTrim.length === 0) {
            idxTabBar.currentIndex = 0
            errorMessage = qsTr("Index keys JSON is required.")
            return false
        }
        var kErr = Backend.validateJson(keysTrim, true)
        if (kErr.length > 0) {
            idxTabBar.currentIndex = 0
            errorMessage = qsTr("Keys JSON error: %1").arg(Language.isolate(kErr))
            return false
        }

        if (ttlCheck.checked) {
            var parsedTtl = parseInt(ttlField.text.trim(), 10)
            if (isNaN(parsedTtl) || parsedTtl < 0) {
                idxTabBar.currentIndex = 1
                errorMessage = qsTr("Expire after (TTL) must be a non-negative integer in seconds.")
                return false
            }
        }

        var pTrim = filterCollationTab.partialText.trim()
        if (pTrim.length > 0 && pTrim !== "{}") {
            if (!supportsPartialFilter) {
                idxTabBar.currentIndex = 2
                errorMessage = qsTr("Partial Filter Expression requires MongoDB 3.2 or newer.")
                return false
            }
            var pErr = Backend.validateJson(pTrim, true)
            if (pErr.length > 0) {
                idxTabBar.currentIndex = 2
                errorMessage = qsTr("Partial Filter JSON error: %1").arg(Language.isolate(pErr))
                return false
            }
        }

        var cTrim = filterCollationTab.collationText.trim()
        if (cTrim.length > 0 && cTrim !== "{}") {
            if (!supportsCollation) {
                idxTabBar.currentIndex = 2
                errorMessage = qsTr("Collation requires MongoDB 3.4 or newer.")
                return false
            }
            var cErr = Backend.validateJson(cTrim, true)
            if (cErr.length > 0) {
                idxTabBar.currentIndex = 2
                errorMessage = qsTr("Collation JSON error: %1").arg(Language.isolate(cErr))
                return false
            }
        }

        var wpTrim = filterCollationTab.wildcardText.trim()
        if (wpTrim.length > 0 && wpTrim !== "{}") {
            if (!supportsWildcard) {
                idxTabBar.currentIndex = 2
                errorMessage = qsTr("Wildcard Projection requires MongoDB 4.2 or newer.")
                return false
            }
            var wpErr = Backend.validateJson(wpTrim, true)
            if (wpErr.length > 0) {
                idxTabBar.currentIndex = 2
                errorMessage = qsTr("Wildcard Projection JSON error: %1").arg(Language.isolate(wpErr))
                return false
            }
        }

        var wTrim = textSearchTab.weightsText.trim()
        if (wTrim.length > 0 && wTrim !== "{}") {
            var wErr = Backend.validateJson(wTrim, true)
            if (wErr.length > 0) {
                idxTabBar.currentIndex = 3
                errorMessage = qsTr("Text weights JSON error: %1").arg(Language.isolate(wErr))
                return false
            }
        }

        errorMessage = ""
        return true
    }

    onAccepted: {
        var ttlVal = -1
        if (ttlCheck.checked) {
            var parsed = parseInt(ttlField.text.trim(), 10)
            if (!isNaN(parsed) && parsed >= 0)
                ttlVal = parsed
        }
        var cleanOpt = function(s) {
            var t = String(s || "").trim()
            return t === "{}" ? "" : t
        }
        Backend.saveIndex(serverUuid, dbName, collName, oldIndexName, {
            "name": nameField.text.trim(),
            "keys": keysEditor.text.trim(),
            "unique": uniqueCheck.checked,
            "sparse": sparseCheck.checked,
            "background": (!backgroundDeprecated && bgCheck.checked),
            "hidden": (supportsHidden && hiddenCheck.checked),
            "ttl": ttlVal,
            "partialFilterExpression": supportsPartialFilter ? cleanOpt(filterCollationTab.partialText) : "",
            "collation": supportsCollation ? cleanOpt(filterCollationTab.collationText) : "",
            "wildcardProjection": supportsWildcard ? cleanOpt(filterCollationTab.wildcardText) : "",
            "defaultLanguage": textSearchTab.defaultLang.trim(),
            "languageOverride": textSearchTab.langOverride.trim(),
            "textWeights": cleanOpt(textSearchTab.weightsText),
            "oldKeys": oldMeta.keys || "",
            "oldUnique": !!oldMeta.unique,
            "oldBackground": !!oldMeta.background,
            "oldSparse": !!oldMeta.sparse,
            "oldHidden": !!oldMeta.hidden,
            "oldTtl": (oldMeta.ttl !== undefined ? oldMeta.ttl : -1),
            "oldPartialFilterExpression": oldMeta.partialFilterExpression || "",
            "oldCollation": oldMeta.collation || "",
            "oldWildcardProjection": oldMeta.wildcardProjection || "",
            "oldDefaultLanguage": oldMeta.defaultLanguage || "",
            "oldLanguageOverride": oldMeta.languageOverride || "",
            "oldTextWeights": oldMeta.textWeights || ""
        })
    }

    contentItem: ColumnLayout {
        spacing: 10

        // ---- Classic Server / DB / Collection + MongoDB Version Strip -------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 34
            radius: 8
            color: Theme.surfaceColor
            border.width: 1
            border.color: Theme.glassBorder

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 6

                RagoBadge {
                    visible: dlg.serverLabel.length > 0
                    text: "⛁ " + dlg.serverLabel
                    badgeColor: Theme.disabledTextColor
                }
                RagoBadge {
                    text: "DB: " + dlg.dbName
                    badgeColor: "#3b9eff"
                }
                RagoBadge {
                    text: "▤ " + dlg.collName
                    badgeColor: Theme.accentColor
                }
                Item { Layout.fillWidth: true }
                RagoBadge {
                    visible: dlg.serverVerText.length > 0 || dlg.serverVer > 0
                    text: "MongoDB " + (dlg.serverVerText.length > 0
                                        ? dlg.serverVerText
                                        : dlg.serverVer.toFixed(1))
                    badgeColor: "#d49a35"
                }
            }
        }

        // ---- 4-Tab Segmented Bar -------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            radius: 8
            color: Theme.surfaceColor
            border.width: 1
            border.color: Theme.glassBorder

            TabBar {
                id: idxTabBar
                anchors.fill: parent
                anchors.margins: 3
                spacing: 4
                background: null

                Repeater {
                    model: [
                        qsTr("Basic"),
                        qsTr("Advanced"),
                        qsTr("Partial & Collation"),
                        qsTr("Text Search")
                    ]
                    TabButton {
                        id: segBtn
                        required property int index
                        required property string modelData
                        text: modelData
                        implicitHeight: 28
                        width: (idxTabBar.width - idxTabBar.spacing * (idxTabBar.count - 1)) / idxTabBar.count
                        contentItem: Label {
                            text: segBtn.text
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            font.bold: idxTabBar.currentIndex === segBtn.index
                            color: idxTabBar.currentIndex === segBtn.index
                                   ? Theme.textColor : Theme.disabledTextColor
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: 6
                            color: idxTabBar.currentIndex === segBtn.index
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
            contentHeight: pages.height
            rightPadding: pages.implicitHeight > availableHeight ? 8 : 0
            clip: true

            StackLayout {
                id: pages
                width: pageScroll.availableWidth
                height: Math.max(implicitHeight, pageScroll.availableHeight)
                Layout.minimumWidth: 0
                currentIndex: idxTabBar.currentIndex

            // ================================================================
            // Tab 0: Basic (Name, Keys JSON + templates, Unique)
            // ================================================================
            ColumnLayout {
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        text: qsTr("Name")
                        font.pixelSize: 12
                        font.bold: true
                        color: Theme.textColor
                        Layout.preferredWidth: 52
                    }
                    RagoTextField {
                        id: nameField
                        placeholderText: qsTr("e.g. agendaId_1")
                        font.family: AppSettings.codeFontFamily
                        font.pixelSize: AppSettings.codeFontSize
                        Layout.fillWidth: true
                    }
                    RagoPillButton {
                        text: qsTr("Suggest Name")
                        implicitHeight: 28
                        onClicked: dlg.suggestIndexNameFromKeys()
                    }
                }

                // The templates wrap under the title on a narrow screen.
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    Label {
                        text: qsTr("Keys (JSON)")
                        font.pixelSize: 12
                        font.bold: true
                        color: Theme.textColor
                    }
                    RagoPillButton {
                        compact: true
                        text: qsTr("Compound")
                        onClicked: keysEditor.setText("{\n  \"field1\": 1,\n  \"field2\": -1\n}")
                    }
                    RagoPillButton {
                        compact: true
                        text: qsTr("Text")
                        onClicked: keysEditor.setText("{\n  \"title\": \"text\"\n}")
                    }
                    RagoPillButton {
                        compact: true
                        text: qsTr("2dsphere")
                        onClicked: keysEditor.setText("{\n  \"location\": \"2dsphere\"\n}")
                    }
                    RagoPillButton {
                        compact: true
                        visible: dlg.supportsWildcard
                        text: qsTr("Wildcard ($**)")
                        onClicked: keysEditor.setText("{\n  \"$**\": 1\n}")
                    }
                    RagoPillButton {
                        compact: true
                        text: qsTr("Format")
                        onClicked: {
                            var f = Backend.formatJson(keysEditor.text)
                            if (f.length > 0) keysEditor.setText(f)
                        }
                    }
                }

                RagoCodeEditor {
                    id: keysEditor
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    folding: false
                    showLineNumbers: true
                }

                Label {
                    text: qsTr("Enter a JSON object where each field maps to 1 (ascending), -1 (descending), \"text\", \"2dsphere\", or \"hashed\".")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.disabledTextColor
                    Layout.fillWidth: true
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: uniqueCol.implicitHeight + 16
                    radius: 8
                    color: Theme.surfaceColor
                    border.width: 1
                    border.color: Theme.glassBorder

                    ColumnLayout {
                        id: uniqueCol
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 3

                        RagoCheckBox {
                            id: uniqueCheck
                            //: Checkbox: the index rejects duplicate values of the indexed fields.
                            text: qsTr("Unique")
                        }
                        Label {
                            text: qsTr("MongoDB rejects documents that contain a duplicate value for the indexed field(s).")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 11
                            color: Theme.disabledTextColor
                            Layout.leftMargin: 26
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            // ================================================================
            // Tab 1: Advanced (Sparse, TTL toggle + seconds, Hidden, Background)
            // ================================================================
            ScrollView {
                clip: true

                ColumnLayout {
                    width: parent.width
                    spacing: 10

                    // Sparse
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: sparseCol.implicitHeight + 16
                        radius: 8
                        color: Theme.surfaceColor
                        border.width: 1
                        border.color: Theme.glassBorder

                        ColumnLayout {
                            id: sparseCol
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 3

                            RagoCheckBox {
                                id: sparseCheck
                                text: qsTr("Drop duplicates / Sparse index")
                            }
                            Label {
                                text: qsTr("Only references documents with the specified field(s). Documents missing the indexed field are skipped.")
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Theme.disabledTextColor
                                Layout.leftMargin: 26
                                Layout.fillWidth: true
                            }
                        }
                    }

                    // Expire after (TTL)
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: ttlCol.implicitHeight + 16
                        radius: 8
                        color: Theme.surfaceColor
                        border.width: 1
                        border.color: Theme.glassBorder

                        ColumnLayout {
                            id: ttlCol
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10
                                RagoCheckBox {
                                    id: ttlCheck
                                    text: qsTr("Expire after (TTL)")
                                }
                                RagoTextField {
                                    id: ttlField
                                    ltr: true
                                    enabled: ttlCheck.checked
                                    Layout.preferredWidth: 110
                                    implicitHeight: 28
                                    font.family: AppSettings.codeFontFamily
                                    font.pixelSize: AppSettings.codeFontSize
                                    horizontalAlignment: TextInput.AlignRight
                                    text: "3600"
                                }
                                Label {
                                    text: qsTr("seconds")
                                    font.pixelSize: 12
                                    color: ttlCheck.checked ? Theme.textColor : Theme.disabledTextColor
                                }
                                Item { Layout.fillWidth: true }
                            }
                            Label {
                                text: qsTr("Specifies a TTL in seconds to automatically delete expired documents on a Date / Date-array field.")
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Theme.disabledTextColor
                                Layout.leftMargin: 26
                                Layout.fillWidth: true
                            }
                        }
                    }

                    // Hidden Index (MongoDB 4.4+)
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: hiddenCol.implicitHeight + 16
                        radius: 8
                        color: Theme.surfaceColor
                        border.width: 1
                        border.color: Theme.glassBorder
                        opacity: dlg.supportsHidden ? 1.0 : 0.6

                        ColumnLayout {
                            id: hiddenCol
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 3

                            RowLayout {
                                Layout.fillWidth: true
                                RagoCheckBox {
                                    id: hiddenCheck
                                    enabled: dlg.supportsHidden
                                    text: qsTr("Hide index from query planner (hidden)")
                                }
                                Item { Layout.fillWidth: true }
                                RagoBadge {
                                    text: "MongoDB 4.4+"
                                    badgeColor: dlg.supportsHidden ? Theme.accentColor : Theme.disabledTextColor
                                }
                            }
                            Label {
                                text: dlg.supportsHidden
                                      ? qsTr("Maintains the index on writes while hiding it from the query planner so you can safely evaluate dropping it.")
                                      : qsTr("Hidden indexes require MongoDB 4.4 or newer (connected server does not support 'hidden').")
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Theme.disabledTextColor
                                Layout.leftMargin: 26
                                Layout.fillWidth: true
                            }
                        }
                    }

                    // Background Index (Legacy < 4.2 vs Hybrid >= 4.2)
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: bgCol.implicitHeight + 16
                        radius: 8
                        color: Theme.surfaceColor
                        border.width: 1
                        border.color: Theme.glassBorder
                        opacity: dlg.backgroundDeprecated ? 0.65 : 1.0

                        ColumnLayout {
                            id: bgCol
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 3

                            RowLayout {
                                Layout.fillWidth: true
                                RagoCheckBox {
                                    id: bgCheck
                                    enabled: !dlg.backgroundDeprecated
                                    text: qsTr("Create index in background")
                                }
                                Item { Layout.fillWidth: true }
                                RagoBadge {
                                    text: dlg.backgroundDeprecated ? qsTr("Auto Hybrid (4.2+)") : "MongoDB < 4.2"
                                    badgeColor: dlg.backgroundDeprecated ? "#3b9eff" : Theme.disabledTextColor
                                }
                            }
                            Label {
                                text: dlg.backgroundDeprecated
                                      ? qsTr("MongoDB 4.2+ automatically uses an optimized non-blocking hybrid index build; the legacy 'background' flag is deprecated and ignored by the server.")
                                      : qsTr("Builds the index in the background so that index creation does not block read/write operations on MongoDB < 4.2.")
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Theme.disabledTextColor
                                Layout.leftMargin: 26
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }

            // ================================================================
            IndexFilterCollationTab {
                id: filterCollationTab
                dlg: dlg
            }

            IndexTextSearchTab {
                id: textSearchTab
            }
        }
        }

        Label {
            visible: dlg.errorMessage.length > 0
            text: dlg.errorMessage
            color: "#ff6b6b"
            wrapMode: Text.WordWrap
            font.pixelSize: 12
            Layout.fillWidth: true
        }
    }
}
