# Cloud & Sync UX Simplification and Top-Bar Integration

Streamlines the Cloud Account & Encrypted Sync user experience (`src/ragoshell-app/qml/PreferencesAccountSection.qml`), introduces a dedicated `CloudSyncDialog.qml` decoupled from Preferences' Save/Cancel workflow, and exposes one-click sync status, Sync Now, and Sync Setup in the main top bar (`AppBar.qml`), the More menu, and the Command Palette (`AppActions.qml`).

## 1. Goals

1. **Remove visual noise and inverted hierarchy** in the Cloud & Sync view:
   - Hide signed-out onboarding prose once signed in.
   - Hide the default server configuration card unless a custom server, organization policy, environment override, or active server edit is present.
   - Remove internal short device hex IDs and routine JWT validity timestamps (show validity timestamp only during offline grace).
   - Elevate everyday primary actions (`Sync Now`, `Set Up Encrypted Sync…`, `Sign In with Browser…`) and move rare or destructive operations (`Refresh Status`, `Change Server…`, `Disable Sync`, `Delete Remote Data…`) into a collapsible `Advanced` section so zero red destructive buttons appear at rest.
2. **Simplify Encrypted Sync setup**:
   - Replace the ambiguous *"This device already has a sync vault"* checkbox with an explicit two-mode selector: **New Sync Vault** (first device) vs **Use Existing Passphrase** (second+ device), plus a quiet single-device key option.
   - Standardize all controls on `RagoCheckBox` (2-column grid at `>= 520 dp`, 1-column below `520 dp`), `RagoTextField`, and `RagoDialog`.
3. **Top-bar and command-palette discoverability**:
   - Place a status-aware Sync button in `AppBar.qml` (`⇅` pill on wide windows `>= 900 dp`, `⇅` icon button below `900 dp`) with a quick menu for one-click `Sync Now` and `Set Up Sync…` / `Cloud & Sync…`.
   - Register `file.cloudSync` and `tools.syncNow` in `AppActions.qml` for the `⋮` More menu and `Ctrl+K` Command Palette.

## 2. Step 1 — `CloudService` Error Mapping & Preview Modes

- Map internal `SyncEngine` error codes (`offline`, `unauthorized`, `sync_key_locked`, `kdf_params_missing`, `sync_entitlement_required`, `response_too_large`, `put_http_429`, `sync_conflict`, etc.) to localized user-facing sentences in `CloudService::syncLastError()` instead of surfacing raw error tokens.
- Extend `CloudService::previewDebugState(int mode)` with `mode = 3` (signed-in with active encrypted sync, sample collections, and recent sync timestamp) so headless `--open-view` snapshots and `--check-geometry` verify the active-sync layout without live credentials.

## 3. Step 2 — Simplified `PreferencesAccountSection.qml` & `CloudSyncDialog.qml`

- **`PreferencesAccountSection.qml`**:
  - Accepts `property bool standalone: false` (hides the redundant section title row when hosted inside `CloudSyncDialog.qml`, where the dialog header already shows the title and status).
  - Exposes `openSyncSetup()` so callers (`AppBar` -> *Set Up Encrypted Sync…*) can open straight into the passphrase setup flow.
  - **Signed-out state**: concise optional-account note, device name `RagoTextField`, primary `Sign In with Browser…` button, and a subtle inline `Change Server…` trigger when default server is active.
  - **Signed-in state**:
    - Compact account summary card: email, status badge, clean device name, offline-grace notice (only when `offlineGraceActive`), and neutral compact `Manage Account…` / `Sign Out` buttons.
    - Encrypted Sync card: status badge + last sync time, concise E2EE privacy note, 2-column `RagoCheckBox` collection grid + saved-passwords opt-in, primary `Sync Now` (or `Set Up Encrypted Sync…`) button, and an `Advanced` disclosure button revealing `Refresh Status`, `Change Server…`, `Disable Sync`, and `Delete Remote Data…`.
  - Confirmation popups (`syncWipeDialog`, `serverSwitchDialog`) migrate from raw `Dialog` to `RagoDialog` (`parent: Overlay.overlay`, `dangerAccept: true` on remote wipe).
- **`CloudSyncDialog.qml`**:
  - `RagoDialog` (`width: Math.min(visibleWidth - 24, 500)`, `acceptText: qsTr("Close")`, `showReject: false`) hosting `ScrollView` + `FocusReveal` + `PreferencesAccountSection { standalone: true }`.
  - Instantiated lazily via `LazyDialog` in `Main.qml` (`openCloudSync(openSetup)`).
  - `PreferencesDialog.qml` retains `PreferencesAccountSection` inline (plus `revealSection === "account"`) so users browsing Preferences never trigger modal-on-modal dialogs.

## 4. Step 3 — Top Bar (`AppBar.qml`) & Command Registry (`AppActions.qml`)

- **`AppBar.qml`** (hidden only when `AppSettings.disableHttpsFeatures` is `true`):
  - Wide windows (`bar.showName`, `width >= 900`): compact `RagoPillButton` (`iconText: "⇅"`) labeled `Sync` when `CloudService.syncActive` (or `Syncing…` while busy) and `Set Up Sync` otherwise.
  - Narrower windows (`width < 900`): `RagoIconButton` (`glyph: "⇅"`, `accent: CloudService.syncActive`, `danger: CloudService.syncLastError.length > 0 || CloudService.policyInvalid`).
  - Clicking opens `syncMenu` (`RagoMenu`):
    - Signed out: `Set Up Sync…` -> `app.openCloudSync(false)`.
    - Awaiting browser approval: `Complete Sign-In…` -> `app.openCloudSync(false)`.
    - Signed in, sync off: dimmed account row, separator, `Set Up Encrypted Sync…` -> `app.openCloudSync(true)`, `Cloud & Sync…` -> `app.openCloudSync(false)`.
    - Signed in, sync active: `Sync Now` (`shortcutText: "↻"`) -> `CloudService.syncNow()`, dimmed `Last synced: %1` (or warning status), separator, `Cloud & Sync…` -> `app.openCloudSync(false)`.
- **`AppActions.qml`**:
  - Add `file.cloudSync` (`Cloud & Sync…`, also placed in `moreMenu` above `Preferences…`) and `tools.syncNow` (`Sync Now`, enabled when signed in & sync active).
  - Connect `CloudService.onStateChanged` to bump `actions.revision` so menus and the command palette reflect live auth/sync transitions.

## 5. Threats, Privacy, and Layout Edge Cases

| # | Concern | Mitigation |
|---|---|---|
| T1 | Unsolicited network calls (`Roadmap.md` §3.4) | Opening `syncMenu`, `CloudSyncDialog`, or `CommandPalette` reads cached `CloudService` state only; network requests happen strictly on user click (`Sync Now`, `Sign In`, `Refresh Status`). |
| T2 | Passphrase input exposure | `RagoTextField` sets `Qt.ImhSensitiveData \| Qt.ImhNoAutoUppercase \| Qt.ImhNoPredictiveText`, defaults to `TextInput.Password`, resets visibility toggle, and clears passphrase text on submit, cancel, mode switch, and dialog close. |
| T3 | Custom server / enterprise policy visibility | Custom server summary, policy lock badge, environment override notice, and invalid-policy errors remain pinned at the top whenever active; only the default `RagoShell Cloud` state is collapsed into `Advanced`. |
| T4 | Mobile / narrow / RTL geometry | Forms switch to 1 column when `root.width < 520` (component width, not window width); `AppBar` collapses the Sync pill to `RagoIconButton` below `900 dp`; glyphs (`⇅`, `↻`, `✓`) come from `DejaVuSans.ttf` (no emoji). |
| T5 | Dialog clipping inside scroll views | Confirmation dialogs use `RagoDialog` (`parent: Overlay.overlay`, `VisibleArea`), avoiding raw `Dialog` centering inside `ScrollView`. |
| T6 | Localization rules (`ci/check-i18n.mjs`) | Never concatenate text onto `qsTr()`; use `%1` + `.arg()`; refresh all 15 `.ts` catalogs via `update_translations` and keep translations complete. |

## 6. Verification Gates

1. `cmake --build --preset windows-debug`
2. `ctest / rago_unit_tests`
3. `ragoshell --check-qml`
4. `cmake --build --preset windows-debug --target update_translations` and `node ci/check-i18n.mjs`
5. Headless geometry checks (`--check-geometry`) and visual snapshots (`--snapshot`) across desktop (`1280x800`) and narrow mobile (`360x800 Touch` in `en`, `tr`, `ar`) for `cloud-sync`, `cloud-sync-code`, `cloud-sync-pro`, `cloud-sync-active`, `sync-menu`, `more-menu`, and `prefs`.
