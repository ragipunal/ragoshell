pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

RagoMenu {
    id: resultContextMenu
    required property var page
    property string filterText: ""
    width: 276

    RagoMenuItem {
        text: qsTr("Edit Document…")
        keys: "Enter"
        visible: resultContextMenu.page.shell ? resultContextMenu.page.shell.canModifyDocuments : false
        enabled: resultContextMenu.page.selectedDocIndices.length === 1
        onTriggered: resultContextMenu.page.triggerEditSelectedDocument()
    }
    RagoMenuItem {
        text: qsTr("View Document…")
        enabled: resultContextMenu.page.selectedDocIndices.length >= 1
        onTriggered: resultContextMenu.page.triggerViewSelectedDocument()
    }
    RagoMenuItem {
        text: qsTr("Insert Document…")
        keys: "Ctrl+D"
        visible: resultContextMenu.page.shell ? resultContextMenu.page.shell.canInsertDocuments : false
        onTriggered: resultContextMenu.page.triggerInsertDocument()
    }
    RagoMenuItem {
        text: qsTr("Duplicate Document…")
        visible: resultContextMenu.page.shell ? resultContextMenu.page.shell.canModifyDocuments : false
        enabled: resultContextMenu.page._ctxDocIndex >= 0
        onTriggered: resultContextMenu.page.triggerDuplicateDocument(resultContextMenu.page._ctxDocIndex)
    }
    RagoMenuItem {
        text: resultContextMenu.page.selectedDocIndices.length > 1
              ? qsTr("Delete %n Documents…", "", resultContextMenu.page.selectedDocIndices.length)
              : qsTr("Delete Document…")
        keys: Platform.usesCommandKey ? "Ctrl+Backspace" : "Del"
        danger: true
        visible: resultContextMenu.page.shell ? resultContextMenu.page.shell.canDeleteDocuments : false
        enabled: resultContextMenu.page.selectedDocIndices.length > 0
        onTriggered: resultContextMenu.page.triggerDeleteSelectedDocuments()
    }
    RagoMenuItem {
        text: qsTr("Select All")
        keys: "Ctrl+A"
        enabled: resultContextMenu.page.shell ? resultContextMenu.page.shell.resultCount > 0 : false
        onTriggered: resultContextMenu.page.selectAllDocuments()
    }

    RagoMenuSeparator {}

    RagoMenuItem {
        text: qsTr("Copy Value")
        keys: "Ctrl+C"
        enabled: resultContextMenu.page._ctxRawValue.length > 0
        onTriggered: if (resultContextMenu.page.shell) resultContextMenu.page.shell.copyToClipboard(resultContextMenu.page._ctxRawValue)
    }
    RagoMenuItem {
        text: qsTr("Copy Field Name")
        enabled: resultContextMenu.page._ctxKey.length > 0
        onTriggered: if (resultContextMenu.page.shell) resultContextMenu.page.shell.copyToClipboard(resultContextMenu.page._ctxKey)
    }
    RagoMenuItem {
        text: qsTr("Copy Field Path")
        shortcutText: resultContextMenu.page._ctxFieldPath.length > 20
                      ? (resultContextMenu.page._ctxFieldPath.slice(0, 19) + "…")
                      : resultContextMenu.page._ctxFieldPath
        enabled: resultContextMenu.page._ctxFieldPath.length > 0
        onTriggered: if (resultContextMenu.page.shell) resultContextMenu.page.shell.copyToClipboard(resultContextMenu.page._ctxFieldPath)
    }
    RagoMenuItem {
        text: qsTr("Copy Document JSON")
        keys: "Ctrl+Shift+C"
        enabled: resultContextMenu.page.selectedDocIndices.length > 0
        onTriggered: resultContextMenu.page.copySelectedValueOrDoc(true)
    }
    RagoMenuItem {
        text: qsTr("Copy Timestamp from ObjectId")
        visible: resultContextMenu.page._ctxOidIso.length > 0
        onTriggered: if (resultContextMenu.page.shell) resultContextMenu.page.shell.copyToClipboard(resultContextMenu.page._ctxOidIso)
    }

    RagoMenuSeparator {
        visible: resultContextMenu.page.shell ? resultContextMenu.page.shell.viewMode === 0 && resultContextMenu.page._ctxTreeRow >= 0 : false
    }

    RagoMenuItem {
        text: qsTr("Expand Recursively")
        keys: "Alt+Right"
        visible: resultContextMenu.page.shell ? resultContextMenu.page.shell.viewMode === 0 && resultContextMenu.page._ctxTreeRow >= 0 : false
        enabled: resultContextMenu.page._ctxExpandable
        onTriggered: resultContextMenu.page.expandTreeSubtree(resultContextMenu.page._ctxTreeRow, true)
    }
    RagoMenuItem {
        text: qsTr("Collapse Recursively")
        keys: "Alt+Left"
        visible: resultContextMenu.page.shell ? resultContextMenu.page.shell.viewMode === 0 && resultContextMenu.page._ctxTreeRow >= 0 : false
        enabled: resultContextMenu.page._ctxExpandable
        onTriggered: resultContextMenu.page.expandTreeSubtree(resultContextMenu.page._ctxTreeRow, false)
    }

    RagoMenuSeparator {
        visible: resultContextMenu.page.shell ? resultContextMenu.page.shell.activeCollection.length > 0 : false
    }

    RagoMenuItem {
        text: qsTr("Export Collection…")
        visible: resultContextMenu.page.shell ? resultContextMenu.page.shell.activeCollection.length > 0 : false
        onTriggered: {
            var dlg = resultContextMenu.page._ensureImportExportDialog()
            if (dlg)
                dlg.openExport(
                    resultContextMenu.page.shell.serverUuid,
                    resultContextMenu.page.shell.activeDatabase,
                    resultContextMenu.page.shell.activeCollection,
                    resultContextMenu.filterText || "{}")
        }
    }
    RagoMenuItem {
        text: qsTr("Import Data…")
        visible: resultContextMenu.page.shell ? resultContextMenu.page.shell.activeCollection.length > 0 : false
        onTriggered: {
            var dlg = resultContextMenu.page._ensureImportExportDialog()
            if (dlg)
                dlg.openImport(
                    resultContextMenu.page.shell.serverUuid,
                    resultContextMenu.page.shell.activeDatabase,
                    resultContextMenu.page.shell.activeCollection)
        }
    }
}
