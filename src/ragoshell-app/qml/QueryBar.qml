import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Collapsible visual Collection Query Bar:
// Two-way live synchronized with the IntelliShell script editor.
Rectangle {
    id: bar
    property ShellController shell
    property string collectionOverride: ""

    property alias filterText: filterField.text
    property alias projectionText: projField.text
    property alias sortText: sortField.text
    property alias limitText: limitField.text

    // Preserved syntax metadata from the script editor so round-trip edits
    // keep the user's accessor style, skip, and explain options intact.
    property bool _useGetCollection: true
    property int _skip: 0
    property bool _explain: false
    property string _explainArg: ""
    property bool _syncLocked: false

    signal queryEdited(string builtScript)
    signal executeScriptRequested(string builtScript)

    implicitHeight: Math.max(76, AppSettings.codeFontSize * 2 + 52)
    color: Theme.surfaceColor
    border.width: 1
    border.color: Theme.glassBorder

    function focusFilter() {
        filterField.forceActiveFocus()
        filterField.selectAll()
    }

    function effectiveCollection() {
        return bar.collectionOverride.length > 0
               ? bar.collectionOverride
               : (bar.shell ? bar.shell.activeCollection : "")
    }

    function buildCurrentScript(forceExplain) {
        if (!bar.shell)
            return ""
        var coll = effectiveCollection()
        if (coll.length === 0)
            return ""
        var lim = parseInt(limitField.text, 10)
        if (isNaN(lim) || lim < 0)
            lim = 0
        var wantExplain = (forceExplain !== undefined) ? !!forceExplain : bar._explain
        return bar.shell.buildFindQuery(
            coll,
            bar._useGetCollection,
            filterField.text,
            projField.text,
            sortField.text,
            lim,
            bar._skip,
            wantExplain,
            bar._explainArg)
    }

    function syncFromParsed(parsed) {
        if (!parsed || !parsed.valid)
            return
        bar._syncLocked = true
        bar._useGetCollection = parsed.useGetCollection !== undefined ? !!parsed.useGetCollection : true
        bar._skip = parsed.skip || 0
        bar._explain = !!parsed.explain
        bar._explainArg = parsed.explainArg || ""
        var newFilter = parsed.filter || "{}"
        var newProj = parsed.projection || ""
        var newSort = parsed.sort || ""
        var newLim = (parsed.limit && parsed.limit > 0) ? String(parsed.limit) : ""
        if (filterField.text !== newFilter)
            filterField.text = newFilter
        if (projField.text !== newProj)
            projField.text = newProj
        if (sortField.text !== newSort)
            sortField.text = newSort
        if (limitField.text !== newLim)
            limitField.text = newLim
        bar._syncLocked = false
    }

    function _notifyUserEdit() {
        if (bar._syncLocked)
            return
        var built = buildCurrentScript(bar._explain)
        if (built.length > 0)
            bar.queryEdited(built)
    }

    function runQuery(explain) {
        if (!bar.shell)
            return
        bar._explain = !!explain
        var built = buildCurrentScript(!!explain)
        if (built.length > 0) {
            bar.executeScriptRequested(built)
        } else {
            let lim = parseInt(limitField.text, 10)
            if (isNaN(lim) || lim < 0)
                lim = 0
            bar.shell.executeQueryBar(effectiveCollection(),
                                      filterField.text,
                                      projField.text,
                                      sortField.text,
                                      lim,
                                      !!explain)
        }
    }

    function resetFields() {
        bar._syncLocked = true
        filterField.text = "{}"
        projField.text = ""
        sortField.text = ""
        limitField.text = ""
        bar._skip = 0
        bar._explain = false
        bar._explainArg = ""
        bar._syncLocked = false
        var built = buildCurrentScript(false)
        if (built.length > 0)
            bar.executeScriptRequested(built)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.topMargin: 6
        anchors.bottomMargin: 6
        spacing: 6

        // Row 1: Filter + Run / Explain / Reset
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Label {
                //: Label of the box with the MongoDB query filter (first argument of find).
                text: qsTr("Filter")
                font.pixelSize: 11
                font.bold: true
                color: Theme.accentColor
                Layout.preferredWidth: 48
            }

            RagoTextField {
                id: filterField
                ltr: true
                Layout.fillWidth: true
                implicitHeight: Math.max(28, AppSettings.codeFontSize + 15)
                font.family: AppSettings.codeFontFamily
                font.pixelSize: AppSettings.codeFontSize
                text: "{}"
                placeholderText: "{ status: \"active\" }"
                onTextEdited: bar._notifyUserEdit()
                onAccepted: bar.runQuery(false)
            }

            RagoPillButton {
                text: qsTr("Apply")
                iconText: "▶"
                primary: true
                implicitHeight: Math.max(28, AppSettings.codeFontSize + 15)
                enabled: bar.shell ? (!bar.shell.busy && bar.effectiveCollection().length > 0) : false
                onClicked: bar.runQuery(false)
            }

            RagoPillButton {
                //: Button that runs MongoDB explain() and shows the query plan.
                text: qsTr("Explain")
                iconText: "⚡"
                implicitHeight: Math.max(28, AppSettings.codeFontSize + 15)
                checked: bar._explain
                enabled: bar.shell ? (!bar.shell.busy && bar.effectiveCollection().length > 0) : false
                onClicked: bar.runQuery(true)
            }

            RagoPillButton {
                text: qsTr("Reset")
                implicitHeight: Math.max(28, AppSettings.codeFontSize + 15)
                onClicked: bar.resetFields()
            }
        }

        // Row 2: Project + Sort + Limit
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: qsTr("Project")
                font.pixelSize: 11
                color: Theme.disabledTextColor
                Layout.preferredWidth: 48
            }

            RagoTextField {
                id: projField
                ltr: true
                Layout.fillWidth: true
                implicitHeight: Math.max(26, AppSettings.codeFontSize + 13)
                font.family: AppSettings.codeFontFamily
                font.pixelSize: AppSettings.codeFontSize
                placeholderText: "{ field: 1 }"
                onTextEdited: bar._notifyUserEdit()
                onAccepted: bar.runQuery(false)
            }

            Label {
                //: Label of the box with the MongoDB sort document.
                text: qsTr("Sort")
                font.pixelSize: 11
                color: Theme.disabledTextColor
            }

            RagoTextField {
                id: sortField
                ltr: true
                Layout.fillWidth: true
                implicitHeight: Math.max(26, AppSettings.codeFontSize + 13)
                font.family: AppSettings.codeFontFamily
                font.pixelSize: AppSettings.codeFontSize
                placeholderText: "{ _id: -1 }"
                onTextEdited: bar._notifyUserEdit()
                onAccepted: bar.runQuery(false)
            }

            Label {
                //: Label of the box with the maximum number of documents to return.
                text: qsTr("Limit")
                font.pixelSize: 11
                color: Theme.disabledTextColor
            }

            RagoTextField {
                id: limitField
                ltr: true
                Layout.preferredWidth: 74
                implicitHeight: Math.max(26, AppSettings.codeFontSize + 13)
                font.family: AppSettings.codeFontFamily
                font.pixelSize: AppSettings.codeFontSize
                text: ""
                placeholderText: qsTr("none")
                horizontalAlignment: TextInput.AlignHCenter
                onTextEdited: bar._notifyUserEdit()
                onAccepted: bar.runQuery(false)
            }
        }
    }
}
