# Focused WASM Live Playground UX (`W5`)

Status: Implemented (`W5`, `2026-11-27`)
Companion specs: `docs/WebAssembly.md`, `docs/TransportShell.md` (`W4`), `ragoshell-infra/docs/wasm-demo-vhost.md` (`W2`)

## 1. Goal & Context

`demo.<domain>` serves the static WebAssembly build (`Mod A`) as a zero-install, interactive MongoDB shell and GUI showcase backed by `InMemoryDemoTransport` (`W4`).

Without mode-aware UI curation, a first-time browser visitor lands on a blank Welcome screen, sees desktop/gateway concepts (`Local Agent`, `Custom Gateway`, pairing tokens, `Server Monitor`, `Cloud & Sync`, `Check for Updates`, `AI Query Assistant`), and can trigger background HTTPS calls that violate `50-demo.conf.template`'s strict `connect-src 'self'` Content Security Policy.

At the same time, Phase 2 self-hosted deployments (`Mod B` Docker gateway and `Mod C` local agent) compile the same WebAssembly frontend and require full access to `Local Agent`, `Custom Gateway`, and optional cloud/AI configuration.

`W5` introduces explicit runtime mode resolution (`Playground` vs `Gateway` vs `DesktopOrMobile`) so the default static WASM build delivers a focused, zero-click Playground experience while preserving 100% of desktop and `RAGOSHELL_WASM_MODE=gateway` capabilities.

---

## 2. Step 1 — Mode Resolution (`WasmMode` & `PlatformInfo`)

Pure mode resolution lives in `src/ragoshell/core/utils/WasmMode.h` (`rago-core`) so it is directly covered by `rago_unit_tests`:

| Build (`isWasmBuild`) | `RAGOSHELL_WASM_MODE` (`productEnv("WASM_MODE")`) | Resolved `WasmRuntimeMode` | `Platform.isWasm` | `Platform.isWasmPlayground` |
|---|---|---|---|---|
| `false` (Desktop / Mobile) | Any value (ignored) | `DesktopOrMobile` | `false` | `false` |
| `true` (`Q_OS_WASM`) | Unset / empty | `Playground` | `true` | `true` |
| `true` (`Q_OS_WASM`) | `"playground"` (case-insensitive, trimmed) | `Playground` | `true` | `true` |
| `true` (`Q_OS_WASM`) | Unrecognized value (fail-closed) | `Playground` | `true` | `true` |
| `true` (`Q_OS_WASM`) | `"gateway"` (case-insensitive, trimmed) | `Gateway` | `true` | `false` |

Additional `PlatformInfo` rules:
- `PlatformInfo::os()` returns `"wasm"` under `#elif defined(Q_OS_WASM)` (fixing the previous fallback to `"linux"`).
- Desktop/mobile production runs never honor `RAGOSHELL_WASM_MODE`. For automated layout verification (`--check-qml`, `--check-geometry`, `--open-view`), `app/main.cpp` honors `--wasm-playground-preview` **only** when `headlessRun` is already active, flipping `PlatformInfo`'s UI flags without relaxing any compile-time `#if defined(Q_OS_WASM)` transport guards in `MongoClient`, `MongoWorker`, or `QmlBackend`.

---

## 3. Step 2 — Zero-Network CSP Alignment (`connect-src 'self'`)

`50-demo.conf.template` enforces `connect-src 'self'` on `demo.<domain>`. Any automatic or user-triggered request to external origins (`api.<domain>`, third-party AI APIs) would fail in browser DevTools with CSP errors.

1. **`UpdateService`**:
   - When `_platform && _platform->isWasm()` (and `_debugPreviewMode == 0`), constructor skips `restoreCachedState()` and `QNetworkInformation` reachability listeners; `bannerVisible()` returns `false`; `onWelcomeShown()`, `checkNow()`, and `startCheck()` return early without issuing `QNetworkRequest`s.
2. **`CloudService`**:
   - When `_platform && _platform->isWasmPlayground()`, constructor skips `restoreOfflineSession()` and `setAutoRefreshEnabled(true)`. Interactive entry points (`startSignIn`, `refreshStatus`, `syncNow`) also return early in Playground mode as defense-in-depth.

---

## 4. Step 3 — Zero-Click Startup Flow (`app/main.cpp` & `WasmMode`)

When `platformInfo.isWasmPlayground()` and `!headlessRun`:

1. **Demo Connection Guarantee**:
   - Inspects `settings->connections()` for a connection with `transportKind() == "memory"`.
   - If none exists (first visit, or cleared storage), seeds a `"memory"` connection using `BrandInfo::productName` (`"<ProductName> Playground"`, `mongodb://demo/`, `defaultDatabase = "catalog"`, `transportKind = "memory"`), never spelling the brand literal in code (`docs/Brand.md`).
   - Refreshes `ConnectionListModel` if a connection was added or normalized.
2. **Noise-Free Workspace**:
   - Disables the crash-recovery modal startup prompt (`scriptDrafts.setStartupPromptEnabled(false)`).
   - Closes the initial `Welcome` tab (`backend.tabs()->setWelcomeOpen(false)`) so the user lands directly in the live shell tab once connected (`Welcome Tab` remains accessible via `View › Welcome Tab` / Command Palette).
3. **Auto-Connect & Starter Query Execution**:
   - Unless `--connect` was passed on the command line, calls `backend.connectToServer(demoUuid)` on startup.
   - Connects a one-shot handler waiting for `backend.databasesListed() && backend.shells().isEmpty()` and opens a shell tab on database `"catalog"` (`openShellWithScript(demoUuid, "catalog", WasmMode::playgroundStarterScript(), true)`), immediately executing `db.getCollection('products').find({ active: true }).sort({ price: -1 })` while leaving commented ES2016 aggregation and write examples ready to select and run with `F5`.

---

## 5. Step 4 — Surface Curation, Reset Demo Data & Download CTA

### 5.1 Curated Surfaces

| Surface | `Platform.isWasmPlayground == true` (`Mod A`) | `RAGOSHELL_WASM_MODE=gateway` (`Mod B/C`) | Desktop / Mobile |
|---|---|---|---|
| `AppBar` primary actions | **Reset Demo** (`↻`) + **Download App** (`⬇`, `Brand.downloadUrl`) | `New Connection` (`✦`) + `Monitor` (`◉`) + `Sync` (`⇅`) | `New Connection` + `Monitor` + `Sync` |
| `AppActions` & `MainShortcuts` | Hides `file.new` (`Ctrl+N`), `file.cloudSync`, `tools.ai` (`Ctrl+Shift+G`), `tools.monitor` (`Ctrl+Shift+M`), `tools.syncNow`; adds `tools.resetDemo` & `help.downloadDesktop`; hides `file.quit` (`Ctrl+Q`), `Ctrl+M`, `help.logFolder` on all WASM | Hides `file.quit`, `Ctrl+M`, `help.logFolder` (`noWasm`); keeps `New Connection`, `Cloud & Sync`, `AI` | All desktop/mobile commands |
| `SavedConnectionsSection` | Shows active demo connection with `Connect`/`Refresh` & `Forget Schema Data`; hides `+ New Connection`, `Edit`, `Clone`, `Move`, `Remove`, `Server Monitor`, `Disconnect` | Full connection list & actions | Full connection list & actions |
| `ExplorerNodeMenu` | Keeps working shell/CRUD/index/schema/aggregation items; hides `Server Monitor`, `Users & Roles`, `Export/Import`, `Ask AI`, storage-engine/legacy commands (`Compact`, `Validate`, `Latency`, `Shard Distribution`, `Profiling`, `Views/TimeSeries/Clustered`, `system.js`) | Full menu | Full menu |
| `ConnectionDialog` | Defense-in-depth: `rebuildChannels()` offers only `["memory"]` and `collectData()` forces `transportKind = "memory"`, `transportToken = ""` | Offers `memory`, `hosted` (when entitled), `agent`, `custom` | Native TCP/TLS/SSH/SRV |
| `PreferencesDialog` | Keeps Appearance, Language (15 locales), Query & Display, Editor & Shell, Schema Analysis; hides `trayCheck`, `SAVED PASSWORDS`, `AI QUERY ASSISTANT`, `MCP`, `Cloud Account`, `UPDATES & USAGE` | Hides `trayCheck`, `MCP`, `UPDATES & USAGE`; keeps `AI` & `Cloud Account` | Full preferences |
| `AboutDialog` | Replaces `Check for Updates` with `Download Desktop App` (`Brand.downloadUrl`), keeping 2-column grid symmetry | Replaces `Check for Updates` with `Download Desktop App` | `Check for Updates` (non-store) |
| `WelcomePage` & empty `MainWorkspaceArea` | Replaces `New Connection` CTA with `Open Playground Shell` + `Download Desktop App` | `New Connection` + `View Logs` | `New Connection` + `View Logs` |

### 5.2 Reset Demo Data (`QmlBackend::resetPlaygroundData`)
- Triggered from `AppBar` (`↻`) or Command Palette (`tools.resetDemo`), gated by a confirmation dialog (`MainServiceDialogs.qml`) so scratch collections/documents created during the session are not wiped by an accidental click.
- `QmlBackend::resetPlaygroundData()`:
  - Stops any currently busy shell tab bound to a `"memory"` connection.
  - Calls `InMemoryDemoTransport::resetSharedStore()` (thread-safe mutex-guarded factory reset of `catalog.{products,customers,orders}` and indexes).
  - Clears analyzed schema snapshots (`forgetSchema(uuid)`) and reloads Explorer databases (`refreshServer(uuid)`) for every connected `"memory"` server.
  - Updates the status bar with a localized confirmation message.

### 5.3 Brand-Derived Download URL (`BrandBridge::downloadUrl`)
- `WasmMode::resolveDownloadPageUrl(BrandInfo::productUrl)` strips trailing slashes, appends `"/download/"`, and validates the result with `UpdatePulse::isSafeDownloadUrl` (strict `https://`, no userinfo, `<= 1024` chars).
- Exposed as `Brand.downloadUrl`; opened via `Qt.openUrlExternally` only after `Brand.isSafeHttpsUrl(Brand.downloadUrl)` passes.

---

## 6. Threat Model & Edge Cases

- **`T1` — Desktop environment-variable spoofing of `"memory"` transport or Playground UI**:
  - *Mitigation*: `resolveWasmRuntimeMode(false, env)` unconditionally returns `DesktopOrMobile`. `MongoClient`, `MongoWorker`, and `QmlBackend::schemaOptions` keep compile-time `#if defined(Q_OS_WASM)` guards. `--wasm-playground-preview` is accepted only inside `headlessRun` (`--check-qml` / `--check-geometry` / `--open-view`).
- **`T2` — CSP violations / unintended phone-home on `demo.<domain>`**:
  - *Mitigation*: `UpdateService` is inert on WASM; `CloudService` skips session restore and token auto-refresh in Playground mode; `AI Query Assistant` is hidden in Playground mode so browser `connect-src 'self'` is respected cleanly.
- **`T3` — Unsafe external URL navigation from Download CTA**:
  - *Mitigation*: `Brand.downloadUrl` is derived solely from compile-time `BrandInfo::productUrl` and validated through `UpdatePulse::isSafeDownloadUrl` (`https://` only, no credentials/fragments/javascript schemes).
- **`T4` — Resetting demo data while a shell script is running or Explorer cache is populated**:
  - *Mitigation*: `InMemoryDemoStore` serializes commands behind `std::mutex`; `QmlBackend::resetPlaygroundData()` stops busy `"memory"` shells first, resets `sharedStore()`, clears `SchemaCatalog` for `"memory"` UUIDs, and calls `refreshServer(uuid)` so Explorer and schema badges never display stale dropped collections.
- **`T5` — Narrow viewports (`< 720 px`), RTL (`ar`), and 15-language text expansion**:
  - *Mitigation*: `AppBar` collapses Playground pills to `RagoIconButton`s below `720 px` and places `tools.resetDemo` + `help.downloadDesktop` in `moreMenu` and Command Palette (`TodoMac.md` §0: no action hidden without a place to go). All new strings use `%1` placeholders (never glued onto `qsTr()`), are translated across all 15 `.ts` catalogs (`100%` Turkish), and are verified with `--wasm-playground-preview --check-geometry` in wide, narrow, LTR, and RTL layouts.
