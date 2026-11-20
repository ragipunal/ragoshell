# RagoShell — every screen on every platform (the matrix)

Status: **2026-10-29.** What was looked at, how, what it showed, what was
fixed, and what has not been looked at yet. The tools are in `dev/ui-shots/` (see its `README.md`).

## How it was checked

1. **The layout guard** (`dev/ui-shots/geometry.sh`) starts the Windows desktop build at a window size, density and
   language, opens a view and lists every visible item that sticks out of the window or of its dialog (right edge,
   left edge for Arabic) and every button whose text had to be cut. A view is "ok" when it lists nothing. It does not
   judge looks: it catches the failures that make a screen unusable (a button you cannot reach, a field with no
   width, a label running under another).
2. **Pictures**, to judge what the guard cannot: the desktop build's own `--snapshot` (Windows, light and dark, 100 %
   and 150 % scaling, English / German / Arabic), `adb screencap` on the Android tablet (native landscape, and the
   `phone` / `tablet-p` presets of `shots.sh`), and `xcrun devicectl device capture screenshot` of the iPad over the
   Mac.
3. **Real data**: the shell and monitor tabs are opened on a local docker `mongo:4.0` server with seed data (`dev/mongo-test/` of the author, git-ignored), not
   on a mock.

Sizes (in dp): **phone** 360×800 touch, **tablet portrait** 800×1280 touch, **tablet landscape** 1280×800 touch,
**desktop** 1100×700 compact. Touch sizes are the table of `docs/DesignSystem.md`. Languages: English, Turkish (long words),
German (longest words), Arabic (right to left), Japanese (wide glyphs, no spaces).

## Guard results

`ok` = nothing listed. A number = items listed (the same screen, that language and size). 20 views × 4 sizes × 5
languages = 400 runs; `tabs`, `shell` and `monitor` were run again after the fixes below, the dialog views were run
once, before the toolbar fixes below (nothing in the dialogs changed since).

**360x800 Touch**

| view | en | tr | de | ar | ja |
|---|---|---|---|---|---|
| conn | ok | ok | ok | ok | ok |
| prefs | ok | ok | ok | ok | ok |
| about | ok | ok | ok | ok | ok |
| shortcuts | ok | ok | ok | ok | ok |
| history | ok | ok | ok | ok | ok |
| create-collection | ok | ok | ok | ok | ok |
| index | ok | ok | ok | ok | ok |
| user | ok | ok | ok | ok | ok |
| doc | ok | ok | ok | ok | ok |
| palette | ok | ok | ok | ok | ok |
| more-menu | ok | ok | ok | ok | ok |
| theme-menu | ok | ok | ok | ok | ok |
| export-python | ok | ok | ok | ok | ok |
| hostkey | ok | ok | ok | ok | ok |
| hostkey-changed | ok | ok | ok | ok | ok |
| recover | ok | ok | ok | ok | ok |
| log | ok | ok | ok | ok | ok |
| tabs | ok | ok | ok | ok | ok |
| shell | ok | ok | ok | ok | ok |
| monitor | ok | ok | ok | ok | ok |

**800x1280 Touch**

| view | en | tr | de | ar | ja |
|---|---|---|---|---|---|
| conn | ok | ok | ok | ok | ok |
| prefs | ok | ok | ok | ok | ok |
| about | ok | ok | ok | ok | ok |
| shortcuts | ok | ok | ok | ok | ok |
| history | ok | ok | ok | ok | ok |
| create-collection | ok | ok | ok | ok | ok |
| index | ok | ok | ok | ok | ok |
| user | ok | ok | ok | ok | ok |
| doc | ok | ok | ok | ok | ok |
| palette | ok | ok | ok | ok | ok |
| more-menu | ok | ok | ok | ok | ok |
| theme-menu | ok | ok | ok | ok | ok |
| export-python | ok | ok | ok | ok | ok |
| hostkey | ok | ok | ok | ok | ok |
| hostkey-changed | ok | ok | ok | ok | ok |
| recover | ok | ok | ok | ok | ok |
| log | ok | ok | ok | ok | ok |
| tabs | ok | ok | ok | ok | ok |
| shell | ok | ok | ok | ok | ok |
| monitor | ok | ok | ok | ok | ok |

**1280x800 Touch**

| view | en | tr | de | ar | ja |
|---|---|---|---|---|---|
| conn | ok | ok | ok | ok | ok |
| prefs | ok | ok | ok | ok | ok |
| about | ok | ok | ok | ok | ok |
| shortcuts | ok | ok | ok | ok | ok |
| history | ok | ok | ok | ok | ok |
| create-collection | ok | ok | ok | ok | ok |
| index | ok | ok | ok | ok | ok |
| user | ok | ok | ok | ok | ok |
| doc | ok | ok | ok | ok | ok |
| palette | ok | ok | ok | ok | ok |
| more-menu | ok | ok | ok | ok | ok |
| theme-menu | ok | ok | ok | ok | ok |
| export-python | ok | ok | ok | ok | ok |
| hostkey | ok | ok | ok | ok | ok |
| hostkey-changed | ok | ok | ok | ok | ok |
| recover | ok | ok | ok | ok | ok |
| log | ok | ok | ok | ok | ok |
| tabs | ok | ok | ok | ok | ok |
| shell | ok | ok | ok | ok | ok |
| monitor | ok | ok | ok | ok | ok |

**1100x700 Compact**

| view | en | tr | de | ar | ja |
|---|---|---|---|---|---|
| conn | ok | ok | ok | ok | ok |
| prefs | ok | ok | ok | ok | ok |
| about | ok | ok | ok | ok | ok |
| shortcuts | ok | ok | ok | ok | ok |
| history | ok | ok | ok | ok | ok |
| create-collection | ok | ok | ok | ok | ok |
| index | ok | ok | ok | ok | ok |
| user | ok | ok | ok | ok | ok |
| doc | ok | ok | ok | ok | ok |
| palette | ok | ok | ok | ok | ok |
| more-menu | ok | ok | ok | ok | ok |
| theme-menu | ok | ok | ok | ok | ok |
| export-python | ok | ok | ok | ok | ok |
| hostkey | ok | ok | ok | ok | ok |
| hostkey-changed | ok | ok | ok | ok | ok |
| recover | ok | ok | ok | ok | ok |
| log | ok | ok | ok | ok | ok |
| tabs | ok | ok | ok | ok | ok |
| shell | ok | ok | ok | ok | ok |
| monitor | ok | ok | ok | ok | ok |

## What the pictures showed (not caught by the guard)

| Platform / size | Screens looked at | Result |
|---|---|---|
| Android tablet, landscape 1280×800 | Welcome, "more" menu, Connection, Preferences, Index, shell tab (tree), Server Monitor, app drawer (launcher icon) | good; the shell toolbar fits; icon sits well in the launcher's round mask |
| Android tablet, portrait 800×1280 | Welcome, Connection, Index, Preferences, shell, Server Monitor | good after U2 and U4; the Explorer takes 360 of 800 dp, so the work area is 440 dp (monitor 2×2 cards, charts stacked) |
| Android, phone preset 360×800 | Connection, Preferences, Collection, Index, User, History, About, shell, Server Monitor | good after U2 and U4; the shell toolbar, context strip and paging bar scroll sideways, the tree's Type column is off to the right (scrolls) |
| iPad 8th gen, landscape | Welcome (no key text without a keyboard), shell tab, Server Monitor | good; the icon on the home screen and Dock |
| iPad Pro 13" and iPad mini (iOS simulator, portrait) | shell tab, Server Monitor, Preferences | good; on the mini (744 pt) the monitor is 2×2 cards with stacked charts, the shell toolbar scrolls |
| macOS 27 (Apple M3 Max), guard: 27 runs (3 sizes, English and Arabic, conn / prefs / index / shell / monitor / tabs) all ok; pictures at 1280×800 | Preferences, shell tab on the seed data, Welcome in Arabic and dark | good; ⌘K and ⌘N labels, mirrored Arabic; the Welcome paragraphs still said Ctrl on the Mac: fixed (`Platform.macKeys`) |
| Windows 100 %, light | Welcome, Preferences, shell tab, Server Monitor, Tabs | good |
| Windows 150 % + German, light | Connection dialog | good; the URI row scrolls with the page, the test bar stays |
| Windows dark | Preferences | good |
| Arabic (Windows, 1280×800 and shell with two tabs) | shell tab, tab strip with six tabs | mirrored correctly after the tab strip fix |

## What U4 fixed

- **Toolbar buttons lost their text** ("Tr…", "Ta…", "JS…"): the pill buttons shrink and elide in a
  narrow row. Toolbars now scroll sideways instead (`HScrollRow.qml`): the shell result toolbar, the paging bar, the
  context strip, the monitor header and its filter bar. The guard reports a button with cut text (`cut=`).
- **Right-to-left tab strip**: only the current tab stayed in view; the strip's "keep the current tab in range" is
  turned off and the current tab is scrolled into view by hand (`Main.qml`, `workTabs.revealCurrent`).
- **Server Monitor on a narrow page** (it is the page's width that counts, not the window's: a tablet in portrait
  leaves 440 dp): KPI cards 2×2 below 720 dp, charts one above the other, legends wrap, the Type, Duration, Client
  and Plan columns give way below 560 / 900 dp, the empty-state text wraps.
- **The guard itself**: it started by measuring against the window (it missed a dialog sticking out of its frame),
  did not open the shell and monitor tabs at all (a wrong config path made every tab "ok"), and could not see the
  left edge. All three fixed; the first matrix run that did see them is the source of the list above.

## Known limits (not defects of a single screen)

- **Tree view**: the key / value / type columns want about 476 dp; in a narrow work area the Type column is cut at the
  edge (it scrolls horizontally).
- **Not looked at**: Linux desktop, iPad Split View, Stage Manager and rotation of the
  simulators, Android split window and a physical phone, macOS by hand (Dock, ⌘ keys), Windows at 100 % in Arabic and Japanese by picture (guard only), the detached log window, a hardware keyboard on the tablets (shortcut
  text), screen readers. Universal iPhone + iPad (`iPhone 18 Pro`, `iPad mini`, `iPad Pro` simulators) and phone portrait/landscape (`360×800`, `800×360`) have been verified. The remaining Mac, iPad and Linux items are in `docs/MacOsWork.md`.
- **Translations** of the new texts are machine translations; `ky`, `kk` and `uz` want a native speaker.
