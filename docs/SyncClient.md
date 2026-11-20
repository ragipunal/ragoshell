# Encrypted Sync Client (Roadmap 5.3 — A7)

Client side of the zero-knowledge cloud sync. Server contract: `ragoshell-api/docs/SyncProtocol.md`
(endpoints, quotas, ETag/precondition rules). Account/device plumbing: `CloudClient` (RFC 8628,
rotating refresh tokens, ES256 entitlement verify) from Phase 5.1.

## 1. Key hierarchy and storage

```
sync passphrase (user, optional) ──► Argon2id(salt, m=64 MiB, t=3, p=4) ──► master key (32 B)
                                          │
device-local random key (32 B, no passphrase mode) ──► same slot
                                          │
              HKDF-SHA256(master key, info = "ragoshell-sync/v1/" + collection) ──► per-collection AEAD key (32 B)
```

- `libargon2` (vcpkg port, reference implementation, CC0) for Argon2id — added to `vcpkg.json`;
  `OpenSSL::Crypto` (already linked in `rago-core`) provides AES-256-GCM, HKDF-SHA256 (`EVP_PKEY_HKDF`),
  and `RAND_bytes`. No hand-rolled primitives.
- **Key storage:** master key lives in `KeyStore` slot `sync_master_key` (new slot beside
  `refresh_token`/`entitlement_jwt` on `ICloudCredentialStore`); the passphrase itself is never
  persisted. In passphrase-less mode a `RAND_bytes(32)` device key is generated once and stored in
  the same slot — the wizard must print that other devices cannot read those vaults.
- Passphrase mode stores *only the derived master key* in KeyStore (so re-login on the same machine
  does not re-ask); `kdf_params` in the envelope lets a *new* device re-derive it from the passphrase.
- Memory hygiene: `cleanseByteArray`-style wipe (the `CloudClient` pattern) on keys/plaintext buffers.

## 2. Envelope and payload format

Per collection the plaintext is canonical JSON:

```json
{
  "schema_version": 1,
  "collection": "connections",
  "device_id": "<uuid>",
  "saved_at": "<unix ms>",
  "payload": { /* collection-specific, see §3 */ }
}
```

Envelope = `{ciphertext: b64, nonce: b64 (12 B random per write), kdf_params}` where `kdf_params`
(carried opaquely by the server, must be non-secret and ≤ 4 KiB):

```json
{"alg":"argon2id","v":1,"m_cost_kib":65536,"t_cost":3,"parallelism":4,
 "salt":"<b64 16 B>","cipher":"aes-256-gcm","schema_version":1}
```

- AES-GCM AAD = `"ragoshell-sync-v1:" + collection` — binds ciphertext to its collection.
- Salt rotates when the passphrase is (re)set, not per write; nonce is fresh per write.
- `kdf_params` never contains key material (server rejects secret-suggesting key names anyway).

## 3. Collections (wizard-selectable)

| Server collection | Source | Passwords? |
|---|---|---|
| `connections` | `SettingsManager::connections()` → `toVariant` map list | `userPasswordEncrypted` fields included only when the "sync passwords" toggle is on; SSH/TLS key *file paths* are synced as paths (file bytes stay local), noted in UI |
| `snippets` | `QueryHistoryStore` favorites only (`favorite == true`) | n/a |
| `query_history` | non-favorite `QueryHistoryStore` entries (bounded to `kMaxEntries`) | n/a |
| `preferences` | `SettingsManager::convertToMap()` minus excluded keys: `aiApiKeys`, `aiAvailableModels`, `anonymousID`, `cacheData`, `acceptedEulaVersions`, `dbVersionsConnected`, `programExitedNormally`, connections, toolbar layout | API keys never sync |
| ~~`startup_script`~~ | **not synced** — remote startup script + `autoExec` would execute on every device (§7 C14) | — |

Collection names match `^[a-z0-9][a-z0-9._-]{0,63}$`. Payload size guard: serialized payload
> 900 KiB → skip that collection with a `sync_collection_too_large` UI note (server hard cap 1 MiB
ciphertext).

## 4. `SyncEngine` (rago-core, `src/ragoshell/core/cloud/`)

QObject sibling of `CloudClient`, constructed with `CloudClient*` + `SettingsManager*` +
stores; fully testable behind a fake HTTP layer (the `CloudClient_test` pattern — a
`QTcpServer`-based fake implementing the SyncProtocol endpoints).

State machine: `Disabled → Idle → Syncing (pull → merge → push) → Error | Blocked(entitlement)`.
Public surface: `enabledCollections`, `passwordsIncluded`, `syncEnabled` (persisted in settings),
`lastSyncAt`, `syncNow()` (user-initiated — never phones home on its own), `wipeRemote()`,
per-collection `status`, `errorCode`. Signals: `syncStarted/Finished`, `conflictResolved`,
`blockedChanged`.

Flow per sync run:
1. `withValidAccessToken` → `GET /v1/sync` manifest (quota, `writable`, per-collection etags).
2. For each enabled local collection: `GET /v1/sync/{col}` with `If-None-Match <local-etag>` →
   `304` skip / `200` pull / `404` push-only.
3. Merge pulled payload with local (§5), then `PUT` with `If-Match "<etag>"` (or
   `If-None-Match: *` when absent). `412` → re-GET, merge once more, single retry, then
   report `sync_conflict`.
4. `403 sync_entitlement_required` → `Blocked` state (upsell copy; sync stays configured).
5. `409 sync_collection_limit_reached` / `413` → surface per-collection error.
6. `401` → engine stops; `CloudClient` handles session expiry (existing `sessionRevoked`).

Offline tolerance (minimal in 5.3, hardened in 5.4): failed runs keep local etag set and retry on
next explicit `syncNow()` or app focus — no background beacon.

## 5. Conflict rule: last-writer-wins + device label

Per A7. Merge granularity:

- `connections`, `snippets`, `query_history`: **union by id/uuid**; same id on both sides → the
  entry whose `saved_at`/`executedAtMs` is newer wins; tie → the device whose name sorts later
  (deterministic). Merged list is what gets pushed.
- `preferences`: whole-document last-writer-wins on `doc_updated_at`; the winner's
  `device_id` is shown as the conflict label in the UI ("Preferences were replaced by <device>").

## 6. Wizard and UI

New `SyncSetupWizard.qml` dialog opened from `PreferencesAccountSection.qml` (Phase 5.1 Account
section): steps = intro/zero-knowledge note → mode choice (passphrase vs device-key, warnings) →
passphrase set+confirm with strength hint → collection checkboxes + password toggle → finish.
Post-setup: sync status row in Account section (`lastSyncAt`, "Sync now", per-collection errors,
Pro upsell when blocked). All strings `qsTr()`; forms one column < 520 dp; `Theme`/`UiDensity`
sizes; `Platform` for any shortcut text.

## 7. Threats and edge cases

| # | Threat / edge | Handling |
|---|---|---|
| C1 | Passphrase/key leak via logs or `ragoshell.json` | Keys only in `KeyStore`; wipe buffers; never log envelope fields |
| C2 | Weak passphrase → offline brute-force of captured blobs | Argon2id 64 MiB/3 it + strength hint + min length in wizard |
| C3 | Passphrase forgotten on 2nd device | Recovery = wipe remote (`DELETE /v1/sync`) and re-seed; documented in wizard |
| C4 | `RagoCrypt`-encrypted passwords inside envelope | Double-encrypted; toggle off by default; UI explains what is included |
| C5 | Corrupted/malformed pulled blob (bad b64, wrong tag, huge kdf) | Strict decode + GCM verify; on failure keep local copy, mark collection `error`, never merge garbage |
| C6 | Clock skew breaking last-writer-wins | Compare `saved_at` only within skew bound; deterministic device-name tiebreak |
| C7 | Payload > 1 MiB (500-entry history) | §3 size guard + trim query_history oldest-first |
| C8 | Downgrade Team→Pro over limit | Server `409`/`412` handled; UI lists which collections to uncheck |
| C9 | Sync runs while signed out / revoked | `401` → stop + `sessionRevoked` path; no retry loop |
| C10 | Two devices push simultaneously | Mandatory preconditions + `412` retry-once; ABA-proof etags server-side |
| C11 | Free user enables sync | `403` → `Blocked` state; no data loss locally |
| C12 | KeyStore unavailable (headless Linux) | Refuse sync setup with clear error (never fall back to plaintext storage) |
| C13 | Hostile server swaps `kdf_params` (desync / weak-KDF) | Params pinned at provisioning (`configure*`/`attachToRemoteVault`); never adopted from pulled envelopes; inbound params must be ≥ shipped defaults |
| C14 | `startup_script`/`autoExec` synced → remote code execution on every device | Both excluded from sync: `startup_script` is not a collection, `autoExec` never exported/applied |
| C15 | Remote-pushed merge deletes live `ConnectionSettings*` held by UI | In-place `apply()` for updates; removals via `reorderConnections()` + `deleteLater()` |
| C16 | `ssh.*Encrypted`/`ssl.*Encrypted` device-key ciphertexts leak or corrupt remotely | Always stripped; plaintext re-attached only inside the sealed envelope when passwords opt-in |
| C17 | Far-future remote `updated_at` wins every merge | Remote timestamps clamped to `now + 24h` |
| C18 | `sync-state.json` SHA-256 entry hashes = "does this device have X" oracle | Documented residual: fingerprints are hashes of canonical plaintext metadata; enable sync implies accepting local-disk metadata traces |
| C19 | Unbounded server response → memory exhaustion | Every sync reply aborted past 1.6 MiB buffered (`kMaxResponseBytes`, §10 R12) |
| C20 | Passphrase resident in heap after failed attach | `finishAttach()` wipes `m_attachPassphrase` on every exit path |

## 8. Test matrix (`rago_unit_tests`, fake server)

encrypt→decrypt roundtrip incl. wrong-key GCM failure; KDF determinism vs `kdf_params` (salt reuse);
manifest GET + 304 path; create via `If-None-Match: *`; update via `If-Match`; `412` → merge → retry;
union merge + tiebreak; `403` free account; `409`/`413` surfaces; offline queue keeps local etags;
KeyStore slot isolation; no secrets in written settings.

## 9. Offline tolerance, revocation and device removal (5.4)

- **Offline tolerance:** transport-level failures (no HTTP status / `network_error` from token
  refresh) do not mark collections failed — `finishOffline()` sets state `Error("offline")` and
  arms a single-shot `QTimer` retry with exponential backoff (30 s → ×2 → 5 min cap) while sync
  stays enabled and the session is signed in. Successful runs reset the backoff. Local state
  (etags, fingerprints, `sync-state.json`) is persisted between runs, so queued changes flush on
  the next reachable run; nothing needs re-derivation.
- **Token revocation:** any `401` (`unauthorized`, `invalid_grant`, revoked refresh) ends the run
  with state `Error("unauthorized")` and **disarms the retry timer** — a revoked device never
  loops. `CloudClient`'s refresh path wipes KeyStore secrets and falls to `SignedOut`; the engine
  settles to `Disabled`/`Error` via the `signedOut` signal — one clean state transition, no crash.
  `CloudClient` already honors `ValidOfflineGrace` (14 days) for read paths.
- **Device removal:** a device deleted on the portal keeps its already-uploaded blobs (the vault
  is per-user, not per-device); it can never write again — the server re-checks device liveness on
  every sync write, so any in-flight write after deletion fails `401` → single sign-out prompt.

## 10. Review fixes 2026-11-04

Findings of the 2026-11-04 sync-client review (numbers are the review's). Each fix has a
regression test next to the code (`SyncEngine_test`, `SyncCollections_test`, `KeyStore_test`).

- **R1 — history store changed behind `QueryHistoryModel`.** A merge of `snippets`/`query_history`
  rebuilt the store with `fromJson()`; the model kept stale row indices (out-of-range read in
  `data()`, ids renumbered under the UI) and nothing saved the file, so after a restart the pulled
  rows were missing and the next export tombstoned them on every device. Fix: `SyncEngine` emits
  `historyChanged()` after a merge changed the store; `CloudService` forwards it and the app calls
  `QueryHistoryModel::reloadFromStore()` (rebuild + immediate save) — synchronously, so the file
  is on disk before `sync-state.json` records the pulled keys. `data()` also bounds-checks the index.
- **R2 — a device with an empty history wiped history everywhere.** `cleared_at` was set to "now"
  whenever the store was empty, i.e. on every fresh install joining a vault. Fix: a local clear
  is only inferred when the previous sync state listed history keys and none are left. Edge:
  favorites never count; a clear applied *from* a remote is re-stamped with the local time on the
  next export (pre-existing, unchanged).
- **R3 — etag/fingerprints committed before the merged PUT.** A failed PUT after a pull left the
  remote etag in `sync-state.json`, the next run got `304` and never pushed the local-only rows.
  Fix: the pull path commits state only when no push is needed; otherwise `handlePutResult()`
  commits on `200`/`201`. A failed PUT keeps the old etag, so the next run pulls and merges again.
- **R4 — tombstones re-stamped on every export.** Fix: a key whose previous slot already is a
  tombstone (`h == "t"`) keeps its original `u`/`d`; only the first detection uses now/this device.
  So a later edit elsewhere can win over an older delete, 90-day pruning works again and an
  unchanged collection with tombstones no longer re-uploads on every run.
- **R6 — preferences of a new/reset device overwrote everyone.** With no saved `_doc` slot the
  local document is exported with `doc_updated_at = 0` ("never established") and `merge()` lets
  the remote win when the local timestamp is 0. Edge: a device that lost `sync-state.json` also
  adopts the remote preferences once (local edits since the last sync are lost — acceptable).
- **R7 — invented KDF params.** `putCollection()` made up a random salt when `kdfParams` were
  missing (lost `sync-state.json`, crash after a join, `Blocked` before the first save) and
  published it next to the real key, so later joins failed with `passphrase_mismatch`. Fix:
  stored params are validated with the inbound rules (`parseKdfParams`); in passphrase mode a
  missing/invalid set ends the run with `kdf_params_missing` (recovery: disable, join again);
  device-key mode (params only cosmetic) still mints and now persists them. `attachToRemoteVault`
  pins the params only after the key was stored and saves `sync-state.json` at once;
  `markBlocked()` also saves.
- **R8 — `RagoCrypt` key from a 32-bit seed.** `generateKey()` seeded `mt19937_64` with one
  `random_device` value. Fix: 64 bits from `QRandomGenerator::system()` mapped onto the same range
  (`[2^61, 2^62)`); format and range unchanged, so existing keys and passwords are untouched.
- **R10 — stale replies and wipe during a run.** Fix: a run generation counter (bumped by
  `finishRun`, `markBlocked`, `disable`) is captured by every reply handler of a run; replies of an
  ended run are ignored. `wipeRemote()` refuses (`wipeFinished(false)`) while a run is in progress.
- **R12 — unbounded reads and wrong blob check.** Every sync reply is aborted once more than
  `kMaxResponseBytes` are buffered (as in `CloudClient`); the manifest's `max_blob_bytes` is
  compared with the ciphertext size (what the server limits), not the base64 JSON body.

Not fixed in this round (tracked): R5 (304 path pushes the local export, dropping remote-only
tombstones), R9 (attach runs Argon2 with server params in a nested loop on the GUI thread; empty
vault join is a silent no-op), R11 (hosted AI cancel/duplicate during token refresh), wipe of
plaintext copies that are still shared (`cleanse` detaches), sync key surviving sign-out.

## 11. Gates

Touched preset builds; `rago_unit_tests`; `ragoshell --check-qml`; `node ci/check-i18n.mjs` after
`update_translations`; security review on the diff; re-read diff; then tick §11 5.3.
