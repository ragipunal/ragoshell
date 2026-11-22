pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import RagoShell

// Preferences: edits SettingsManager via AppSettings; live-previews code &
// data font settings across editors/trees immediately and reverts cleanly on Cancel.
RagoDialog {
    id: dlg
    title: qsTr("Preferences")
    acceptText: qsTr("Save")
    rejectText: qsTr("Cancel")
    showReject: true
    width: Math.min(visibleWidth - 24, 560)

    property string _origFontFamily: ""
    property int _origFontSize: 13
    property bool _origLineNumbers: true
    property bool _loading: false
    property bool _saved: false
    property string _origLanguage: "system"
    // "mcp" scrolls to that section once the dialog has opened ("" = top).
    property string revealSection: ""

    // Language picker: entry 0 follows the OS, the others are the supported
    // languages by their own name (plus the English name, so a language that
    // is unreadable to the user can still be found).
    readonly property var _languageModel: {
        var items = [qsTr("System default (%1)").arg(Language.systemEndonym())]
        var langs = Language.languages
        for (var i = 0; i < langs.length; ++i)
            items.push(langs[i].code === "en" ? langs[i].endonym
                                              : langs[i].endonym + " · " + langs[i].englishName)
        return items
    }
    // A combo whose model holds translated text drops back to its first entry
    // when the language changes (the model is rebuilt), so the choices made
    // so far are kept across a language preview.
    property var _comboStash: []
    Connections {
        target: Language
        function onLanguageAboutToChange() {
            dlg._comboStash = [uuidCombo.currentIndex, tzCombo.currentIndex,
                               viewModeCombo.currentIndex, acCombo.currentIndex]
        }
        function onLanguageChanged() {
            if (dlg._comboStash.length === 4) {
                uuidCombo.currentIndex = dlg._comboStash[0]
                tzCombo.currentIndex = dlg._comboStash[1]
                viewModeCombo.currentIndex = dlg._comboStash[2]
                acCombo.currentIndex = dlg._comboStash[3]
            }
            languageCombo.currentIndex = dlg._languageIndex()
        }
    }
    function _languageIndex() {
        if (Language.current === "system")
            return 0
        var langs = Language.languages
        for (var i = 0; i < langs.length; ++i)
            if (langs[i].code === Language.current)
                return i + 1
        return 0
    }

    // AI providers: the key and model of each are edited side by side and
    // saved together; the provider combo shows one at a time.
    property var _aiKeys: ({})
    property var _aiModelSel: ({})
    property var _aiBaseUrls: ({})        // providers with an address field (OpenAI-compatible, Ollama)
    property var _aiTimeouts: ({})        // answer time limit (sec) of those providers
    // Origin the text in the key field was entered for. When the address
    // moves to another origin the field is emptied: a key typed for one
    // server is never sent to another (docs: T1).
    property string _aiKeyFieldOrigin: ""
    property bool _aiKeyClearedForOrigin: false
    readonly property bool _aiHasBase: AppSettings.aiHasBaseUrl(_aiShownProvider)
    readonly property bool _aiUsesKey: AppSettings.aiUsesKey(_aiShownProvider)   // false: Ollama
    readonly property bool _aiHasPresets: _aiShownProvider === "openai_compat"
    readonly property var _aiBaseCheck: AppSettings.checkAiBaseUrl(aiBaseField.text,
                                                                   _aiUsesKey && aiKeyField.text.trim().length > 0)
    property string _aiShownProvider: ""
    property var _aiModelList: []
    property bool _aiFetched: false
    property bool _aiSelectionRetired: false
    property string _aiKeyPlaceholder: ""
    property bool _aiKeyVisible: false
    property string _aiPastedKey: ""      // offered "Clear clipboard" for this text
    property string _aiPasteError: ""

    // Per-provider entry state that must not outlive a provider switch or
    // the dialog: the key goes back to hidden, old test results go away.
    function _aiResetEntry() {
        _aiKeyVisible = false
        _aiPastedKey = ""
        _aiPasteError = ""
        aiCatalog.clear()
    }
    // The clipboard is read only here, when the user presses Paste.
    function _aiPasteKey() {
        var r = Backend.pasteApiKey()
        if (r.error !== undefined) {
            _aiPasteError = r.error
            _aiPastedKey = ""
            return
        }
        aiKeyField.text = r.key
        _aiKeyFieldOrigin = _aiBaseCheck.origin
        _aiKeyClearedForOrigin = false
        aiCatalog.clear()
        _aiPasteError = ""
        _aiPastedKey = r.key
    }

    function _aiStash() {
        if (_aiShownProvider.length === 0)
            return
        _aiKeys[_aiShownProvider] = aiKeyField.text
        if (_aiHasBase) {
            _aiBaseUrls[_aiShownProvider] = aiBaseField.text.trim()
            _aiModelSel[_aiShownProvider] = aiModelField.text.trim()
            aiTimeoutSpin.commitText()
            _aiTimeouts[_aiShownProvider] = aiTimeoutSpin.value
        } else if (aiModelCombo.currentIndex >= 0) {
            _aiModelSel[_aiShownProvider] = aiModelCombo.currentText
        }
    }
    function _aiShow(provider) {
        _aiShownProvider = provider
        var hasBase = AppSettings.aiHasBaseUrl(provider)
        var models = AppSettings.aiModelsFor(provider).slice()
        var sel = _aiModelSel[provider] || models[0] || ""
        _aiFetched = AppSettings.aiModelsFetched(provider)
        // A saved model the provider no longer lists stays selectable, flagged.
        _aiSelectionRetired = _aiFetched && sel.length > 0 && models.indexOf(sel) < 0
        if (!hasBase && sel.length > 0 && models.indexOf(sel) < 0)
            models.unshift(sel)
        _aiModelList = models
        aiModelCombo.currentIndex = Math.max(0, models.indexOf(sel))
        if (hasBase) {
            // A new setup starts at the provider's default (Ollama) or the
            // first preset; the address stays editable.
            var base = _aiBaseUrls[provider] || AppSettings.aiDefaultBaseUrl(provider)
            if (base.length === 0 && provider === "openai_compat")
                base = AppSettings.aiPresets()[0].baseUrl
            aiBaseField.text = base
            aiModelField.text = sel
            aiTimeoutSpin.value = _aiTimeouts[provider] || AppSettings.aiTimeoutFor(provider)
        }
        aiKeyField.text = _aiKeys[provider] || ""
        _aiKeyFieldOrigin = hasBase ? AppSettings.checkAiBaseUrl(aiBaseField.text, false).origin : ""
        _aiKeyClearedForOrigin = false
        _aiUpdateKeyPlaceholder()
    }
    function _aiUpdateKeyPlaceholder() {
        var env = AppSettings.aiKeyEnvironmentVariable(_aiShownProvider, _aiHasBase ? aiBaseField.text : "")
        _aiKeyPlaceholder = env.length > 0 ? qsTr("%1 is used").arg(env)
                          : _aiShownProvider === "gemini" ? "AIza…"
                          : _aiHasBase ? qsTr("Not needed for a local server")
                          : "sk-ant-…"
    }
    // The address field changed: a key entered for another origin goes away.
    function _aiBaseEdited() {
        aiCatalog.clear()
        var origin = AppSettings.checkAiBaseUrl(aiBaseField.text, false).origin
        if (aiKeyField.text.length > 0 && origin !== _aiKeyFieldOrigin) {
            aiKeyField.text = ""
            _aiPastedKey = ""
            _aiKeyClearedForOrigin = true
        }
        if (aiKeyField.text.length === 0)
            _aiKeyFieldOrigin = origin
        _aiUpdateKeyPlaceholder()
    }

    function selectedFontFamily() {
        if (fontCombo.currentIndex >= 0 && fontCombo.currentIndex < AppSettings.availableCodeFonts.length)
            return AppSettings.availableCodeFonts[fontCombo.currentIndex]
        return fontCombo.currentText
    }

    onOpened: {
        _loading = true
        _saved = false
        _origFontFamily = AppSettings.codeFontFamily
        _origFontSize = AppSettings.codeFontSize
        _origLineNumbers = AppSettings.lineNumbers
        _origLanguage = Language.current
        languageCombo.currentIndex = _languageIndex()
        densityCombo.currentIndex = Math.max(0, Theme.availableDensities.indexOf(Theme.density))

        batchSpin.value = AppSettings.batchSize
        timeoutSpin.value = AppSettings.shellTimeoutSec
        uuidCombo.currentIndex = AppSettings.uuidEncoding
        tzCombo.currentIndex = AppSettings.timeZone
        viewModeCombo.currentIndex = AppSettings.defaultViewMode
        acCombo.currentIndex = AppSettings.autocompletionMode
        var fIdx = AppSettings.availableCodeFonts.indexOf(AppSettings.codeFontFamily)
        fontCombo.currentIndex = Math.max(0, fIdx)
        fontSizeSpin.value = AppSettings.codeFontSize
        lineNumbersCheck.checked = AppSettings.lineNumbers
        autoExpandCheck.checked = AppSettings.autoExpand
        trayCheck.checked = AppSettings.minimizeToTray
        startupScriptArea.text = AppSettings.startupScript
        confirmDestructiveCheck.checked = AppSettings.confirmDestructiveQueries
        aiEnabledCheck.checked = AppSettings.aiAssistantEnabled
        var keys = {}, models = {}, bases = {}, timeouts = {}
        for (var i = 0; i < AppSettings.aiProviders.length; ++i) {
            var p = AppSettings.aiProviders[i]
            keys[p] = AppSettings.aiApiKeyFor(p)
            models[p] = AppSettings.aiModelFor(p)
            if (AppSettings.aiHasBaseUrl(p)) {
                bases[p] = AppSettings.aiBaseUrlFor(p)
                timeouts[p] = AppSettings.aiTimeoutFor(p)
            }
        }
        _aiKeys = keys
        _aiModelSel = models
        _aiBaseUrls = bases
        _aiTimeouts = timeouts
        _aiShownProvider = ""
        _aiResetEntry()
        aiProviderCombo.currentIndex = Math.max(0, AppSettings.aiProviders.indexOf(AppSettings.aiProvider))
        _aiShow(AppSettings.aiProvider)
        aiValuesCheck.checked = AppSettings.aiIncludeSampleValues
        aiFallbackCheck.checked = AppSettings.aiAutoFallback
        schemaSection.modeIndex = AppSettings.schemaCollectionMode
        schemaSection.sampleSize = AppSettings.schemaBackgroundSampleSize
        schemaSection.maxCollections = AppSettings.schemaBackgroundMaxCollections
        schemaSection.unmeteredOnly = AppSettings.schemaUnmeteredOnly
        mcpSection.syncFields()
        accountSection.syncFields()
        updateCheck.checked = AppSettings.checkForUpdates
        usageStatsCheck.checked = AppSettings.shareAnonymousUsage
        _loading = false
        if (revealSection === "mcp" && mcpSection.visible)
            Qt.callLater(function() { prefsScroll.contentItem.contentY = Math.max(0, mcpSection.y - 8) })
        revealSection = ""
    }

    onAccepted: {
        batchSpin.commitText()
        timeoutSpin.commitText()
        fontSizeSpin.commitText()
        schemaSection.commitSpinBoxes()
        mcpSection.commitSpinBoxes()
        accountSection.commitDeviceName()
        _saved = true
        Theme.density = Theme.availableDensities[densityCombo.currentIndex]
        AppSettings.batchSize = batchSpin.value
        AppSettings.shellTimeoutSec = timeoutSpin.value
        AppSettings.uuidEncoding = uuidCombo.currentIndex
        AppSettings.timeZone = tzCombo.currentIndex
        AppSettings.defaultViewMode = viewModeCombo.currentIndex
        AppSettings.autocompletionMode = acCombo.currentIndex
        var fam = dlg.selectedFontFamily()
        if (fam && fam.length > 0)
            AppSettings.codeFontFamily = fam
        AppSettings.codeFontSize = fontSizeSpin.value
        AppSettings.lineNumbers = lineNumbersCheck.checked
        AppSettings.autoExpand = autoExpandCheck.checked
        AppSettings.minimizeToTray = trayCheck.checked
        AppSettings.startupScript = startupScriptArea.text
        AppSettings.confirmDestructiveQueries = confirmDestructiveCheck.checked
        AppSettings.aiAssistantEnabled = aiEnabledCheck.checked
        _aiStash()
        for (var i = 0; i < AppSettings.aiProviders.length; ++i) {
            var p = AppSettings.aiProviders[i]
            if (AppSettings.aiHasBaseUrl(p) && _aiBaseUrls[p] !== undefined) {
                // The address first: the key below is bound to its origin.
                var check = AppSettings.checkAiBaseUrl(_aiBaseUrls[p], false)
                AppSettings.setAiBaseUrlFor(p, check.ok ? check.normalized : _aiBaseUrls[p])
                if (_aiTimeouts[p] !== undefined)
                    AppSettings.setAiTimeoutFor(p, _aiTimeouts[p])
            }
            AppSettings.setAiApiKeyFor(p, _aiKeys[p] || "")
            if (_aiModelSel[p] !== undefined && (_aiModelSel[p].length > 0 || AppSettings.aiHasBaseUrl(p)))
                AppSettings.setAiModelFor(p, _aiModelSel[p])
        }
        AppSettings.aiProvider = _aiShownProvider
        AppSettings.aiIncludeSampleValues = aiValuesCheck.checked
        AppSettings.aiAutoFallback = aiFallbackCheck.checked
        AppSettings.schemaCollectionMode = schemaSection.modeIndex
        AppSettings.schemaUnmeteredOnly = schemaSection.unmeteredOnly
        AppSettings.schemaBackgroundSampleSize = schemaSection.sampleSize
        AppSettings.schemaBackgroundMaxCollections = schemaSection.maxCollections
        mcpSection.apply()
        AppSettings.checkForUpdates = updateCheck.checked
        AppSettings.shareAnonymousUsage = usageStatsCheck.checked
        AppSettings.save()
    }

    onClosed: {
        _aiResetEntry()
        if (!_saved && !_loading) {
            if (_origFontFamily.length > 0)
                AppSettings.codeFontFamily = _origFontFamily
            AppSettings.codeFontSize = _origFontSize
            AppSettings.lineNumbers = _origLineNumbers
            Language.current = _origLanguage
        }
    }

    // Never taller than the window; the settings scroll instead.
    height: Math.min(implicitHeight, visibleHeight - 24)

    // The focused field stays in view when the keyboard takes space.
    FocusReveal { flickable: prefsScroll.contentItem; space: dlg.visibleHeight }

    contentItem: ScrollView {
        id: prefsScroll
        clip: true
        contentWidth: availableWidth
        implicitHeight: prefsColumn.implicitHeight

        ColumnLayout {
            id: prefsColumn
            width: prefsScroll.availableWidth
            spacing: 14

            GridLayout {
                Layout.fillWidth: true
                columns: Window.width < 520 ? 1 : 2
                columnSpacing: 16
                rowSpacing: 10

                Label {
                    text: qsTr("Language")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                // Applies at once as a preview; Cancel restores the old language.
                RagoComboBox {
                    id: languageCombo
                    model: dlg._languageModel
                    Layout.preferredWidth: 260
                    onActivated: (index) => {
                        Language.current = index === 0 ? "system" : Language.languages[index - 1].code
                    }
                }

                Label {
                    text: qsTr("Interface density")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                // Automatic: touch sizes on tablets, compact on a desktop.
                RagoComboBox {
                    id: densityCombo
                    model: [qsTr("Automatic"), qsTr("Compact"), qsTr("Touch")]
                    Layout.preferredWidth: 260
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.glassBorder
            }

            Label {
                text: qsTr("QUERY & DISPLAY")
                font.bold: true
                font.pixelSize: 11
                font.letterSpacing: Language.rtl ? 0 : 1.1
                color: Theme.accentColor
            }

            GridLayout {
                Layout.fillWidth: true
                columns: Window.width < 520 ? 1 : 2
                columnSpacing: 16
                rowSpacing: 10

                Label {
                    text: qsTr("Batch size")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RagoSpinBox {
                    id: batchSpin
                    from: 1
                    to: AppSettings.maxBatchSize
                    Layout.preferredWidth: 200
                }

                Label {
                    text: qsTr("Shell timeout (sec)")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RagoSpinBox {
                    id: timeoutSpin
                    from: 1
                    to: 3600
                    Layout.preferredWidth: 200
                }

                Label {
                    text: qsTr("UUID encoding")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RagoComboBox {
                    id: uuidCombo
                    model: [qsTr("Default"), "Java Legacy", ".NET Legacy", "Python Legacy"]
                    Layout.preferredWidth: 200
                }

                Label {
                    text: qsTr("Time zone")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RagoComboBox {
                    id: tzCombo
                    model: ["UTC", qsTr("Local time")]
                    Layout.preferredWidth: 200
                }

                Label {
                    text: qsTr("Default result view")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RagoComboBox {
                    id: viewModeCombo
                    // Order matches AppSettings.defaultViewMode (0 Tree, 1 Table, 2 Text)
                    //: Result views: tree of documents, table of fields, or JSON text.
                    model: [qsTr("Tree"), qsTr("Table"), qsTr("Text (JSON)")]
                    Layout.preferredWidth: 200
                }

                Label {
                    text: qsTr("Autocompletion")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RagoComboBox {
                    id: acCombo
                    model: [qsTr("None"), qsTr("All"), qsTr("No collection names")]
                    Layout.preferredWidth: 200
                }

                Label {
                    text: qsTr("Code & data font")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RagoComboBox {
                    id: fontCombo
                    model: AppSettings.availableCodeFonts
                    Layout.preferredWidth: 200
                    font.family: dlg.selectedFontFamily().length > 0
                                 ? dlg.selectedFontFamily() : AppSettings.codeFontFamily
                    delegate: ItemDelegate {
                        id: fontDel
                        required property int index
                        required property string modelData
                        width: fontCombo.width - 8
                        implicitHeight: 32
                        highlighted: fontCombo.highlightedIndex === fontDel.index
                        hoverEnabled: true

                        contentItem: Label {
                            text: fontDel.modelData
                            font.family: fontDel.modelData
                            font.pixelSize: 13
                            color: {
                                if (Theme.dark)
                                    return (fontDel.down || fontDel.highlighted) ? "#4ade80" : Theme.textColor
                                return fontDel.down ? "#064e3b" : (fontDel.highlighted ? "#065f46" : Theme.textColor)
                            }
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: 8
                            elide: Text.ElideRight
                        }

                        background: Rectangle {
                            radius: 6
                            color: {
                                if (Theme.dark)
                                    return fontDel.down ? "#1f4735" : (fontDel.highlighted ? "#173628" : "transparent")
                                return fontDel.down ? "#cceee0" : (fontDel.highlighted ? "#e4f5ed" : "transparent")
                            }
                        }
                    }
                    onCurrentIndexChanged: {
                        if (!dlg._loading && dlg.opened) {
                            let fam = dlg.selectedFontFamily()
                            if (fam && fam.length > 0)
                                AppSettings.codeFontFamily = fam
                        }
                    }
                }

                Label {
                    text: qsTr("Code font size (px)")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RagoSpinBox {
                    id: fontSizeSpin
                    from: 10
                    to: 28
                    Layout.preferredWidth: 200
                    onValueChanged: {
                        if (!dlg._loading && dlg.opened)
                            AppSettings.codeFontSize = value
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: Math.max(36, fontSizeSpin.value + 20)
                radius: 8
                clip: true
                color: Theme.baseColor
                border.width: 1
                border.color: Theme.glassBorder

                Label {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    verticalAlignment: Text.AlignVCenter
                    text: "db.getCollection('users').find({ active: true }).limit(50)"
                    font.family: dlg.selectedFontFamily().length > 0
                                 ? dlg.selectedFontFamily() : AppSettings.codeFontFamily
                    font.pixelSize: fontSizeSpin.value
                    color: Theme.textColor
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.glassBorder
            }

            Label {
                text: qsTr("EDITOR & SHELL BEHAVIOR")
                font.bold: true
                font.pixelSize: 11
                font.letterSpacing: Language.rtl ? 0 : 1.1
                color: Theme.accentColor
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                RagoCheckBox {
                    id: lineNumbersCheck
                    Layout.fillWidth: true
                    text: qsTr("Show editor line numbers")
                    onToggled: {
                        if (!dlg._loading && dlg.opened)
                            AppSettings.lineNumbers = checked
                    }
                }

                RagoCheckBox {
                    id: autoExpandCheck
                    Layout.fillWidth: true
                    text: qsTr("Auto-expand first document in Tree view")
                }

                RagoCheckBox {
                    id: trayCheck
                    Layout.fillWidth: true
                    text: qsTr("Enable system tray icon")
                }

                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    text: qsTr("Startup script")
                    font.pixelSize: 13
                    color: Theme.textColor
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("JavaScript that runs before every query in a shell tab: helper functions and shortcuts. It applies to connections opened after you save.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.disabledTextColor
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 120
                    radius: 8
                    color: startupScriptArea.activeFocus ? Theme.surfaceColor : Theme.glassBackground
                    border.width: 1
                    border.color: startupScriptArea.activeFocus ? Theme.accentColor : Theme.glassBorder

                    ScrollView {
                        id: startupScroll
                        anchors.fill: parent
                        anchors.margins: 2
                        clip: true
                        // Wrapped (touch) text is as wide as the box, not as wide as its longest line.
                        contentWidth: Theme.touch ? availableWidth : -1
                        TextArea {
                            id: startupScriptArea
                            // Code reads left to right in every language.
                            LayoutMirroring.enabled: false
                            font.family: AppSettings.codeFontFamily
                            font.pixelSize: 12
                            color: Theme.textColor
                            selectByMouse: true
                            // Long lines wrap on a touch screen, where scrolling sideways inside a dialog is awkward.
                            wrapMode: Theme.touch ? TextArea.Wrap : TextArea.NoWrap
                            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                            placeholderText: "function top(n) { return db.users.find().limit(n) }"
                            background: null
                        }
                    }
                }

                RagoCheckBox {
                    id: confirmDestructiveCheck
                    Layout.fillWidth: true
                    text: qsTr("Warn before executing write/destructive shell commands (insert, update, delete, drop)")
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.glassBorder
            }

            Label {
                text: qsTr("SAVED PASSWORDS")
                font.bold: true
                font.pixelSize: 11
                font.letterSpacing: Language.rtl ? 0 : 1.1
                color: Theme.accentColor
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: AppSettings.secretStorageInKeyStore ? Theme.disabledTextColor : Theme.warningColor
                text: AppSettings.secretStorageInKeyStore
                      ? qsTr("Passwords and API keys are saved encrypted. The key that encrypts them is kept in %1, not in your settings folder.")
                            .arg(AppSettings.secretStorageName)
                      : qsTr("No key store is available on this computer, so the key that encrypts saved passwords and API keys is kept in a file next to your settings (%1). Anyone who can read that folder can read the saved passwords.")
                            .arg(Brand.productId + ".key")
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.glassBorder
            }

            Label {
                text: qsTr("AI QUERY ASSISTANT")
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
                text: qsTr("Turns a request in plain language into a find() or aggregate() query "
                           + "(Tools › AI Query Assistant). Generate sends the request and the "
                           + "collection's field names and types from a 1000-document sample to the "
                           + "selected provider: Anthropic's Claude API, Google's Gemini API (an "
                           + "API key from Google AI Studio) or any OpenAI-compatible server you choose. "
                           + "Nothing is sent until you press Generate.")
            }

            RagoCheckBox {
                id: aiEnabledCheck
                Layout.fillWidth: true
                text: qsTr("Enable the AI query assistant")
            }

            // Shown once after an update when the saved provider was the
            // removed hosted RagoShell AI: the assistant was turned off.
            Label {
                visible: AppSettings.aiHostedProviderRemoved
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.warningColor
                //: Preferences › AI assistant notice after an update (%1 = product name).
                text: qsTr("%1 AI (the hosted assistant) is no longer offered, so the assistant was "
                           + "turned off. Pick a provider, enter your own API key and turn it on again.")
                          .arg(Brand.productName)
            }

            GridLayout {
                Layout.fillWidth: true
                columns: Window.width < 520 ? 1 : 2
                columnSpacing: 16
                rowSpacing: 10
                enabled: aiEnabledCheck.checked

                Label {
                    text: qsTr("Provider")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RagoComboBox {
                    id: aiProviderCombo
                    model: AppSettings.aiProviderNames
                    Layout.preferredWidth: 200
                    onActivated: {
                        dlg._aiStash()
                        dlg._aiResetEntry()
                        dlg._aiShow(AppSettings.aiProviders[currentIndex])
                    }
                }

                // OpenAI-compatible: a preset fills the address; the address
                // stays visible and editable.
                Label {
                    visible: dlg._aiHasPresets
                    text: qsTr("Service")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RagoComboBox {
                    id: aiPresetCombo
                    visible: dlg._aiHasPresets
                    Layout.preferredWidth: 200
                    Accessible.name: qsTr("Service")
                    readonly property var presets: AppSettings.aiPresets()
                    readonly property string presetId: AppSettings.aiPresetFor(aiBaseField.text)
                    model: presets.map(p => p.name).concat([qsTr("Custom address")])
                    function presetIndex() {
                        for (var i = 0; i < presets.length; ++i)
                            if (presets[i].id === presetId)
                                return i
                        return presets.length
                    }
                    currentIndex: presetIndex()
                    onActivated: index => {
                        if (index < presets.length) {
                            aiBaseField.text = presets[index].baseUrl
                            dlg._aiBaseEdited()
                        } else {
                            aiBaseField.forceActiveFocus()
                        }
                        // A pick replaces the binding; keep following the address.
                        currentIndex = Qt.binding(presetIndex)
                    }
                }

                Label {
                    id: aiBaseLabel
                    visible: dlg._aiHasBase
                    text: qsTr("Server address")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RagoTextField {
                    id: aiBaseField
                    visible: dlg._aiHasBase
                    ltr: true
                    placeholderText: dlg._aiShownProvider === "ollama" ? "http://localhost:11434/v1" : "https://…/v1"
                    inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    Layout.preferredWidth: 200
                    Accessible.name: aiBaseLabel.text
                    onTextEdited: dlg._aiBaseEdited()
                }

                // Where the data goes, and who pays (docs: T1, T11).
                Label {
                    visible: dlg._aiHasBase
                    Layout.columnSpan: Math.min(2, parent.columns)
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: !dlg._aiBaseCheck.ok ? Theme.dangerColor
                         : dlg._aiKeyClearedForOrigin ? Theme.warningColor : Theme.disabledTextColor
                    textFormat: Text.PlainText
                    text: !dlg._aiBaseCheck.ok ? dlg._aiBaseCheck.error
                        : (dlg._aiKeyClearedForOrigin
                           ? qsTr("The address points to another server now, so the key was cleared. Enter the key for this server.") + " "
                           : "")
                          + (dlg._aiBaseCheck.scope === "device"
                             //: Preferences › AI, server on this computer (%1 = host name, e.g. localhost).
                             ? qsTr("Requests go to %1 on this device. A model that runs here is free and the data stays on this device; a cloud model or proxy behind this address sends it on.")
                             : dlg._aiBaseCheck.scope === "network"
                             //: Preferences › AI, server on the local network (%1 = host name or IP address).
                             ? qsTr("Requests go to %1 on your local network. A model that runs there is free and the data stays on your network; a cloud model or proxy behind this address sends it on.")
                             //: Preferences › AI, OpenAI-compatible (%1 = host name).
                             : qsTr("Requests go to %1. Usage is billed by that provider, if it charges."))
                                .arg(dlg._aiBaseCheck.host)
                }

                Label {
                    id: aiKeyLabel
                    visible: dlg._aiUsesKey
                    text: dlg._aiShownProvider === "gemini" ? qsTr("Google AI Studio API key")
                        : dlg._aiHasBase ? qsTr("API key (optional)")
                        : qsTr("Anthropic API key")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RagoTextField {
                    id: aiKeyField
                    visible: dlg._aiUsesKey
                    echoMode: dlg._aiKeyVisible ? TextInput.Normal : TextInput.Password
                    // Still a secret while shown: keyboards must not learn or sync it.
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    placeholderText: dlg._aiKeyPlaceholder
                    Layout.preferredWidth: 200
                    Accessible.name: aiKeyLabel.text
                    // A test result belongs to the key it was run with.
                    onTextEdited: {
                        aiCatalog.clear()
                        dlg._aiKeyFieldOrigin = dlg._aiBaseCheck.origin
                        dlg._aiKeyClearedForOrigin = false
                    }
                }

                // Key helpers: show/hide, paste (reads the clipboard only on
                // press), test (model list only, no schema or prompt), and
                // where to create a key.
                Flow {
                    Layout.columnSpan: Math.min(2, parent.columns)
                    Layout.fillWidth: true
                    spacing: 6

                    RagoPillButton {
                        compact: true
                        visible: dlg._aiUsesKey
                        text: dlg._aiKeyVisible ? qsTr("Hide key") : qsTr("Show key")
                        Accessible.role: Accessible.Button
                        Accessible.name: text
                        Accessible.onPressAction: clicked()
                        onClicked: dlg._aiKeyVisible = !dlg._aiKeyVisible
                    }
                    RagoPillButton {
                        compact: true
                        visible: dlg._aiUsesKey
                        //: Preferences › AI: paste the API key from the clipboard.
                        text: qsTr("Paste")
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Paste the API key from the clipboard")
                        Accessible.onPressAction: clicked()
                        onClicked: dlg._aiPasteKey()
                    }
                    RagoPillButton {
                        compact: true
                        enabled: !aiCatalog.busy
                        text: qsTr("Test connection")
                        Accessible.role: Accessible.Button
                        Accessible.name: text
                        Accessible.onPressAction: if (enabled) clicked()
                        ToolTip.visible: hovered
                        ToolTip.text: dlg._aiUsesKey
                                      ? qsTr("Asks the provider for its model list with this key. No query or schema is sent.")
                                      : qsTr("Asks the server for its model list. No query or schema is sent.")
                        onClicked: aiCatalog.refresh(dlg._aiShownProvider, dlg._aiUsesKey ? aiKeyField.text : "",
                                                     dlg._aiHasBase ? aiBaseField.text : "")
                    }
                    RagoPillButton {
                        compact: true
                        readonly property string pageUrl: AppSettings.aiKeyPageUrl(dlg._aiShownProvider,
                                                                                   dlg._aiHasBase ? aiBaseField.text : "")
                        visible: dlg._aiUsesKey && pageUrl.length > 0
                        text: qsTr("How to get a key ↗")
                        Accessible.role: Accessible.Link
                        Accessible.name: qsTr("Open the provider's API key page")
                        Accessible.onPressAction: clicked()
                        ToolTip.visible: hovered
                        ToolTip.text: pageUrl
                        onClicked: Qt.openUrlExternally(pageUrl)
                    }
                }

                // Paste outcome: an error, or the offer to clear the clipboard.
                RowLayout {
                    Layout.columnSpan: Math.min(2, parent.columns)
                    Layout.fillWidth: true
                    visible: dlg._aiUsesKey && (dlg._aiPasteError.length > 0 || dlg._aiPastedKey.length > 0)
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                        color: dlg._aiPasteError.length > 0 ? Theme.dangerColor : Theme.disabledTextColor
                        text: dlg._aiPasteError.length > 0
                              ? dlg._aiPasteError
                              : qsTr("Key pasted. Clear it from the clipboard? (A clipboard history may still keep a copy.)")
                    }
                    RagoPillButton {
                        compact: true
                        Layout.fillWidth: false
                        visible: dlg._aiPasteError.length === 0
                        text: qsTr("Clear clipboard")
                        Accessible.role: Accessible.Button
                        Accessible.name: text
                        Accessible.onPressAction: clicked()
                        onClicked: {
                            Backend.clearClipboardIfEquals(dlg._aiPastedKey)
                            dlg._aiPastedKey = ""
                        }
                    }
                }

                // Where the key in use comes from: the field, an environment variable, or nowhere.
                Label {
                    visible: dlg._aiUsesKey
                    Layout.columnSpan: Math.min(2, parent.columns)
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.disabledTextColor
                    readonly property string envName: AppSettings.aiKeyEnvironmentVariable(
                                                          dlg._aiShownProvider, dlg._aiHasBase ? aiBaseField.text : "")
                    text: aiKeyField.text.trim().length > 0
                          ? qsTr("Using the key entered here (saved with your settings).")
                          : (envName.length > 0
                             ? qsTr("Using the key from the environment variable %1.").arg(envName)
                             : dlg._aiHasBase
                               ? qsTr("No key: requests are sent without one (enough for a local server).")
                               : qsTr("No key yet. Enter one above."))
                }

                Label {
                    text: qsTr("Model")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RagoComboBox {
                    id: aiModelCombo
                    visible: !dlg._aiHasBase
                    model: dlg._aiModelList
                    Layout.preferredWidth: 200
                    Accessible.name: qsTr("Model")
                    onActivated: dlg._aiSelectionRetired = false
                }
                ColumnLayout {
                    visible: dlg._aiHasBase
                    spacing: 6
                    Layout.preferredWidth: 200
                    RagoTextField {
                        id: aiModelField
                        ltr: true
                        Layout.fillWidth: true
                        placeholderText: dlg._aiShownProvider === "ollama" ? qsTr("Installed model, e.g. llama3.2")
                                                                            : qsTr("Model name, e.g. gpt-5.5")
                        Accessible.name: qsTr("Model")
                        onTextEdited: dlg._aiSelectionRetired = false
                    }
                    RagoComboBox {
                        visible: dlg._aiModelList.length > 0
                        Layout.fillWidth: true
                        model: dlg._aiModelList
                        displayText: qsTr("Pick from the server's list…")
                        Accessible.name: qsTr("Models listed by the server")
                        onActivated: index => {
                            aiModelField.text = dlg._aiModelList[index]
                            dlg._aiSelectionRetired = false
                        }
                    }
                }

                // Test connection outcome, then the model list state.
                Label {
                    Layout.columnSpan: Math.min(2, parent.columns)
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: aiCatalog.error.length > 0 ? Theme.dangerColor
                         : aiCatalog.result.length > 0 ? Theme.accentColor
                         : dlg._aiSelectionRetired ? Theme.warningColor : Theme.disabledTextColor
                    textFormat: Text.PlainText   // server error text
                    text: aiCatalog.busy ? qsTr("Testing the connection…")
                        : aiCatalog.error.length > 0 ? aiCatalog.error
                        : aiCatalog.result.length > 0 ? aiCatalog.result
                        : dlg._aiSelectionRetired
                          ? qsTr("The selected model is no longer in the provider's list (retired?). Pick another one.")
                        : dlg._aiFetched ? qsTr("Models from the provider's current list.")
                        : dlg._aiHasBase ? qsTr("Type the model name, or use Test connection to list the server's models.")
                        : qsTr("Built-in list. Test connection loads the models this key can use now.")
                }

                // Local models can be slow, mostly while they load (docs: B7).
                Label {
                    id: aiTimeoutLabel
                    visible: dlg._aiHasBase
                    text: qsTr("Answer time limit (sec)")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RagoSpinBox {
                    id: aiTimeoutSpin
                    visible: dlg._aiHasBase
                    from: AppSettings.aiMinTimeout()
                    to: AppSettings.aiMaxTimeout()
                    Layout.preferredWidth: 200
                    Accessible.name: aiTimeoutLabel.text
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("How long to wait for an answer. A local model can need minutes, especially while it loads.")
                }
            }

            AiModelCatalog {
                id: aiCatalog
                onRefreshed: (provider, models) => {
                    if (provider !== dlg._aiShownProvider)
                        return
                    dlg._aiStash()
                    dlg._aiShow(provider)
                }
            }

            RagoCheckBox {
                id: aiFallbackCheck
                Layout.fillWidth: true
                enabled: aiEnabledCheck.checked
                text: qsTr("When a model runs out of quota, hits a rate limit or is retired, try another model of the same provider")
            }

            RagoCheckBox {
                id: aiValuesCheck
                Layout.fillWidth: true
                enabled: aiEnabledCheck.checked
                text: qsTr("Also send frequent values and min/max ranges (better filters, but sends document data)")
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.glassBorder
            }

            PreferencesSchemaSection {
                id: schemaSection
                Layout.fillWidth: true
            }

            Rectangle {
                visible: McpSettings.available
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.glassBorder
            }

            PreferencesMcpSection {
                id: mcpSection
                Layout.fillWidth: true
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.glassBorder
            }

            PreferencesAccountSection {
                id: accountSection
                Layout.fillWidth: true
            }

            Rectangle {
                visible: !Brand.isStoreBuild
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.glassBorder
            }

            Label {
                visible: !Brand.isStoreBuild
                text: qsTr("UPDATES & USAGE")
                font.bold: true
                font.pixelSize: 11
                font.letterSpacing: Language.rtl ? 0 : 1.1
                color: Theme.accentColor
            }

            Label {
                visible: !Brand.isStoreBuild
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.disabledTextColor
                text: qsTr("Checks for new %1 releases when the Welcome page opens and an internet connection is available. Anonymous usage statistics include only app version, platform, and aggregate feature counts — never hostnames, database or collection names, credentials, or queries.").arg(Brand.productName)
            }

            ColumnLayout {
                visible: !Brand.isStoreBuild
                Layout.fillWidth: true
                spacing: 8

                RagoCheckBox {
                    id: updateCheck
                    Layout.fillWidth: true
                    text: qsTr("Automatically check for updates on startup")
                }

                RagoCheckBox {
                    id: usageStatsCheck
                    Layout.fillWidth: true
                    enabled: updateCheck.checked
                    text: qsTr("Share anonymous usage statistics with update checks")
                }
            }
        }
    }
}
