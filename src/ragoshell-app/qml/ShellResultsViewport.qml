pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

ColumnLayout {
    id: viewport
    required property var page
    spacing: 0

    function forceLayouts() {
        resultTreeView.forceTreeLayout()
        resultTableView.forceTableLayout()
    }

    function expandSubtree(row, expand) {
        resultTreeView.expandSubtree(row, expand)
    }

    function expandFirstRow() {
        resultTreeView.expandFirstRow()
    }

    function activeJsonEditor() {
        return jsonEditorLoader.item
    }

    function syncPagingInputs() {
        skipInput.text = viewport.page.shell ? String(viewport.page.shell.currentSkip) : "0"
        batchInput.text = viewport.page.shell ? String(viewport.page.shell.currentBatchSize) : "50"
    }

    Item {
        id: resultStackHost
        Layout.fillWidth: true
        Layout.fillHeight: true

        StackLayout {
            id: stack
            anchors.fill: parent
            // Result documents (tree, table, JSON) stay left to right in a
            // right-to-left UI.
            LayoutMirroring.enabled: false
            LayoutMirroring.childrenInherit: true
            enabled: !viewport.page.isShellBusy
            opacity: (viewport.page.isShellBusy && viewport.page.shell && viewport.page.shell.resultCount > 0) ? 0.45 : 1.0
            currentIndex: viewport.page.shell ? viewport.page.shell.viewMode : 0


            ShellResultTreeView {
                id: resultTreeView
                Layout.minimumWidth: 0
                page: viewport.page
            }

            ShellResultTableView {
                id: resultTableView
                Layout.minimumWidth: 0
                page: viewport.page
            }

        // ==== 2: Colored & Foldable JSON / Text Mode ============
        Rectangle {
            Layout.minimumWidth: 0
            color: Theme.baseColor

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // JSON View quick bar
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: Math.max(30, jsonBarRow.implicitHeight + 8)
                    Layout.preferredHeight: implicitHeight
                    visible: viewport.page.shell ? viewport.page.shell.hasTextOutput : false
                    color: Theme.surfaceColor

                    RowLayout {
                        id: jsonBarRow
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 6

                        Label {
                            text: qsTr("JSON / Output Viewer")
                            font.pixelSize: 11
                            font.bold: true
                            color: Theme.disabledTextColor
                        }

                        Item { Layout.fillWidth: true }

                        RagoPillButton {
                            compact: true
                            text: qsTr("Collapse")
                            implicitHeight: 22
                            onClicked: { const ed = jsonEditorLoader.item as RagoCodeEditor; if (ed) ed.foldAll(false) }
                        }
                        RagoPillButton {
                            compact: true
                            text: qsTr("Expand")
                            implicitHeight: 22
                            onClicked: { const ed = jsonEditorLoader.item as RagoCodeEditor; if (ed) ed.foldAll(true) }
                        }
                        RagoPillButton {
                            compact: true
                            //: Button that runs a MongoDB find() query. Keep "find" recognisable; it is not a text search.
                            text: qsTr("Find")
                            implicitHeight: 22
                            onClicked: { const ed = jsonEditorLoader.item as RagoCodeEditor; if (ed) ed.openFindBar() }
                        }
                        RagoPillButton {
                            compact: true
                            text: qsTr("Copy All")
                            implicitHeight: 22
                            onClicked: if (viewport.page.shell) viewport.page.shell.copyToClipboard(viewport.page.shell.textResult)
                        }
                    }

                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 1
                        color: Theme.glassBorder
                    }
                }

                Loader {
                    id: jsonEditorLoader
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    active: viewport.page.shell ? viewport.page.shell.viewMode === 2 : false
                    sourceComponent: Component {
                        RagoCodeEditor {
                            framed: false
                            readOnly: true
                            lineNumbers: true
                            folding: true
                            text: (viewport.page.shell && viewport.page.shell.viewMode === 2) ? viewport.page.shell.textResult : ""
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: viewport.page.shell ? (!viewport.page.shell.busy && !viewport.page.shell.cancelled && !viewport.page.shell.hasTextOutput && viewport.page.shell.errorText.length === 0) : true
                text: qsTr("No JSON / text output yet.")
                font.pixelSize: 12
                color: Theme.disabledTextColor
            }
        }

        // ==== 3: Collection Statistics Summary Mode =============
        Rectangle {
            Layout.minimumWidth: 0
            color: Theme.baseColor

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    color: Theme.surfaceColor

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 8

                        Label { text: qsTr("Collection"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 180 }
                        Label { text: qsTr("Documents"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 96 }
                        Label { text: qsTr("Data Size"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 105 }
                        Label { text: qsTr("Storage Size"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 105 }
                        Label { text: qsTr("Total Index Size"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 115 }
                        Label { text: qsTr("Avg Object"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 96 }
                        //: Column header: number of indexes of a collection.
                        Label { text: qsTr("Indexes"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.preferredWidth: 70 }
                        //: Column header: whether the collection is a capped collection.
                        Label { text: qsTr("Capped"); font.bold: true; font.pixelSize: 11; color: Theme.disabledTextColor; Layout.fillWidth: true }
                    }
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: viewport.page.shell ? viewport.page.shell.statsSummaryRows : []
                    delegate: Rectangle {
                        id: statsRow
                        required property int index
                        required property var modelData
                        width: ListView.view.width
                        height: Math.max(30, AppSettings.codeFontSize + 16)
                        color: statsRow.index % 2 === 1 ? Theme.alternateColor : "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 8

                            Label { text: statsRow.modelData.name || ""; font.family: AppSettings.codeFontFamily; font.bold: true; font.pixelSize: AppSettings.codeFontSize; color: Theme.textColor; elide: Text.ElideRight; Layout.preferredWidth: 180 }
                            Label { text: statsRow.modelData.count || "0"; font.family: AppSettings.codeFontFamily; font.pixelSize: AppSettings.codeFontSize; color: Theme.textColor; Layout.preferredWidth: 96 }
                            Label { text: statsRow.modelData.size || ""; font.family: AppSettings.codeFontFamily; font.pixelSize: AppSettings.codeFontSize; color: Theme.textColor; Layout.preferredWidth: 105 }
                            Label { text: statsRow.modelData.storageSize || ""; font.family: AppSettings.codeFontFamily; font.pixelSize: AppSettings.codeFontSize; color: Theme.textColor; Layout.preferredWidth: 105 }
                            Label { text: statsRow.modelData.totalIndexSize || ""; font.family: AppSettings.codeFontFamily; font.pixelSize: AppSettings.codeFontSize; color: Theme.textColor; Layout.preferredWidth: 115 }
                            Label { text: statsRow.modelData.avgObjSize || ""; font.family: AppSettings.codeFontFamily; font.pixelSize: AppSettings.codeFontSize; color: Theme.textColor; Layout.preferredWidth: 96 }
                            Label { text: statsRow.modelData.nindexes || "0"; font.family: AppSettings.codeFontFamily; font.pixelSize: AppSettings.codeFontSize; color: Theme.textColor; Layout.preferredWidth: 70 }
                            Label { text: statsRow.modelData.capped || "No"; font.family: AppSettings.codeFontFamily; font.pixelSize: AppSettings.codeFontSize; color: Theme.disabledTextColor; Layout.fillWidth: true }
                        }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
            }
        }

        // ==== 4: Explain Plan Mode ==============================
        ExplainView {
            Layout.minimumWidth: 0
            shell: viewport.page.shell
        }
        } // end StackLayout (stack)

        // Scoped non-blocking loading card (never blocks Explorer, tabs, or editor)
        Rectangle {
            id: loadingOverlayCard
            visible: viewport.page.isShellBusy
            anchors.centerIn: parent
            z: 20
            width: Math.min(Math.max(240, parent.width - 32), 370)
            implicitHeight: loadingCardCol.implicitHeight + 32
            radius: 14
            color: Theme.surfaceColor
            border.width: 1
            border.color: Theme.accentColor

            ColumnLayout {
                id: loadingCardCol
                anchors.fill: parent
                anchors.margins: 16
                spacing: 10

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 10

                    BusyIndicator {
                        running: loadingOverlayCard.visible
                        Layout.preferredWidth: 24
                        Layout.preferredHeight: 24
                    }

                    Label {
                        text: (viewport.page.shell && viewport.page.shell.activeCollection.length > 0)
                              ? qsTr("Loading %1…").arg(
                                    (viewport.page.shell.activeDatabase.length > 0 ? (viewport.page.shell.activeDatabase + ".") : "")
                                    + viewport.page.shell.activeCollection)
                              : qsTr("Executing query…")
                        font.pixelSize: 13
                        font.bold: true
                        color: Theme.textColor
                        elide: Text.ElideRight
                        Layout.maximumWidth: 280
                    }
                }

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Elapsed: %1 s").arg((viewport.page._liveBusyElapsedMs / 1000.0).toFixed(1))
                    font.family: AppSettings.codeFontFamily
                    font.pixelSize: 11
                    color: Theme.disabledTextColor
                    horizontalAlignment: Text.AlignHCenter
                    Layout.fillWidth: true
                }

                RagoPillButton {
                    Layout.alignment: Qt.AlignHCenter
                    compact: true
                    text: Platform.withShortcut(qsTr("Cancel Operation (%1)"), Theme.touch ? "" : "F6")
                    iconText: "■"
                    danger: true
                    onClicked: viewport.page.stopCurrent()
                }
            }
        }
    } // end resultStackHost

    // ---- Two-Way Pagination Footer (find & aggregate) ----------
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: Theme.touch ? 36 : 32
        color: Theme.surfaceColor
        visible: viewport.page.shell ? viewport.page.shell.hasPaging : false

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: Theme.glassBorder
        }

        HScrollRow {
            anchors.fill: parent
            margin: 8
            spacing: 6

            RagoPillButton {
                compact: true
                text: Language.rtl ? "▶" : "◀"
                enabled: viewport.page.shell ? (!viewport.page.shell.busy && viewport.page.shell.canPagePrev) : false
                onClicked: viewport.page.shell.prevPage()
            }

            Label {
                text: qsTr("Skip:")
                font.pixelSize: 11
                color: Theme.disabledTextColor
            }

            RagoTextField {
                id: skipInput
                numeric: true
                ltr: true
                Layout.preferredWidth: 52
                Layout.minimumWidth: 42
                implicitHeight: 25
                leftPadding: 6
                rightPadding: 6
                font.family: AppSettings.codeFontFamily
                font.pixelSize: 12
                horizontalAlignment: TextInput.AlignHCenter
                text: viewport.page.shell ? String(viewport.page.shell.currentSkip) : "0"
                onAccepted: {
                    var s = parseInt(skipInput.text, 10)
                    var b = parseInt(batchInput.text, 10)
                    if (viewport.page.shell)
                        viewport.page.shell.loadPage(isNaN(s) ? 0 : s, isNaN(b) ? 50 : b)
                }
            }

            Label {
                text: qsTr("Batch:")
                font.pixelSize: 11
                color: Theme.disabledTextColor
            }

            RagoTextField {
                id: batchInput
                numeric: true
                ltr: true
                Layout.preferredWidth: 52
                Layout.minimumWidth: 42
                implicitHeight: 25
                leftPadding: 6
                rightPadding: 6
                font.family: AppSettings.codeFontFamily
                font.pixelSize: 12
                horizontalAlignment: TextInput.AlignHCenter
                text: viewport.page.shell ? String(viewport.page.shell.currentBatchSize) : "50"
                onAccepted: {
                    var s = parseInt(skipInput.text, 10)
                    var b = parseInt(batchInput.text, 10)
                    if (viewport.page.shell)
                        viewport.page.shell.loadPage(isNaN(s) ? 0 : s, isNaN(b) ? 50 : b)
                }
            }

            RagoPillButton {
                compact: true
                text: Language.rtl ? "◀" : "▶"
                enabled: viewport.page.shell ? (!viewport.page.shell.busy && viewport.page.shell.canPageNext) : false
                onClicked: viewport.page.shell.nextPage()
            }

            RagoPillButton {
                compact: true
                text: viewport.width < 430 ? "" : qsTr("Refresh")
                iconText: "↻"
                enabled: viewport.page.shell ? !viewport.page.shell.busy : false
                onClicked: viewport.page.shell.reloadCurrentPage()
            }

            Label {
                text: viewport.page.shell
                      ? qsTr("Showing %1–%2")
                            .arg(viewport.page.shell.resultCount > 0 ? viewport.page.shell.currentSkip + 1 : viewport.page.shell.currentSkip)
                            .arg(viewport.page.shell.currentSkip + viewport.page.shell.resultCount)
                      : ""
                font.pixelSize: 11
                color: Theme.disabledTextColor
            }

            Item { Layout.fillWidth: true }
        }
    }

}
