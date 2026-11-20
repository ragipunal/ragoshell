# RagoShell UI: design system and architecture

How the interface is built, what its rules are and why. Current state; the history is in git. Where a topic has its
own document it is linked: `docs/UiMatrix.md` (how every screen was checked), `docs/MacOsWork.md` (what needs a Mac),
`docs/Platforms.md` (the platforms), `docs/Shell.md` (the embedded shell), `docs/Localization.md` (translations).

## 1. Architecture

- **`rago-core`** (`src/ragoshell`, a static library) holds everything that is not UI: the MongoDB client and
  workers, the embedded shell, settings, domain objects, events, SSH tunnelling, the MCP server's backend. It has no
  GUI dependency; `rago_unit_tests` and `ragoshell-mcp` link only this.
- **`ragoshell`** (`src/ragoshell-app`) is the only frontend (the Qt Widgets app was removed; the last commit
  that has it is tagged `widgets-final`). C++ side: `QmlBackend` (the one object QML talks to), controllers
  (`ShellController`, `ServerMonitorController`), list models (`ConnectionListModel`, `ServerTreeModel`, `TabModel`,
  `QueryHistoryModel`, `ScriptDraftModel`, `LogModel`), and singletons registered with `QML_NAMED_ELEMENT` +
  `QmlSingletonInstance<T>` from `main()` (`Theme`, `Platform`, `Backend`, `AppSettings`, `Language`, the connection, history and
  draft models, the UI services) so that qmlsc can compile the bindings that use them. QML side: `src/ragoshell-app/qml/*.qml`, module `RagoShell`.
- **Style:** Qt Quick Controls `Basic`, with the app's own `Rago*` components on top (section 3). No native
  look, no WebEngine.
- **Editor:** QScintilla rendered into a `QQuickPaintedItem` (`ScintillaQuick`, `RagoCodeEditor.qml`). Chosen over a
  WebView editor (would bring back Chromium) and over a plain `TextArea` (no autocomplete or folding). The vendored
  platform layer needs a `QPaintDevice`, so the app is a `QApplication` that links `Qt6::Widgets`; no widget is ever
  shown. Popups (completion list, call tips, context menu) are QML overlays fed by the editor's notifications.
- **Dialogs** open lazily (`LazyDialog.qml`): several embed Scintilla editors, each backed by a hidden `QWidget`, so a
  dialog is created on its first use, not at startup.

## 2. Colour, type, icons

Two palettes in `ThemeBridge.cpp` (`Theme.dark` and the colour properties), chosen by the setting "System / Light /
Dark" (System follows the OS live):

| Token | Dark | Light |
|---|---|---|
| window / base / surface | `#090d0b` / `#0f1612` / `#0c130f` | `#f3f6f4` / `#ffffff` / `#f8faf9` |
| text / disabled text | `#eef4f0` / `#677b71` | `#14201a` / `#566b60` |
| accent (the brand green) / on accent | `#00be54` / `#05140e` | `#00a344` / `#ffffff` |
| border / hover / selected | `#1f2f25` / `#1c2b22` / `#1c3c2f` | `#d2ded7` / `#dce7e1` / `#c5ebd8` |
| online / offline | `#00e668` / `#ff5c5c` | the same |
| danger / warning | `#ff5c5c` / `#fbbf24` | `#dc2626` / `#b45309` |

Brand colours (the icon's gradients): `#0c2319` to `#05140e` (squircle), petals `#028639`, `#00be54`, `#34f28b`,
glow `#7affb8`.

Type: the platform's UI font for text; a code font for the editor and the data views (the setting "Code & data font"). **Icons in the UI are Unicode symbols** (check marks, arrows, dingbats), not an icon
font: phones and tablets do not carry fonts with all of them (Android 10 showed empty boxes), so the mobile builds
embed DejaVu Sans (`assets/fonts`) as a fallback. Emoji are not in it: do not use them as button glyphs (the Find button
once showed empty).

### The application icon

One artwork, `assets/icon/ragoshell-icon.svg` (dark green squircle, five-petal flower). `dev/icons/make-icons.sh`
makes every file of every platform from it (the emblem layer, the background, the Android monochrome layer and the
iOS tinted icon are derived from the SVG, not drawn again):

| Platform | Files |
|---|---|
| Windows | `install/windows/ragoshell.ico`, `icon-set/*.png`, embedded by `winres.rc.in` |
| macOS | `install/macosx/ragoshell.iconset`, `ragoshell.icns` (Big Sur grid: art 824 of 1024 px, shadow); `dev/icons/png2icns.mjs` writes the `.icns` without `iconutil` |
| Linux | `install/linux/icons/hicolor/{16..512}/apps/ragoshell.png`, `scalable/apps/ragoshell.svg`, installed by CMake with `ragoshell.desktop` |
| Android | `install/android/res/mipmap-*` (legacy, round, adaptive background / foreground / monochrome), `play-store-icon-512.png` |
| iOS / iPadOS | `install/ios/Assets.xcassets/AppIcon.appiconset` (1024 px light, dark, tinted); the catalog is in the target's "Copy Bundle Resources" phase |
| In the app | `src/ragoshell-app/resources/app_icon.png` (window, tray, About, top bar), `welcome_splash.png` |

`assets/icon/ragoshell-wordmark.svg`, `ragoshell-icons-sheet.svg`, `ragoshell-welcome-*.svg` are the brand
artwork the splash and banner were made from. The SVG is the source: edit it, run the script, commit the result.
iOS keeps the icon of an earlier install of the same bundle id and version: after a change, delete the app once.

## 3. Components

`Rago*` components wrap the Controls so that every screen looks and sizes the same: `RagoDialog` (title, close,
accept / reject footer, width `min(parent - 32, N)`), `RagoPillButton`, `RagoIconButton`, `RagoTextField`,
`RagoComboBox`, `RagoSpinBox`, `RagoCheckBox`, `RagoMenu` / `RagoMenuItem` (`keys:` takes a portable key sequence) /
`RagoMenuSeparator`, `RagoBadge`, `RagoStatusDot`, `RagoGlassCard`, `RagoCodeEditor`; plus `HScrollRow` (a toolbar
row that scrolls), `StripButton` and `CommandMenuItem` (registry-driven), `FocusReveal` (keeps the focused field
above the on-screen keyboard), `LazyDialog`, `VisibleArea` (the part of the window the keyboard leaves free).
New screens use them and take their sizes from `Theme`; no screen sets its own control height.

## 4. Density and sizes

`UiDensity::sizes(touch, tablet)` (`src/ragoshell/core/utils/UiDensity.h`, unit tested) is the one table of control
sizes in dp; `ThemeBridge` exposes it (`Theme.controlHeight`, `fieldHeight`, `iconButtonSize`, `menuItemHeight`,
`splitHandle`, `Theme.touch`):

| Column | control | field | icon button | menu item | split handle |
|---|---|---|---|---|---|
| Desktop (compact) | 32 | 34 | 28 | 30 | 4 |
| Tablet (touch, shortest side 600 dp or more) | 34 | 35 | 32 | 36 | 6 |
| Phone (touch below 600 dp; also "Touch" chosen on a desktop) | 36 | 36 | 34 | 38 | 8 |

Minimalist touch controls separate outer finger hit boxes (`32–36 dp`, reaching `>= 44 dp` touch pitch with row/item spacing) from sleek inner visual chrome (`27–32 dp` soft-rounded pills and borderless ghost icon buttons at rest).

The setting "Interface density" is Automatic (touch on a phone or tablet OS), Compact or Touch.
`PlatformInfo::detectFormFactor()` decides phone or tablet from the screen, not the window (a split window of a
tablet is still a tablet).

## 5. Platform model and shortcuts

`Platform` (`PlatformInfo`) is the one place that knows the OS (`windows|macos|linux|android|ios`), the form factor,
whether a hardware keyboard is attached, and how a key is written. No QML file reads `Qt.platform.os` or writes
"Ctrl+" itself.

- **A shortcut is shown only where it works:** always on a desktop; on a phone or tablet only while a physical
  hardware keyboard is attached. Android queries `Configuration.keyboard` and `hardKeyboardHidden` through JNI;
  iOS/iPadOS observes `GCKeyboard` connect and disconnect notifications from the GameController framework. Host
  keyboard bridges in iOS Simulator and Android Emulator (and synthesized soft-keyboard clipboard chords) are
  ignored so mobile screens stay free of shortcut hints unless a real physical keyboard is connected (or
  `RAGOSHELL_SIM_HW_KEYBOARD=1` is set during simulator testing).
- **Spelling:** a portable sequence ("Ctrl+Shift+S") is written `Ctrl+Shift+S` on Windows, Linux and Android and
  `⇧⌘S` on macOS and iPadOS (`keyLabel`, `KeyLabel.cpp`, unit tested). The Command key is the Control key of the code.
- **In text:** `Platform.withShortcut(qsTr("Close Log (%1)"), "Ctrl+L")` puts the key in, or removes the placeholder
  with its brackets, so one translated string serves both. Sentences with keys inside them have a second key-free
  text for touch (`WelcomePage.qml`), and `Platform.macKeys()` writes ⌘ in running text on a Mac. In menus,
  `RagoMenuItem { keys: "Ctrl+Z" }`. When the keyboard state changes the engine retranslates.
- **Commands that do not exist on a platform** are not offered: Quit and the shortcut reference are hidden on a
  touch device without a keyboard (`desktopOnly`, `keyboardOnly` in the registry).

## 6. Navigation

One top bar and one command palette replace the old menu bar, the same on every platform.

- **App bar** (`AppBar.qml`, one row, `Theme.fieldHeight + 12` on touch, 52 on desktop): explorer toggle, logo (and
  name from 900 px), the search field that opens the palette (an icon below 720 px), New Connection, Monitor (in the
  menu below 720 px), a theme menu, and a "more" menu (Log Panel, Preferences, Keyboard Shortcuts, Open Log
  Folder, About).
- **Command registry** (`AppActions.qml`): 37 commands in five sections (File 7, Shell 14, View 8, Tools 5, Help 3)
  with title, key text, enabled state and handler. The app bar menus, the palette and the shell action strip read it.
  The keys themselves are bound by the `Shortcut` items of `Main.qml`; the registry only displays them, so no key is
  bound twice. It checks itself at start (a duplicate id, unknown section, missing handler or title makes
  `--check-qml` fail).
- **Command palette** (`CommandPalette.qml`): Ctrl+K / ⌘K / Ctrl+Shift+P. An empty query shows the last 8 commands
  and the saved connections; typing filters commands, connections and the databases and collections the explorer has
  loaded, accent- and case-insensitive, word starts first ("nsh" finds New Shell Tab). A dimmed command says why
  ("Needs a shell tab"). Enter runs a command, connects a connection, or opens a shell on a database or collection.
  The popup height follows the space the on-screen keyboard leaves.
- **Shell action strip** (the context strip of a shell tab): badges for server, version, database, collection, then
  Format, History, Snippets, Export, Query Bar, Find and a menu. It adapts to the width of the *tab*: labels from 900
  dp, icons from 620 dp, less moves Format / History / Snippets / Export into the menu (thresholds times 1.45 on
  touch); the version badge hides below 520 dp.

## 7. Layout rules (what the checks taught)

Each of these cost a screen that broke on a phone, in a long translation or in a split window; the layout guard
(section 8) finds the failures.

1. **A Qt layout does not shrink an item that does not fill.** A row of buttons, a check box, a badge row keeps its
   natural width and pushes the page past the screen. `RagoPillButton` and `RagoCheckBox` therefore fill with a
   maximum of their natural width and a small minimum (they give way, never grow; `stretch: true` makes a pill
   grow). An elided fill `Label` gets `Layout.minimumWidth: 0`. Rows of template buttons and badges are a `Flow`.
2. **Toolbars scroll instead of squeezing:** `HScrollRow` keeps the natural widths and scrolls sideways; a button
   with cut text ("Tr...") is a defect (the guard reports `cut=`).
3. **A `StackLayout` is as wide as its widest hidden page:** give it `Layout.minimumWidth: 0` in dialogs.
4. **Forms:** two columns (`GridLayout`) become one column, label above field, below 520 dp
   (`columns: Window.width < 520 ? 1 : 2`); a `Layout.columnSpan: 2` is `Math.min(2, parent.columns)`.
5. **Decide by the page, not the window:** a tablet in portrait with the Explorer open leaves a work area of
   440 dp. Pages that adapt (Server Monitor, the shell tree) test their own width.
6. **No fixed heights around controls.** A bar is as tall as its content (`implicitHeight` of the row plus margins);
   a `Layout.preferredHeight: 42` around a 44 dp button made the Test Connection button overflow. Small toolbar pills
   keep a touch minimum (`Theme.controlHeight - 8`).
7. **Dialog width** is `Math.min(parent.width - 32, N)`; its height `Math.min(visibleHeight - 40, N)`.
8. **Right to left:** layouts mirror (`LayoutMirroring`); text and symbols that must not (code, hosts, ports) are
   `ltr: true`. A `TabBar` (a `ListView`) scrolls its current tab into view by hand: Qt's range-following left only
   the current tab visible in Arabic.

QML traps met on the way: `palette` is a property of `Popup`/`Control` (do not name an id that); `availableHeight` is
final on `Popup`; on Android Qt reports the keyboard rectangle in device pixels (divide by the pixel ratio); the
Enter key of the Android keyboard needs `EnterKey.type` and `onAccepted`; setting `Layout.*` on an item outside a
layout is silently ignored.

## 8. Checking a change

- **Layout guard (no device):** `dev/ui-shots/geometry.sh -s 360x800 -d Touch -l ar` opens each view in the desktop
  build at a size, density and language and lists items beyond the edge of the window or of their dialog (both edges),
  and buttons with cut text. The matrix of sizes and languages and its last result: `docs/UiMatrix.md`.
- **Pictures:** `RagoShell --snapshot x.png` (the app's own, works on a headless Windows or Mac session),
  `dev/ui-shots/shots.sh` (Android over adb, with `phone` / `tablet-p` / `tablet-l` presets), `devicectl` and
  `simctl` for iPad (`docs/MacOsWork.md`).
- **QML check:** `ragoshell --check-qml` loads the main window and validates the command registry; run it before
  every commit that touches QML. Unit tests: `rago_unit_tests`.
