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
    property string _aiShownProvider: ""
    property var _aiModelList: []
    property bool _aiFetched: false
    property bool _aiSelectionRetired: false
    property string _aiKeyPlaceholder: ""

    function _aiStash() {
        if (_aiShownProvider.length === 0)
            return
        _aiKeys[_aiShownProvider] = aiKeyField.text
        if (aiModelCombo.currentIndex >= 0)
            _aiModelSel[_aiShownProvider] = aiModelCombo.currentText
    }
    function _aiShow(provider) {
        _aiShownProvider = provider
        var models = AppSettings.aiModelsFor(provider).slice()
        var sel = _aiModelSel[provider] || models[0]
        _aiFetched = AppSettings.aiModelsFetched(provider)
        // A saved model the provider no longer lists stays selectable, flagged.
        _aiSelectionRetired = _aiFetched && models.indexOf(sel) < 0
        if (models.indexOf(sel) < 0)
            models.unshift(sel)
        _aiModelList = models
        aiModelCombo.currentIndex = Math.max(0, models.indexOf(sel))
        aiKeyField.text = _aiKeys[provider] || ""
        var env = AppSettings.aiKeyEnvironmentVariable(provider)
        _aiKeyPlaceholder = env.length > 0 ? qsTr("%1 is used").arg(env)
                                           : (provider === "gemini" ? "AIza…" : "sk-ant-…")
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
        var keys = {}, models = {}
        for (var i = 0; i < AppSettings.aiProviders.length; ++i) {
            var p = AppSettings.aiProviders[i]
            keys[p] = AppSettings.aiApiKeyFor(p)
            models[p] = AppSettings.aiModelFor(p)
        }
        _aiKeys = keys
        _aiModelSel = models
        _aiShownProvider = ""
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
            AppSettings.setAiApiKeyFor(p, _aiKeys[p] || "")
            if (_aiModelSel[p])
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
                           + "selected provider: Anthropic's Claude API or Google's Gemini API (an "
                           + "API key from Google AI Studio). Nothing is sent until you press Generate.")
            }

            RagoCheckBox {
                id: aiEnabledCheck
                Layout.fillWidth: true
                text: qsTr("Enable the AI query assistant")
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
                        dlg._aiShow(AppSettings.aiProviders[currentIndex])
                    }
                }

                Label {
                    visible: dlg._aiShownProvider !== "ragoshell_cloud"
                    text: dlg._aiShownProvider === "gemini" ? qsTr("Google AI Studio API key")
                                                            : qsTr("Anthropic API key")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RagoTextField {
                    id: aiKeyField
                    visible: dlg._aiShownProvider !== "ragoshell_cloud"
                    echoMode: TextInput.Password
                    placeholderText: dlg._aiKeyPlaceholder
                    Layout.preferredWidth: 200
                }

                // Where the key in use comes from: the field, an environment variable, or nowhere.
                Label {
                    Layout.columnSpan: Math.min(2, parent.columns)
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.disabledTextColor
                    readonly property string envName: AppSettings.aiKeyEnvironmentVariable(dlg._aiShownProvider)
                    text: dlg._aiShownProvider === "ragoshell_cloud"
                          ? (CloudService.signedIn
                             ? qsTr("Signed in — requests run through your RagoShell account quota; no API key needed.")
                             : qsTr("Sign in on the Account tab to use RagoShell AI, or pick another provider and enter its key."))
                          : (aiKeyField.text.trim().length > 0
                             ? qsTr("Using the key entered here (saved with your settings).")
                             : (envName.length > 0
                                ? qsTr("Using the key from the environment variable %1.").arg(envName)
                                : qsTr("No key yet. Enter one above.")))
                }

                Label {
                    visible: dlg._aiShownProvider !== "ragoshell_cloud"
                    text: qsTr("Model")
                    font.pixelSize: 13
                    color: Theme.textColor
                    Layout.fillWidth: true
                }
                RowLayout {
                    visible: dlg._aiShownProvider !== "ragoshell_cloud"
                    spacing: 6
                    Layout.preferredWidth: 200
                    RagoComboBox {
                        id: aiModelCombo
                        model: dlg._aiModelList
                        Layout.fillWidth: true
                        onActivated: dlg._aiSelectionRetired = false
                    }
                    // Providers add and retire models; fetch what this key can use.
                    RagoPillButton {
                        text: "⟳"
                        enabled: !aiCatalog.busy
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Load the provider's current model list")
                        onClicked: aiCatalog.refresh(dlg._aiShownProvider, aiKeyField.text)
                    }
                }

                Label {
                    visible: dlg._aiShownProvider !== "ragoshell_cloud"
                    Layout.columnSpan: Math.min(2, parent.columns)
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: aiCatalog.error.length > 0 ? Theme.dangerColor
                         : dlg._aiSelectionRetired ? Theme.warningColor : Theme.disabledTextColor
                    text: aiCatalog.busy ? qsTr("Loading the model list…")
                        : aiCatalog.error.length > 0 ? aiCatalog.error
                        : dlg._aiSelectionRetired
                          ? qsTr("The selected model is no longer in the provider's list (retired?). Pick another one.")
                        : dlg._aiFetched ? qsTr("Models from the provider's current list.")
                        : qsTr("Built-in list. ⟳ loads the models this key can use now.")
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
