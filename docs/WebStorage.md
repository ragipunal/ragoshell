# WASM Browser Persistence — IDBFS, KeyStore, File Flows (roadmap 8.2, WebAssembly.md Faz 2)

Written: 2026-11-03 · High tier (new persistence surface + crypto-adjacent key
storage + new pre-js entry point). Plan first, code after.

Scope = WebAssembly.md §5 Faz 2 items 2.1–2.4. Mod A playground (8.1) runs the
shell on an in-memory filesystem: every reload loses settings, credentials and
imported keys. Faz 2 makes the browser session survivable without pretending
to be a desktop.

## 1. IDBFS mount + write-back (item 2.1)

- `AppPaths::defaultAppDataDir()` under `Q_OS_WASM` returns
  `/persist/.ragoshell/` instead of `~/.ragoshell/` (Emscripten home is
  `/home/web_user`, MEMFS, gone on reload).
- `/persist` is an **IDBFS mount point**, populated *before* `main()`:
  a `--pre-js` file (`src/ragoshell-app/wasm/persist-pre.js`) registered with
  `target_link_options` pushes a `Module.preRun` callback that calls
  `addRunDependency('idbfs')`, mounts `IDBFS` on `/persist`, runs
  `FS.syncfs(populate=true, …)` and `removeRunDependency` in the callback —
  Emscripten delays `main()` until run dependencies drain, so
  `SettingsManager` sees persisted JSON on first read. **No Asyncify needed.**
  Sync errors only log to console; the app still starts on a fresh volume.
- Write-back is fire-and-forget: `WasmPersist::requestSync()` (new
  `core/utils/WasmPersist.{h,cpp}`, `#if defined(Q_OS_WASM)` body, no-op
  elsewhere) calls `EM_ASM({ FS.syncfs(false, cb) })`. Call sites:
  - end of `SettingsManager::save()` on success (spec's named hook),
  - `importCredentialFileToDir` after a successful copy (SSH keys / TLS certs
    land under `appCredentialsDir()` → `/persist`),
  - the WASM keystore after each write.
- A `beforeunload` listener registered once in `persist-pre.js` issues one
  last `FS.syncfs(false)` so a tab closed right after save still persists.
- Link: `-lidbfs.js` + `--pre-js` in `src/ragoshell-app/CMakeLists.txt` under
  `SYSTEM_WASM`, next to the existing brotli block.

## 2. `KeyStore_wasm` (item 2.2)

Two modes behind the existing `KeyStore` interface (`read/write/remove`,
`isAvailable`, `name`):

- **Default — "Browser Session"**: secrets live in a `QHash<QString,QString>`
  in process memory. `isAvailable()` true, survives navigation *within* the
  page, gone on reload. Honest, zero-pretend behaviour for Mod A.
- **Passphrase mode — "Browser (passphrase)"**: when `RAGOSHELL_KEYSTORE_PASS`
  is present in `ENV` (Emscripten env vars are injectable via `Module['ENV']`
  / qtloader config later), secrets are sealed with the existing
  `SyncCrypto` primitives — Argon2id(passphrase, random salt) → 32-byte key,
  AES-256-GCM per entry, AAD bound to `ragoshell-keystore/v1/<account>` —
  and the JSON blob is written to `<appDataDir>/keystore.json` (i.e. under
  `/persist`), then `WasmPersist::requestSync()`. Salt + KDF params persist
  inside the blob; key material never touches disk.
- The encrypted-file core is platform-neutral → unit-testable natively
  (`KeyStore_passfile_test.cpp`): round-trip, wrong-passphrase returns
  `NotFound`/`Error` without wiping the file, corrupt blob fails closed,
  `remove` drops the entry. The `FS.syncfs` call is the only
  `Q_OS_WASM`-guarded line.
- `KeyStore.cpp` factory gains `#elif defined(Q_OS_WASM) return createWasmKeyStore(name);`.

## 3. File open/save flows (item 2.3)

Qt 6.12's WASM platform plugin implements the `QtQuick.Dialogs` `FileDialog`
with the browser picker for **open**: the picked file is copied into MEMFS and
the dialog hands back a real `file://` path. Open flows (import file pick,
`.js` script open, TLS certificate pick) therefore need no new API —
`Backend.readFile` and `importCredentialFileToDir` read that MEMFS path as-is.

**Save** flows are different: a QML `SaveFile` dialog only picks a name —
nothing downloads what the app writes afterwards. Two `Q_INVOKABLE`s cover it
(`Q_OS_WASM` only, `Qt6::Widgets` is already linked via QScintilla):

- `Backend.saveFileAs(name, text)` → `QFileDialog::saveFileContent` — used by
  the `.js` script save path in `Main.qml`.
- `Backend.downloadFile(path)` → reads a finished MEMFS file and downloads it
  — used by `ImportExportDialog`: on wasm the export writes to a generated
  `/tmp/<coll>.<ext>` path (path field may stay empty) and
  `onTransferFinished` downloads it under that name.

- `importCredentialFileToDir` — copied file lands under
  `appCredentialsDir()` (`/persist/…`) + `requestPersistSync()`.
- `ensureLegacyStoragePermission` — verified no-op on Emscripten.
- `UiServices::getSaveFileName` (`ScriptInfo::saveAs`) — returns a MEMFS path
  on wasm; the script lands in app storage (persistent via `/persist`) rather
  than downloading. Acceptable semantic for "Save As" into app storage;
  noted for the 8.6 review if a download is wanted instead.

## 4. Threat list

| # | Threat | Mitigation |
|---|---|---|
| T1 | Private mode / quota → IDBFS mount or `syncfs` fails | Errors logged to console only; app runs on MEMFS; `save()` still returns its own result — persistence failure never crashes the app |
| T2 | Tab closes between save and async write-back | `beforeunload` hook does a final `syncfs(false)`; the write itself already hit MEMFS |
| T3 | pre-js runs on a runtime without `FS`/`IDBFS` | Guarded (`typeof FS`); `removeRunDependency` in a `finally`-style path so the app can never be wedged at boot |
| T4 | Passphrase-derived key leaks | `SyncCrypto::cleanse` on the master-key buffer after each seal/open; passphrase env read once, never logged |
| T5 | Corrupt/tampered `keystore.json` | GCM tag failure → `Error`, entry treated absent; file is **not** deleted (a transient read error must not destroy secrets) |
| T6 | Wrong-account AAD confusion | AAD = fixed domain string + account name, same pattern as `SyncCrypto::encryptEnvelope` |
| T7 | Keystore JSON grows unbounded | Entries ≤ 64 KiB each, ≤ 256 accounts — guard in `write` (defensive, UI never approaches it) |
| T8 | Credential import path traversal | Existing `sanitizeCredentialFileName` strips everything to `[A-Za-z0-9._-]`; unchanged |
| T9 | `requestSync` storms (many saves in a burst) | `syncfs` is cheap (dirty-only); no debounce needed at this scale — revisit if profiling says otherwise |
| T10 | wasm `FileDialog` URL pointing outside readable MEMFS | `readFile` opens `url.toLocalFile()` read-only; failure returns empty — same as desktop |

## 5. Test plan

- `WasmPersist`: native build keeps it a no-op — tested indirectly.
- `KeyStore_wasm_test` (native-run): set/clear `RAGOSHELL_KEYSTORE_PASS`;
  write→reopen round-trip, wrong passphrase, corrupt file, remove, isAvailable,
  mode switch (env off ⇒ memory only ⇒ nothing written to disk).
- `AppPaths_test`: extend with the wasm default-dir expectation compiled only
  under `Q_OS_WASM` (guard) — native expectations unchanged.
- Native suite must stay 364/364 — the seam is additive.
- `--check-qml`, `check-i18n` (new `qsTr` only if dialogs gain text).
- Browser smoke (manual, item 2.4 — human): reload keeps connections, file
  open/save round-trips, passphrase keystore survives reload.

## 6. Explicitly out of scope

- `wasm_multithread` COOP/COEP headers (host concern, Faz 1 note).
- Clipboard/`getOpenFileContent` C++ statics — QML dialogs already cover UX.
- 2.4 device browser matrix — human item, ticked `[~]` with this doc as prep.
