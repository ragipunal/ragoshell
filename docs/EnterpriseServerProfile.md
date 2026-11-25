# Enterprise Custom Server Profile & Policy Lock

This document defines the architecture, precedence contract, validation rules, credential-isolation
invariants, UI behavior, and threat model for configurable enterprise cloud server profiles in
RagoShell (`apiUrl`, `portalUrl`, optional `bridgeUrl`, and ES256 entitlement public keyring).

---

## 1. Overview & Goals

- Allow enterprise and self-hosted teams running their own `ragoshell-api`, account portal, and
  optional `ragoshell-web-gateway` instances to point the client at their custom endpoints and
  ES256 (`P-256`) public keyring from **Preferences → Account** or via a read-only system
  **`policy.json`** file, without rebuilding the binary or deploying per-user environment variables.
- Preserve **100% offline local MongoDB operation** (`connections`, SSH/TLS tunnels, shell, schema
  analysis, index advisor, BYOK AI) regardless of cloud server selection or invalid policy state.
- Keep embedded defaults single-sourced from `BrandInfo` (`cmake/RagoShellDefaults.cmake`) and retain
  backward compatibility with existing `RAGOSHELL_*` environment variable overrides.
- Prevent cross-server key confusion, token/sync leakage, `deviceId` tracking correlation, and
  silent telemetry/update leakage back to public SaaS endpoints.

---

## 2. Configuration Schema & Precedence

Two profile modes are supported:

- **`"default"`**: Uses embedded `BrandInfo::apiUrl`, `BrandInfo::portalUrl`, derived hosted bridge
  (`wss://app.<BrandInfo::domain>/ws`), and `BrandInfo::entitlementPubkeys` (unless overridden by
  explicit policy defaults or environment variables).
- **`"custom"`**: Explicitly supplies `apiUrl`, `portalUrl`, optional `bridgeUrl`, and mandatory
  `entitlementPubkeys`.

Effective cloud configuration is resolved in deterministic descending precedence:

1. **In-process C++ test overrides**: `CloudClient::setApiUrlOverride()`,
   `CloudClient::setPortalUrlOverride()`, `CloudClient::setBridgeUrlOverride()`, and
   `CloudClient::setEntitlementVerifier()`.
2. **Locked System Enterprise Policy (`policy.json` with `"locked": true`)**: Cannot be overridden
   by user settings (`ragoshell.json`) or `RAGOSHELL_*` process environment variables.
3. **Process environment variables**: `RAGOSHELL_API_URL`, `RAGOSHELL_PORTAL_URL`,
   `RAGOSHELL_BRIDGE_URL`, and `RAGOSHELL_ENTITLEMENT_PUBKEYS`.
   - **Fail-closed keyring rule**: If `RAGOSHELL_API_URL` differs from `BrandInfo::apiUrl` and
     `RAGOSHELL_ENTITLEMENT_PUBKEYS` is unset or invalid (unless locked/provided at a higher
     precedence), `CloudClient::reloadVerifierFromConfig()` resolves an empty keyring (`0` keys)
     instead of falling back to `BrandInfo::entitlementPubkeys`.
4. **User settings (`ragoshell.json` via `SettingsManager`)**: Stored under the `"cloudServer"`
   object (`mode`, `apiUrl`, `portalUrl`, `bridgeUrl`, `entitlementPubkeys`) once explicitly
   configured by the user:
   - `cloudServer.mode`: `"default"` | `"custom"` (default `"default"`)
   - `cloudServer.apiUrl`: `QString`
   - `cloudServer.portalUrl`: `QString`
   - `cloudServer.bridgeUrl`: `QString` (optional override; empty = derive from portal URL)
   - `cloudServer.entitlementPubkeys`: `QString` (`"<kid>:<base64-spki-der>;..."`)
5. **Unlocked System Enterprise Policy (`policy.json` with `"locked": false`)**: Supplies
   organization defaults when the user has not explicitly configured `"custom"` mode (or has not set
   an explicit user override).
6. **Embedded `BrandInfo` defaults**: `cmake/RagoShellDefaults.cmake`.

Note: Existing URL overrides and environment variables (`RAGOSHELL_API_URL`, `RAGOSHELL_PORTAL_URL`,
`RAGOSHELL_BRIDGE_URL`) remain independent from the air-gap kill switch
`SettingsManager::disableHttpsFeatures()`, which disables cloud sign-in, auto-refresh, hosted
bridge, and update checks globally when enabled.

---

## 3. Read-Only Enterprise Policy File (`policy.json`)

### 3.1 Platform Locations

Policy paths are derived solely from `BrandInfo::productName` / `BrandInfo::productId` (never
hardcoding product literals in C++):

| Platform | System Policy Path |
|---|---|
| Windows | `%ProgramData%\<BrandInfo::productName>\policy.json` |
| macOS | `/Library/Application Support/<BrandInfo::productName>/policy.json` |
| Linux | `/etc/<BrandInfo::productId>/policy.json` |
| iOS / Android / WASM | Not loaded from filesystem (UI configuration only) |

For unit tests, `SettingsManager::setPolicyFilePathForTest(const QString &)` overrides the system
path in-process. There is intentionally **no** production environment variable (such as
`RAGOSHELL_POLICY_FILE`) so an unprivileged user environment cannot spoof an admin-locked policy.

### 3.2 File Reading & Fail-Closed Contract

- Reject symlinks/junctions and non-regular files (FIFOs, directories, device nodes) before reading
  (on POSIX via `O_RDONLY | O_NOFOLLOW | O_NONBLOCK` + `fstat` regular-file verification; on Windows via
  `QFileInfo::isSymLink() || isJunction()` + `isFile()`, where installers must pre-create
  `%ProgramData%\<BrandInfo::productName>` with Administrators-only write ACLs).
- Read in bounded `64 KiB` chunks capped at `65536` bytes (`64 KiB`).
- Supported JSON schema:

```json
{
  "locked": true,
  "disableHttpsFeatures": false,
  "checkForUpdates": false,
  "updateUrl": "https://updates.corp.example/v1/release-check",
  "cloudServer": {
    "mode": "custom",
    "apiUrl": "https://api.corp.example",
    "portalUrl": "https://app.corp.example",
    "bridgeUrl": "wss://bridge.corp.example/ws",
    "entitlementPubkeys": "2026-11:MFkwEwY..."
  }
}
```

- **Fail-closed guarantee**: If a `policy.json` file exists on disk and cannot be opened/read, fails
  the regular-file check, exceeds `64 KiB`, fails JSON parsing, contains invalid types, or fails
  URL/keyring validation under `"locked": true` or `"mode": "custom"`, `SettingsManager` sets
  `policyInvalid = true`. While `policyInvalid` is true, **all** cloud authentication, entitlement
  refresh, sync, hosted bridge, and `UpdateService` network requests are blocked — never silently
  falling back to public `BrandInfo` endpoints.

---

## 4. URL & Keyring Validation Contract

All validation runs deterministically on both `SettingsManager` load/save and `CloudClient`
resolution:

1. **`apiUrl` and `portalUrl`**:
   - Parsed with `QUrl::StrictMode`, trimmed of trailing `'/'`, maximum length `512` characters, no
     control characters or backslashes.
   - Non-empty `host()`, empty `userInfo()`, `!hasQuery()`, `!hasFragment()`, origin-only
     (`path().isEmpty() || path() == "/"`).
   - Scheme must be **`https://`** for all non-loopback hosts, including internal RFC 1918 LAN/VPN
     IPs and private DNS names. Plain **`http://`** is allowed strictly for loopback hosts
     (`127.0.0.1`, `::1`, `localhost`).
2. **`bridgeUrl`** (optional):
   - When non-empty: `QUrl::StrictMode`, maximum `512` characters, no control characters or
     backslashes, non-empty `host()`, empty `userInfo()`, `!hasQuery()`, `!hasFragment()`, valid path
     without `..` or `//`.
   - Scheme must be **`wss://`** for non-loopback hosts; **`ws://`** is permitted strictly for
     loopback hosts.
   - When empty: `CloudClient::deriveBridgeUrlFromPortal(effectivePortalUrl())` deterministically
     maps `https://host[:port]` → `wss://host[:port]/ws` and loopback `http://host[:port]` →
     `ws://host[:port]/ws` (falling back to `wss://app.<BrandInfo::domain>/ws` only in default mode
     with default portal URL).
3. **`entitlementPubkeys`**:
   - Required when effective mode is `"custom"`.
   - Parsed with `EntitlementVerifier::fromKeyRingString()` (`1..16` valid P-256 SPKI DER keys,
     maximum `8192` characters total, unique valid `kid`s).
   - `"custom"` mode never falls back to `BrandInfo::entitlementPubkeys`; an invalid or empty ring
     keeps `verifier.keyCount() == 0` so offline/online tokens fail verification cleanly.
   - Note: HTTP requests continue setting `MaximumRedirectsAllowed = 0` (`CloudClient::buildApiRequest`),
     and RFC 8628 `verification_uri` / `verification_uri_complete` continue checking exact host+port
     match against `effectivePortalUrl()` via `CloudClient::isAllowedVerificationUrl()`.

---

## 5. State Isolation, Credential Wipe & Server Switching

Each resolved effective cloud profile produces a deterministic 16-hex-char fingerprint stored in
`SettingsManager` cache under `cloudProfileFingerprint`:

```text
fingerprint = sha256_hex_16(
    effectiveApiUrl + "\n" +
    effectivePortalUrl + "\n" +
    effectiveBridgeUrl + "\n" +
    canonicalSortedKeyIdsAndSpkiBase64
)
```

- **Interactive server switch in Preferences (while signed in)**:
  - Prompt the user for confirmation before applying the switch.
  - Trigger `CloudClient::signOut()` against the active server first (respecting
    `!disableHttpsFeatures()`), then apply the new profile settings, save `SettingsManager`, reset
    local sync state, and store the updated `cloudProfileFingerprint`.
- **Startup / offline profile mismatch**:
  - In `CloudClient::restoreOfflineSession()`, compare the previously recorded
    `cloudProfileFingerprint` against the newly resolved fingerprint. (If transitioning an existing
    legacy signed-in session that had no recorded fingerprint, bind it once to the default SaaS
    profile fingerprint; if the active profile already differs from default SaaS, treat it as a
    mismatch.)
  - On mismatch, perform an **immediate, strictly local atomic wipe** with **zero network traffic**
    to either the old or new server:
    1. Remove `refresh_token`, `entitlement_jwt`, and `sync_master_key` from
       `ICloudCredentialStore` (`KeyStore`).
    2. Clear `CloudSessionMetadata` (`cloudSignedIn = false`, `cloudUserId`, `cloudUserEmail`, and
       explicitly clear **`cloudDeviceId`** so the next sign-in on the new server mints a fresh
       UUID v4 instead of linking devices across servers).
    3. Call `SyncEngine::resetLocalSyncStateForServerSwitch()` to stop timers, bump
       `m_runGeneration`, disable `syncEnabled` / clear `syncKeyMode`, and clear `sync-state.json`
       ETags, KDF params, and entry fingerprints.
    4. Preserve all local MongoDB connections, SSH/TLS settings, snippets, preferences, and local
       query history untouched.

---

## 6. Subsystem Changes (Implementation Steps)

1. **Step A — `SettingsManager` (`src/ragoshell/core/settings/SettingsManager.{h,cpp}`)**:
   - Add `SettingsManager::CloudServerConfig` (`mode`, `apiUrl`, `portalUrl`, `bridgeUrl`,
     `entitlementPubkeys`) with `userCloudServerConfig()`, `setUserCloudServerConfig()`, and
     `clearUserCloudServerConfig()` serialized under `"cloudServer"` in `ragoshell.json`.
   - Load and validate platform `policy.json` (or `setPolicyFilePathForTest` path) using bounded
     regular-file reads; expose `hasEnterprisePolicy()`, `isEnterprisePolicyLocked()`,
     `isEnterprisePolicyInvalid()`, `isCloudServerPolicyLocked()`, and policy overrides
     (`policyDisableHttpsFeatures()`, `policyCheckForUpdates()`, `policyUpdateUrl()`).
2. **Step B — `CloudClient` & `SyncEngine` (`src/ragoshell/core/cloud/`)**:
   - Implement the 6-tier precedence rules in `effectiveApiUrl()`, `effectivePortalUrl()`, new
     `effectiveBridgeUrl()`, `effectiveServerMode()`, and `reloadVerifierFromConfig()`.
   - Enforce `policyInvalid` and `disableHttpsFeatures` gating across all network paths (`startDeviceSignIn`,
     `refreshAccountAndEntitlements`, `withValidAccessToken`, `checkEntitlementRefresh`,
     `signOut` remote call, `SyncEngine::syncNow`/`wipeRemote`).
   - Implement `CloudClient::deriveBridgeUrlFromPortal()` and `cloudProfileFingerprint` comparison +
     atomic local wipe on mismatch, wired cleanly to `SyncEngine::resetLocalSyncStateForServerSwitch()`.
   - Guard `SyncEngine::buildRequest()` / `CloudClient::buildApiRequest()` against empty/invalid URLs.
3. **Step C — Hosted Bridge Routing (`QmlBackend.cpp`, `QmlBackendUtils.cpp`, `QmlBackendInternal.h`, `MongoWorker.cpp`)**:
   - In `QmlBackend::openConnection()` and `QmlBackendUtils::testConnectionNow()`, when
     `clone->transportKind() == "hosted"`, resolve `const QString bridgeUrl = cloud->effectiveBridgeUrl()`
     and validate it (`wss://` or loopback `ws://`); pass `bridgeUrl.toStdString()` as the resolved
     transport URI (`MongoWorker.cpp` can retain `"hosted"` fallback using `BrandInfo::domain` only
     if a direct core caller bypasses `QmlBackend`, or accept an explicit bridge URL directly).
   - Update `QmlBackendDetail::networkHosts()` in `QmlBackendInternal.h` so `transportKind == "hosted"`
     (and custom `ws://`/`wss://` gateways) include the resolved gateway host for iOS local-network
     permission checks instead of only probing the raw Mongo host directly.
4. **Step D — `UpdateService` (`src/ragoshell-app/UpdateService.{h,cpp}`, `UpdatePulse.{h,cpp}`)**:
   - Suppress automatic and manual `startCheck()` calls when `SettingsManager::isEnterprisePolicyInvalid()`
     or policy sets `"checkForUpdates": false`.
   - When a custom cloud server profile is active (mode `"custom"` or `RAGOSHELL_API_URL` differs
     from `BrandInfo::apiUrl`), suppress requests to default `BrandInfo::apiUrl` (`/v1/release-check`
     and `BrandInfo::releasesManifestUrl`) unless an explicit `updateUrl` is supplied via
     `policy.json` (`updateUrl`) or `RAGOSHELL_UPDATE_URL` (`RAGOSHELL_RELEASES_MANIFEST_URL`).
5. **Step E — `CloudService`, UI & Translations (`CloudService.{h,cpp}`, `PreferencesAccountSection.qml`, `ConnectionDialog.qml`, `i18n/*.ts`)**:
   - Expose read-only and editable server profile properties, validation feedback, `policyLocked`,
     `policyInvalid`, `isCustomServerActive`, and confirmation-aware `applyServerProfile(...)` via
     `CloudService`.
   - Add a **Cloud Server** configuration card in `PreferencesAccountSection.qml`:
     - Responsive `GridLayout` (`columns: root.singleColumn ? 1 : 2`, single column below `520 dp`),
       `Layout.minimumWidth: 0`, sizes via `Theme` / `UiDensity`.
     - Displays `"Managed by organization"` (`RagoBadge`) and disables user inputs when locked by
       `policy.json`, or surfaces a clear fail-closed error banner when `policyInvalid` is true.
     - Displays custom endpoint host clearly (using `Brand.productName` placeholder only for the
       product default) in `ConnectionDialog.qml` and account status summaries.
   - Run `cmake --build --preset <preset> --target update_translations` and `node ci/check-i18n.mjs`
     for all new `qsTr()` / `tr()` strings across the 15 translation catalogs.

---

## 7. Threat Model & Edge Cases

| ID | Threat / Failure Mode | Mitigation |
|---|---|---|
| **T1** | Cross-server ES256 key/issuer confusion: public SaaS keys verify a custom server's JWT (or vice versa) | Custom mode requires an explicit `entitlementPubkeys` ring (never falls back to `BrandInfo::entitlementPubkeys`); env `RAGOSHELL_API_URL` override without `RAGOSHELL_ENTITLEMENT_PUBKEYS` yields `0` keys; `EntitlementVerifier` enforces exact `iss == effectiveApiUrl()` match. |
| **T2** | Credential & vault leakage across server switch (`refresh_token`, `entitlement_jwt`, `sync_master_key`, `sync-state.json`) | Interactive switch runs `signOut()` + confirmation; silent/policy mismatch triggers immediate offline wipe of all 3 `KeyStore` slots and `sync-state.json` with zero network traffic. |
| **T3** | Cross-server user tracking via preserved `cloudDeviceId` UUID | `cloudDeviceId` is cleared alongside `cloudSignedIn`/`cloudUserId` on profile fingerprint mismatch and interactive switch so the new server receives a fresh UUID v4. |
| **T4** | Plaintext `http://` or `ws://` downgrade on corporate LAN/VPN, embedded `userInfo`, or HTTP 30x redirect SSRF | URL validator enforces `https://` / `wss://` for all non-loopback hosts (RFC 1918 included), rejects `userInfo`/queries/fragments/paths on API+portal, and `buildApiRequest()` keeps `MaximumRedirectsAllowed = 0`. |
| **T5** | Unprivileged `RAGOSHELL_*` env vars or user-edited `ragoshell.json` bypassing admin-locked `policy.json` | Precedence tier 2 (`policy.json` with `locked: true`) outranks both env vars (tier 3) and `ragoshell.json` (tier 4); no env var can relocate `policy.json` in production. |
| **T6** | Malformed or corrupted `policy.json` causing silent fallback to public SaaS `api.ragoshell.com.tr` | Any present `policy.json` that fails file, parse, size, or field validation sets `policyInvalid = true` and fails closed across all cloud, bridge, sync, and update code paths. |
| **T7** | `UpdateService` leaking enterprise client launches/IPs to public `/v1/release-check` | Custom cloud profile disables default `BrandInfo::apiUrl` / `BrandInfo::releasesManifestUrl` update checks unless an internal `updateUrl` is explicitly provided by policy or env. |
| **T8** | Local DoS via symlink/FIFO/huge `policy.json` file | Policy loader verifies regular non-symlink/non-junction file and reads bounded `64 KiB` chunks capped at `65536` bytes. |
| **T9** | Brand-spelling or domain hardcoding regressions | Policy paths use `BrandInfo::productName` / `BrandInfo::productId`; default strings stay solely in `cmake/RagoShellDefaults.cmake` and `Brand` QML singleton (`BrandInfo_test` remains green). |

---

## 8. Verification & Gate Plan

When implementing Steps A–E, add automated unit tests covering:

- **`SettingsManager_test.cpp`**: Server profile round-trip, default vs custom validation (`https://`
  enforcement on RFC 1918 LAN IPs, loopback `http://`, rejecting `userInfo`/paths/queries), locked
  and unlocked `policy.json` precedence, symlink/oversized/malformed `policy.json` fail-closed state.
- **`CloudClient_test.cpp`**: 6-tier precedence resolution (locked policy > env > user settings >
  unlocked policy > defaults), `RAGOSHELL_API_URL` without pubkeys yielding 0 keys,
  `deriveBridgeUrlFromPortal()`, `isAllowedBridgeUrl()`, `policyInvalid` blocking sign-in/refresh,
  and `cloudProfileFingerprint` mismatch wiping all 3 credentials + `cloudDeviceId` cleanly offline.
- **`SyncEngine_test.cpp`**: `resetLocalSyncStateForServerSwitch()` clears `sync-state.json`, master
  key, and disables sync while preserving local connections/history.

Mandatory repository gates before marking implementation complete:

1. `cmake --build --preset <touched-preset>` (including `update_translations` when `qsTr()`/`tr()` changes)
2. `rago_unit_tests` (100% pass for non-live-DB tests)
3. `ragoshell --check-qml` (exit code `0`)
4. `node ci/check-i18n.mjs` (exit code `0` across all 15 language catalogs)
5. Pure LF (`\n`, zero `\r`) verification on all touched files + post-implementation diff security review
