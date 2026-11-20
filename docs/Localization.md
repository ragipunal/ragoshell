# RagoShell — Localization (i18n)

Status: implemented. The UI is available in 15 languages, switches live, and
Arabic is laid out right to left. The translations are machine drafts.

## What the user gets

- Preferences › Language lists "System default (<language>)" and the 15
  languages by endonym, with the English name as a second line so a user stuck
  in an unreadable language can find English. A change applies at once as a
  preview; Cancel restores the previous language and the other combos (a
  ComboBox with a translated model resets to its first entry on a switch, so
  Preferences saves and restores them).
- The setting is `uiLanguage` (default `"system"`): the first entry of
  `QLocale::system().uiLanguages()` that matches a language. A script subtag must
  match the translation's script, so `az-Arab` and `zh-Hant` match neither `az`
  nor `zh_CN`. No match gives English.
- Data is never changed by the UI language: result cells, JSON, exports and the
  clipboard use the raw form (no digit grouping, `.` as decimal separator, Latin
  digits). Only UI chrome uses the locale (counts in
  `ExplainView.qml`, an analysis time in a tooltip). `main.cpp` keeps
  `setlocale(LC_NUMERIC, "C")`, so C-level number parsing ignores the UI language.

## What is not translated

- MongoDB itself (operators, shell commands, JSON keys, server error messages,
  anything typed in the editor), product and protocol names (`TLS`, `SSH`), and
  the MCP tool texts (read by LLM clients, `docs/McpServer.md`).
- Technical log lines (`LogModel`), so bug reports and searches work across
  languages; a message that reaches both is translated only at the UI edge.
- AI assistant answers follow the language of the request: the system prompt in
  `src/ragoshell/core/utils/AiQueryAssistant.cpp` asks for the explanation "in
  the language of the request". The UI language is not sent.

## Languages

English is the source language. The one table that defines a language is
`src/ragoshell/core/utils/LanguageCatalog.cpp` (`LanguageCatalog_test.cpp`).

| Code (`.ts` suffix) | Endonym | Locale | Script | Direction | Qt ships its own catalogs |
|---|---|---|---|---|---|
| `en` | English | `en` | Latn | LTR | n/a |
| `hi` | हिन्दी | `hi_IN` | Deva | LTR | no |
| `zh_CN` | 简体中文 | `zh_CN` | Hans | LTR | yes |
| `es` | Español | `es` | Latn | LTR | yes |
| `pt_BR` | Português (Brasil) | `pt_BR` | Latn | LTR | yes |
| `ru` | Русский | `ru` | Cyrl | LTR | yes |
| `de` | Deutsch | `de` | Latn | LTR | yes |
| `fr` | Français | `fr` | Latn | LTR | yes |
| `ja` | 日本語 | `ja` | Jpan | LTR | yes |
| `ar` | العربية | `ar_AE` | Arab | RTL | yes |
| `tr` | Türkçe | `tr` | Latn | LTR | yes |
| `az` | Azərbaycan dili | `az_Latn_AZ` | Latn | LTR | no |
| `kk` | Қазақ тілі | `kk` | Cyrl | LTR | yes |
| `ky` | Кыргызча | `ky` | Cyrl | LTR | no |
| `uz` | Oʻzbekcha | `uz_Latn_UZ` | Latn | LTR | no |

Decisions:

- The ten most-spoken languages that fit a technical audience (German and
  Japanese ahead of Bengali, Indonesian and Urdu for their weight in the
  developer market), plus the five major Turkic languages; Portuguese is the
  Brazilian variant. No Turkic language of the list is right to left in its
  official form, so Arabic is the only RTL language.
- The locale carries the script (`az_Latn_AZ`, `uz_Latn_UZ`), or formats could come
  from a Cyrillic or Arabic variant. Arabic uses `ar_AE` because `ar` defaults to
  Arabic-Indic digits, wrong for a database tool: UI numbers are Latin everywhere.
- Files are named by language, so a script variant (`kk_Latn`, `uz_Cyrl`) is a
  new row and a new file. "Qt ships" (`qtTranslated`) was checked against Qt 6.12.

## How it is built

- `src/ragoshell-app/CMakeLists.txt` (section "UI translations") sets
  `RAGO_UI_LANGUAGES`, `QT_I18N_SOURCE_LANGUAGE en` and calls
  `qt_add_translations` with `TS_FILE_DIR src/ragoshell-app/i18n`,
  `TS_FILE_BASE ${RAGO_PRODUCT_ID}`, `RESOURCE_PREFIX /i18n`, `SOURCE_TARGETS ragoshell
  rago-core` (so core `tr()` strings are extracted) and `LUPDATE_OPTIONS
  -no-obsolete -locations none` (a refactor does not churn every file).
- The `.ts` files are committed; the `.qm` files are embedded under `:/i18n/`.
  `ragoshell_en.ts` holds only the plural messages.
- Qt's own `qtbase_<code>.qm` and `qtdeclarative_<code>.qm` are copied from the Qt
  installation at configure time for the languages Qt ships and embedded under
  `:/i18n/qt/` (`windeployqt` runs with `--no-translations`, so every platform
  uses the embedded copy). `hi`, `az`, `ky` and `uz` have none and need none: the
  app has its own buttons and menus, and `FileDialog` is native on Windows and
  macOS; only the Qt Quick fallback dialog (Linux) stays English for them.
- After changing UI text run the `update_translations` target (`cmake --build <dir>
  --target update_translations`), then `python ci/fill-english-plurals.py`.

## Runtime: the `Language` singleton

`LanguageManager` (`src/ragoshell-app/LanguageManager.{h,cpp}`) is the QML
singleton `Language` (`current`, `effective`, `rtl`, `isolate()`, ...); the table
and the OS-language matching are in `LanguageCatalog`. It is created in
`main.cpp` before the QML engine loads, so nothing builds a string early.

- A switch removes the old translators, installs Qt's two catalogs and
  `ragoshell_<code>.qm` last (so ours can override Qt), sets
  `QLocale::setDefault()` and the layout direction, applies the fonts, emits
  `languageAboutToChange`, calls `engine.retranslate()`, emits `languageChanged`.
  Missing catalogs are skipped quietly.
- C++ that caches translated text is rebuilt: `QmlBackend::retranslate` resets the
  status line and calls `ServerTreeModel::retranslate` and
  `TabModel::retranslate`; the tray menu follows `languageChanged`.
- Fonts: Inter has no CJK, Devanagari or Arabic glyphs, so `applyFonts` sets a
  per-language family list on the application font (`QFont::setFamilies`; Qt
  substitutions work per missing family, not per glyph). Japanese and Chinese
  share code points but draw them differently, so each lists its own font first
  (`fallbackFamilies()`, per OS). The code font is unchanged. Linux needs
  `fonts-noto-cjk` and `fonts-noto-core` (`docs/BuildRagoShellOnMacAndLinux.md`).

## Writing translatable strings

- QML uses `qsTr()`, C++ uses `tr()`; rago-core classes that are not `QObject`s use
  `QCoreApplication::translate("<Class>", ...)` or `QT_TRANSLATE_NOOP`.
- Never build a sentence by gluing text onto `qsTr()`: use one string with `%1`
  (numbered when there are several values, so translators can reorder) and
  `.arg()`. Status glyphs (`✓ ✕`) go inside the string, so RTL languages can
  place them.
- Counts with a noun use plurals: `qsTr("%n document(s)", "", count)`. The `.ts`
  files have 6 forms for Arabic, 3 for Russian, 1 for Japanese, Chinese and
  Turkish, 2 for the rest. `ci/fill-english-plurals.py` fills the English forms.
- Ambiguous words ("Index", "Drop", "Run", "Find") and strings with MongoDB
  keywords get a `//:` comment for the translator (47 in `qml/`).
- A database, collection, host, path or field name inside a sentence goes through
  `Language.isolate()` (QML) or `LanguageCatalog::isolate()` (C++): Unicode
  isolates U+2068 ... U+2069 keep the name's direction in an Arabic sentence.
- Identifiers, keywords, field and collection names and operators are never
  case-converted with a locale: `QString::toUpper()` and JS `toUpperCase()`,
  never `toLocaleUpperCase()`, or in Turkish and Azerbaijani `"index"` becomes
  `"İNDEX"`. `LanguageCatalog_test.cpp` checks this under the Turkish locale.

### Shortcut text

Key names are built by `Platform` (`src/ragoshell-app/PlatformInfo.{h,cpp}`), so one
translation serves every platform.

- A translated string that contains a key uses `%1` and
  `Platform.withShortcut(qsTr("Close Log (%1)"), "Ctrl+L")`. Where shortcuts are
  shown, `%1` becomes the key as the platform writes it (`Platform.keyLabel`:
  Command on Mac and iPad). Where they are not, the placeholder and the round,
  full-width or square brackets around it are removed: "Close Log".
- Shortcuts show on desktop, and on phones and tablets only while a hardware
  keyboard is detected (`Platform.shortcutsVisible`).
- A sentence with keys inside it keeps them as text and passes through
  `Platform.macKeys()`, which turns `Ctrl+` into the Command symbol on Mac and
  iPad, so every translation must keep the literal `Ctrl+`. Such a text has a
  second, key-free variant for touch devices, picked with
  `Platform.shortcutsVisible` (see the getting-started texts in
  `src/ragoshell-app/qml/WelcomePage.qml`).

## Glossary, workflow and checks

`src/ragoshell-app/i18n/GLOSSARY.md` holds the agreed word per term and language
and the rules for every translator, human or machine: placeholders and leading
or trailing symbols stay, MongoDB keywords are never translated, numbers use
Latin digits. Terms that are also commands (`find`, `aggregate`) stay English.
Translate with Qt Linguist, or in bulk with `ci/ts-exchange.py` (`.ts` to JSON
and back: `export`, `import`, `check`, `forms`). Unfinished strings show English.

```bash
node ci/check-i18n.mjs
```

This is the consistency check CI runs. It needs only Node and exits with 1 when a
sentence is glued onto `qsTr()` in `src/ragoshell-app/qml` or a translation loses a
placeholder of its source (a plural form may omit `%n`). It also prints finished
/ total strings per language; missing translations are not an error. The build
scripts `ci/build-windows.ps1`, `ci/build-linux.sh` and `ci/build-macos.sh` first
build `update_translations` and fail when `git diff` shows changes in
`src/ragoshell-app/i18n`, then run the check; the Windows script also loads the QML
with `--check-qml --lang ar` and `--lang ja`.

The layout check in `docs/UiMatrix.md` (`dev/ui-shots/geometry.sh`) runs every
screen at four sizes in `en`, `tr`, `de`, `ar` and `ja` (`--lang`) and lists every
item that sticks out of its window or dialog (the left edge for Arabic) and every
button whose text had to be cut. Fix cut text with `Layout.fillWidth`,
`elide: Text.ElideRight` and a `ToolTip`, not with fixed widths.

## Right to left (Arabic)

- `Main.qml` sets `LayoutMirroring.enabled` from `Qt.application.layoutDirection`
  with `childrenInherit`; dialogs and menus sit in the window's overlay and
  inherit it. `SplitView` does not mirror, so `Main._placeExplorer` moves the
  explorer behind the work area.
- Left-to-right islands: the code editor (`RagoCodeEditor.qml`), the result views
  (tree, table, JSON) in `ShellTabPage.qml`, the startup script field, and every `RagoTextField { ltr: true }` (host, port, path, URI,
  JSON, identifiers). The result tree is LTR too, because column headers in a
  different order than the data columns are worse; Arabic text in a cell still
  renders correctly (Qt's bidi handling).
- `RagoComboBox`, `RagoCheckBox`, `RagoSpinBox` and the explorer tree delegate
  place their indicators by `mirrored`. Direction glyphs (chevrons, tab arrows)
  switch on `Language.rtl` inline; run, refresh, check and sort glyphs are not
  mirrored. Headings set `font.letterSpacing` to 0 when `Language.rtl` (it breaks
  Arabic letter joining).

## Packaging and OS metadata

- macOS and iOS: `CFBundleLocalizations` in `install/macosx/Info.plist.in` and
  `install/ios/Info.plist.in` lists the 15 languages.
- NSIS (`install/windows/ragoshell.nsi`): English first, then every other
  language NSIS has (a missing language file is skipped); Azerbaijani, Kazakh and
  Kyrgyz get English. WiX is English only.
- Linux: `install/linux/ragoshell.desktop` has `Comment[<lang>]` for the 14
  translated languages, no `Name[<lang>]`.
- Adding a language: a row in `LanguageCatalog.cpp`, its code in
  `RAGO_UI_LANGUAGES` (CMake then creates the empty `.ts` file), a glossary
  column (`README.md` › Translating RagoShell).

## Development helpers

- `--lang <code>`: start in a language for this run only (nothing is stored).
  `--switch-lang <code>`: switch 2.5 s after start, like the picker.
- `--check-qml --lang ar` loads the QML in a language and exits; `--ime-selftest
  --lang ja` checks input methods; `--open-view prefs --snapshot out.png 3500
  --exit-after-snapshot` takes a picture; `RAGOSHELL_CONFIG_DIR=/tmp/ragotest` is
  a throw-away settings folder.
- Pseudo-locale (accented, about 40 % longer, in `[` `]`): plain English left over
  is not translatable, a cut-off `]` is a layout that will break. Build it with
  `ci/pseudo-translate.py <lang>.ts pseudo.ts` and `lrelease`, load it with
  `--qm-file pseudo.qm`.

## Translation status

All 14 translated languages are complete in `src/ragoshell-app/i18n/` (1152
messages each) and pass `ci/check-i18n.mjs`, apart from the strings under "Open
items". They are machine drafts: made from the glossary, checked mechanically and
looked at in screenshots, but not reviewed by a native speaker who knows MongoDB.
Until a reviewer signs off, a language is a preview. The new strings of
2026-10-29 were translated by an AI; the Kyrgyz, Kazakh and Uzbek ones in
particular want a native speaker, as there is little established MongoDB
terminology in those languages.

## Open items

- Native review of every language, the 2026-10-29 strings of `ky`, `kk`, `uz` first.
- One rule for "Explain": English in `fr`, `ja`, `ru`, translated in `tr`, `de`, `es`, `ar`.
- Right-to-left review of the server monitor charts (the canvas draws left to
  right; whether time should flow that way in Arabic is undecided) and of the
  pipeline builder card order.
- Unticked items of `docs/ManualTests.md` § 25 (persistence, Cancel restore,
  pseudo-locale pass, Arabic mirrored comparison, IME input, Linux fonts). macOS
  fonts are untested.
