pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

ColumnLayout {
    id: root
    spacing: 6

    property int selectedMemberIndex: 0

    ListModel {
        id: rsMembersModel
    }

    function normalizeMemberAddress(raw) {
        var s = String(raw || "").trim().replace(/\s+/g, "")
        if (s.length === 0)
            return ""

        // IPv6 bracketed address: [::1]:27017 or [::1]
        if (s.charAt(0) === "[") {
            var closeIdx = s.indexOf("]")
            if (closeIdx > 0) {
                var ipv6Host = s.substring(0, closeIdx + 1)
                var rest = s.substring(closeIdx + 1)
                if (rest.charAt(0) === ":") {
                    var p6 = parseInt(rest.substring(1).replace(/\D/g, ""), 10)
                    return ipv6Host + ":" + ((p6 > 0 && p6 <= 65535) ? p6 : 27017)
                }
                return ipv6Host + ":27017"
            }
        }

        var colonIdx = s.lastIndexOf(":")
        if (colonIdx > 0) {
            var hostPart = s.substring(0, colonIdx)
            var portDigits = s.substring(colonIdx + 1).replace(/\D/g, "")
            var portNum = parseInt(portDigits, 10)
            return hostPart + ":" + ((portNum > 0 && portNum <= 65535) ? portNum : 27017)
        }
        return s + ":27017"
    }

    function setMembersFromArray(arr) {
        rsMembersModel.clear()
        for (var i = 0; i < arr.length; ++i) {
            var norm = normalizeMemberAddress(arr[i])
            if (norm.length > 0)
                rsMembersModel.append({ "address": norm })
        }
        if (rsMembersModel.count === 0)
            rsMembersModel.append({ "address": "localhost:27017" })
        selectedMemberIndex = 0
    }

    function splitPastedMembersAt(rowIdx, rawText) {
        var tokens = String(rawText || "").split(/[,;\r\n]+/)
        var normalized = []
        for (var i = 0; i < tokens.length; ++i) {
            var norm = normalizeMemberAddress(tokens[i])
            if (norm.length > 0)
                normalized.push(norm)
        }
        if (normalized.length === 0) {
            rsMembersModel.setProperty(rowIdx, "address", "")
            return
        }
        rsMembersModel.setProperty(rowIdx, "address", normalized[0])
        for (var k = 1; k < normalized.length; ++k)
            rsMembersModel.insert(rowIdx + k, { "address": normalized[k] })
        selectedMemberIndex = rowIdx + normalized.length - 1
        Qt.callLater(function() {
            membersListView.positionViewAtIndex(selectedMemberIndex, ListView.Contain)
        })
    }

    function addMemberRow() {
        var nextAddr = "localhost:27017"
        if (rsMembersModel.count > 0) {
            var lastIdx = rsMembersModel.count - 1
            var lastNorm = normalizeMemberAddress(rsMembersModel.get(lastIdx).address)
            if (lastNorm.length > 0) {
                rsMembersModel.setProperty(lastIdx, "address", lastNorm)
                var colon = lastNorm.lastIndexOf(":")
                if (colon > 0) {
                    var h = lastNorm.substring(0, colon)
                    var p = parseInt(lastNorm.substring(colon + 1), 10) || 27017
                    nextAddr = h + ":" + Math.min(65535, p + 1)
                } else {
                    nextAddr = "localhost:" + (27017 + rsMembersModel.count)
                }
            } else {
                rsMembersModel.setProperty(lastIdx, "address", nextAddr)
                selectedMemberIndex = lastIdx
                return
            }
        }
        rsMembersModel.append({ "address": nextAddr })
        selectedMemberIndex = rsMembersModel.count - 1
        Qt.callLater(function() {
            membersListView.positionViewAtEnd()
        })
    }

    function removeMemberRow(targetIdx) {
        if (rsMembersModel.count === 0)
            return
        var idx = (targetIdx !== undefined && targetIdx >= 0 && targetIdx < rsMembersModel.count)
                  ? targetIdx
                  : ((selectedMemberIndex >= 0 && selectedMemberIndex < rsMembersModel.count)
                     ? selectedMemberIndex
                     : (rsMembersModel.count - 1))
        if (rsMembersModel.count <= 1) {
            rsMembersModel.setProperty(0, "address", "")
            selectedMemberIndex = 0
            return
        }
        rsMembersModel.remove(idx)
        if (selectedMemberIndex >= rsMembersModel.count)
            selectedMemberIndex = rsMembersModel.count - 1
    }

    function collectCleanedMembers() {
        var clean = []
        for (var i = 0; i < rsMembersModel.count; ++i) {
            var raw = rsMembersModel.get(i).address
            var parts = String(raw || "").split(/[,;\r\n]+/)
            for (var p = 0; p < parts.length; ++p) {
                var norm = normalizeMemberAddress(parts[p])
                if (norm.length > 0)
                    clean.push(norm)
            }
        }
        return clean
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 118
        radius: 8
        color: Theme.glassBackground
        border.width: 1
        border.color: Theme.glassBorder

        ListView {
            id: membersListView
            anchors.fill: parent
            anchors.margins: 4
            clip: true
            spacing: 3
            model: rsMembersModel

            delegate: Rectangle {
                id: memberRow
                required property int index
                required property string address

                width: membersListView.width
                height: 28
                radius: 6
                color: (root.selectedMemberIndex === memberRow.index || memberInput.activeFocus)
                       ? Theme.selectedColor
                       : (rowHover.hovered ? Theme.cardBackgroundHover : "transparent")
                border.width: memberInput.activeFocus ? 1 : 0
                border.color: Theme.accentColor

                HoverHandler { id: rowHover }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 4
                    spacing: 6

                    Label {
                        text: "●"
                        font.pixelSize: 8
                        color: Theme.accentColor
                        Layout.alignment: Qt.AlignVCenter
                    }

                    TextInput {
                        id: memberInput
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        text: memberRow.address
                        color: Theme.textColor
                        selectionColor: Theme.accentColor
                        selectedTextColor: "#ffffff"
                        font.family: AppSettings.codeFontFamily
                        font.pixelSize: 12
                        selectByMouse: true
                        clip: true

                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: memberInput.text.length === 0
                            text: "host:27017"
                            font.family: memberInput.font.family
                            font.pixelSize: 12
                            color: Theme.disabledTextColor
                        }

                        onActiveFocusChanged: {
                            if (activeFocus)
                                root.selectedMemberIndex = memberRow.index
                        }

                        onTextEdited: {
                            if (/[,;\r\n]/.test(text)) {
                                root.splitPastedMembersAt(memberRow.index, text)
                            } else {
                                rsMembersModel.setProperty(memberRow.index, "address", text)
                            }
                        }

                        onEditingFinished: {
                            var norm = root.normalizeMemberAddress(text)
                            if (norm !== text)
                                text = norm
                            rsMembersModel.setProperty(memberRow.index, "address", norm)
                        }
                    }

                    RagoIconButton {
                        glyph: "✕"
                        size: 22
                        danger: true
                        opacity: (rowHover.hovered || memberInput.activeFocus) ? 1.0 : 0.45
                        toolTipText: qsTr("Remove Member")
                        onClicked: root.removeMemberRow(memberRow.index)
                    }
                }
            }

            ScrollBar.vertical: ScrollBar {}
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        Label {
            text: qsTr("Tip: Add one host:port per row, or paste comma-separated hosts")
            font.pixelSize: 11
            color: Theme.disabledTextColor
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.minimumWidth: 0
        }

        RagoIconButton {
            glyph: "+"
            size: 26
            accent: true
            toolTipText: qsTr("Add Replica Set Member")
            onClicked: root.addMemberRow()
        }

        RagoIconButton {
            glyph: "−"
            size: 26
            danger: true
            toolTipText: qsTr("Remove Selected Member")
            onClicked: root.removeMemberRow(root.selectedMemberIndex)
        }
    }
}
