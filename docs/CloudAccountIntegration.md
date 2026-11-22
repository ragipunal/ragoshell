# Cloud Account Integration, Signed Update Verification & About Dialog (`Faz 5.1` — `A1`–`A4`)

This document defines the architecture, wire contracts, state machines, threat model, and security
verification plan for Roadmap Phase `5.1` (`docs/Roadmap.md` §3.4, §4.6, §6 `A1`–`A4`, §7 Faz 5 `5.1`, §12.1):

- **`A1` — `CloudClient` & `CloudService`**: RFC 8628 OAuth 2.0 Device Authorization Grant, rotating refresh
  tokens in OS `KeyStore`, offline-verifiable ES256 `entitlement+jwt` caching, and single-source brand URLs.
- **`A2` — Account Preferences (`PreferencesAccountSection.qml`)**: Optional sign-in UX, device code display,
  plan/entitlement summary, store-compliant external portal links, and clean sign-out.
- **`A3` — Signed Update Verification & Store Build Guard (`Minisign`, `UpdateService`)**: Pure OpenSSL 3
  Ed25519 + BLAKE2b-512 Minisign verification, 24-hour automatic check throttle, and `RAGO_STORE_BUILD`.
- **`A4` — About Dialog & Installer Metadata (`AboutDialog.qml`, Windows NSIS/MSI)**: Accessible single-source
  product/privacy/source/license/donate links and installer `URLInfoAbout` alignment.

---

## 1. Core Principles & Privacy Invariants

1. **Zero unsolicited network traffic (`Roadmap.md` §3.4)**:
   - Local MongoDB features (connections, SSH, TLS, replica sets, shell, schema analyzer, index advisor,
     collection compare, import/export, BYOK AI) work **100% offline and unsigned-in** forever.
   - `CloudClient` makes **zero network calls on startup or in the background** when unsigned-in.
   - When signed in, startup restores cached identity metadata and verifies the cached `entitlement+jwt`
     purely offline via `EntitlementVerifier` (OpenSSL ES256) without contacting `api.<domain>` until the
     user clicks **Refresh Status** or invokes a cloud feature (`Faz 5.2`–`5.4`).
2. **OS Keychain isolation (`KeyStore` only)**:
   - `refresh_token` and cached `entitlement+jwt` are stored **exclusively** via `ICloudCredentialStore`
     (backed in production by `KeyStore` — macOS Keychain, Windows DPAPI/CredWrite, Linux libsecret,
     iOS Keychain, Android Keystore).
   - `access_token` (15-minute lifetime) is held **in RAM only** and cleansed (`OPENSSL_cleanse`) on sign-out
     or destruction.
   - `ragoshell.json` NEVER stores `access_token`, `refresh_token`, `device_code`, or raw JWTs; only
     non-secret display metadata (`cloudSignedIn`, `cloudDeviceId`, `cloudDeviceName`, `cloudUserId`,
     `cloudUserEmail`) is persisted in settings cache data.
3. **Single-source brand configuration (`docs/Brand.md`)**:
   - Domain and product URLs are derived once in `cmake/RagoShellDefaults.cmake` -> `BrandInfo.h` ->
     `BrandBridge` (`Brand` QML singleton). No URL or product name literal is hardcoded in C++ or QML.
4. **App Store / Play Store compliance (`Roadmap.md` §12.1)**:
   - When `Platform.isMobile` (`SYSTEM_IOS` or `SYSTEM_ANDROID`) is true, external purchase/billing
     and donation links (`Manage Account`, `Sponsor / Support`) are hidden from QML views.
   - When built with `-DRAGO_STORE_BUILD=ON`, `UpdateService` is inert (never phones home, hides self-update
     controls in `PreferencesDialog`, `AboutDialog`, and `MandatoryUpdateDialog`).

---

## 2. Step `A1`: Brand URLs, `CloudClient` (`rago-core`) & `CloudService` (`ragoshell-app`)

### 2.1 Single-Source Brand Constants & Environment Overrides

`cmake/RagoShellDefaults.cmake` derives (all overridable via `-DRAGO_*=...`):

| CMake Variable | Default Derivation | `BrandInfo` (`std::string_view` / `bool`) | `Brand` QML Property |
|---|---|---|---|
| `RAGO_API_URL` | `https://api.${RAGO_DOMAIN}` | `BrandInfo::apiUrl` | `Brand.apiUrl` |
| `RAGO_PORTAL_URL` | `https://app.${RAGO_DOMAIN}` | `BrandInfo::portalUrl` | `Brand.portalUrl` |
| `RAGO_RELEASES_MANIFEST_URL` | `https://dl.${RAGO_DOMAIN}/releases.json` | `BrandInfo::releasesManifestUrl` | — |
| `RAGO_PRIVACY_URL` | `${RAGO_PRODUCT_URL}/legal/privacy/` | `BrandInfo::privacyUrl` | `Brand.privacyUrl` |
| `RAGO_TERMS_URL` | `${RAGO_PRODUCT_URL}/legal/terms/` | `BrandInfo::termsUrl` | `Brand.termsUrl` |
| `RAGO_SOURCE_REPO_URL` | `https://github.com/${RAGO_GITHUB_OWNER}/${RAGO_PRODUCT_ID}` | `BrandInfo::sourceRepoUrl` | `Brand.sourceRepoUrl` |
| `RAGO_MINISIGN_PUBKEY` | `""` (until human item `0.7`) | `BrandInfo::minisignPubkey` | — |
| `RAGO_STORE_BUILD` | `OFF` | `BrandInfo::isStoreBuild` | `Brand.isStoreBuild` |

Runtime environment overrides (resolved via `productEnv(...)`, i.e., `RAGOSHELL_<NAME>`):
- `RAGOSHELL_API_URL`: overrides `BrandInfo::apiUrl` (validated: `https://` in general, or loopback
  `http://127.0.0.1:<port>`, `http://localhost:<port>`, `http://[::1]:<port>` solely for local dev/testing).
- `RAGOSHELL_ENTITLEMENT_PUBKEYS`: overrides `BrandInfo::entitlementPubkeys` (`kid:base64,...` or JWKS JSON).
- `RAGOSHELL_MINISIGN_PUBKEY`: overrides `BrandInfo::minisignPubkey`.
- `RAGOSHELL_RELEASES_MANIFEST_URL`: overrides `BrandInfo::releasesManifestUrl`.

### 2.2 Wire Contract with `ragoshell-api` (`openapi/openapi.yaml`)

All requests enforce:
- `15,000 ms` transfer timeout (`QNetworkRequest::setTransferTimeout(15000)`).
- Strict `64 KiB` (`65,536` bytes) response body cap (replies exceeding cap are aborted immediately).
- Redirects disabled (`QNetworkRequest::NoLessSafeRedirectPolicy` with maxRedirects = `0` for auth API calls
  so `Authorization` headers and `refresh_token` payloads are never forwarded across origins).
- Strict JSON serialization with no extraneous fields (`ragoshell-api` uses `json.Decoder.DisallowUnknownFields()`).
- Dual error parser supporting both `ragoshell-api`'s `ErrorEnvelope`
  (`{"error": {"code": "...", "message": "..."}}`) and flat RFC 8628 OAuth responses
  (`{"error": "...", "error_description": "..."}`).

Endpoints consumed:
1. `POST /v1/auth/device/code`
   - Request: `{"device_name": "<1..128 chars>", "platform": "<macos|windows|linux|ios|android|wasm>", "arch": "<arch>", "app_version": "<ver>"}`
   - Response `200`: `{"device_code", "user_code", "verification_uri", "verification_uri_complete", "expires_in", "interval"}`
2. `POST /v1/auth/device/token`
   - Request: `{"grant_type": "urn:ietf:params:oauth:grant-type:device_code", "device_code": "<code >"}`
   - Response `200`: `{"access_token", "refresh_token", "token_type": "Bearer", "expires_in": 900, "device_id": "<uuid>"}`
   - Response `400`:
     - `authorization_pending` -> keep polling at current `interval` (clamped `>= 1s`, default `5s`; configurable in tests).
     - `slow_down` -> increase poll interval by `+5s` per RFC 8628 §3.5 and continue polling.
     - `access_denied` -> stop polling, transition to `SignedOut` with user-visible denial message.
     - `expired_token` -> stop polling, transition to `SignedOut` with code-expired message.
     - `429` -> back off by `Retry-After` (clamped `5s..120s`).
3. `POST /v1/auth/refresh`
   - Request: `{"grant_type": "refresh_token", "refresh_token": "<token>"}`
   - Response `200`: rotated `TokenPairResponse` -> atomically persist new `refresh_token` into `KeyStore`,
     update RAM `access_token`, resolve all queued single-flight callers.
   - Response `400` / `401` (`invalid_grant`, `unauthorized`) -> token revoked/replayed; wipe cloud secrets
     from `KeyStore`, clear session metadata, transition cleanly to `SignedOut` (`Free` plan) without
     touching local MongoDB connections or scripts.
   - Network / `5xx` error -> preserve `refresh_token` and cached `entitlement+jwt` so offline grace continues!
4. `GET /v1/me`
   - Header: `Authorization: Bearer <access_token>`
   - Response `200`: `{"id", "email", "locale", "emailVerified", "totpEnabled", "plan", "createdAt"}`
5. `GET /v1/me/entitlements`
   - Header: `Authorization: Bearer <access_token>`
   - Response `200`: `{"token": "<entitlement+jwt>", "kid", "plan", "status", ...}`
   - `CloudClient` verifies `token` cryptographically via `EntitlementVerifier::verify` with:
     - `expectedIssuer = effectiveApiUrl` (without trailing slash)
     - `expectedDeviceId = deviceId`
     - `expectedUserId = userId` (when already known from `/v1/me`)
   - Only claims extracted from a **verified** `entitlement+jwt` signature are trusted to unlock paid
     capabilities or persist into `KeyStore` (`cloud/entitlement_jwt`). Unsigned JSON fields outside `token`
     are never trusted on their own if `EntitlementVerifier` has keys configured; if no public keys are
     compiled/configured yet (pre-`0.7` local dev without keys), `CloudClient` rejects unverified paid
     entitlement tokens and stays on `Free` unless valid verification keys are supplied.
6. `POST /v1/auth/logout`
   - Request: `{"refresh_token": "<token>"}` **without** `Authorization` header when `refresh_token` is
     available (because `ragoshell-api` checks `Authorization` first and would reject an expired 15-minute
     access token with `401` before revoking the refresh token family; fallback to `Authorization: Bearer`
     only if no `refresh_token` exists).
   - Regardless of network outcome (offline, timeout, `5xx`), `signOut()` unconditionally deletes
     `refresh_token` and `entitlement_jwt` from `KeyStore`, cleanses RAM buffers, and resets local cloud
     metadata to `SignedOut`.

### 2.3 Verification URI Validation

Before exposing or opening `verification_uri` / `verification_uri_complete` via `QDesktopServices::openUrl`:
- Reject any URL exceeding `512` characters, containing control characters, userinfo (`user:pass@`), or
  invalid syntax.
- When `effectiveApiUrl` uses `https://`: require `scheme == "https"` and `host` equal (case-insensitive)
  to the configured portal host (`QUrl(BrandInfo::portalUrl).host()`, or override `RAGOSHELL_PORTAL_URL`).
- When `effectiveApiUrl` uses loopback `http://127.0.0.1` / `localhost` / `[::1]`: allow `http` on that
  same loopback host or the configured portal host.
- All other schemes (`javascript:`, `file:`, `data:`, `custom://`, external domains) are rejected.

### 2.4 State Machine & Single-Flight Refresh

```
  [SignedOut] ──startDeviceSignIn()──> [RequestingCode]
       ▲                                      │
       │ cancel / denied / expired / error    ▼
       ├────────────────────────────── [AwaitingBrowserApproval] (userCode + countdown + poll timer)
       │                                      │
       │ signOut() / 401 invalid_grant        ▼ token issued -> fetch /v1/me + /v1/me/entitlements
       └────────────────────────────── [SignedIn] (OnlineFresh | OfflineGrace | FreeFallback)
```

**Single-flight refresh**: `CloudClient::withValidAccessToken(callback)` checks whether RAM `access_token` is
present and has `> 30s` remaining before expiry. If expired or missing, it enqueues `callback` into
`m_pendingTokenWaiters` and starts **at most one** `POST /v1/auth/refresh` request. When the refresh
finishes, all queued callbacks receive the fresh `access_token` (or failure error) in order, preventing
concurrent callers from racing on single-use rotating refresh tokens and triggering reuse-detection family
revocation on the server.

### 2.5 `ICloudCredentialStore` & Profile Isolation

```cpp
class ICloudCredentialStore {
public:
    virtual ~ICloudCredentialStore() = default;
    virtual bool isAvailable() const = 0;
    virtual bool readSecret(const QString &slot, QString *valueOut, QString *errorOut = nullptr) = 0;
    virtual bool writeSecret(const QString &slot, const QString &value, QString *errorOut = nullptr) = 0;
    virtual bool removeSecret(const QString &slot, QString *errorOut = nullptr) = 0;
};
```
- Production `KeyStoreCloudCredentialStore` wraps `KeyStore` with account prefix `cloud/` (scoped by a
  16-hex SHA-256 prefix of `appDataDir()` when `appDataDirOverridden()` is true, matching
  `SettingsManager::encryptionKeyAccount()` so isolated test/debug profiles never clobber the user's real
  cloud login).
- Unit tests inject `InMemoryCloudCredentialStore`, guaranteeing zero macOS Keychain authorization dialogs
  in `rago_unit_tests`.
- If `KeyStore::isAvailable()` is false (e.g., minimal headless Linux container without `libsecret`),
  `CloudClient` reports a clear warning and keeps tokens strictly in RAM for the current process lifetime
  only — **never** falling back to plaintext storage in `ragoshell.json`.

---

## 3. Step `A2`: Account Preferences UI (`PreferencesAccountSection.qml`)

Integrated as a dedicated section inside `PreferencesDialog.qml` (`Account` section with quick-jump header
or top-level section consistent with existing `PreferencesDialog.qml` cards):

- **Responsive layout rules (`.agents/rules/qt-app-code.md`)**:
  - Column count (`1` vs `2`) decided strictly by **section width** (`root.width < 520 ? 1 : 2`), never
    `Window.width`.
  - Every `StackLayout` child specifies `Layout.minimumWidth: 0` and `Layout.fillWidth: true`.
  - Colors, radii, and typography come exclusively from `Theme` / `UiDensity`.
  - QML JS uses ES2016 syntax only.
- **Signed-Out State**:
  - Informative card explaining that an account is **completely optional** — local MongoDB connections,
    shell, schema/index tools, and BYOK AI are free without signing in.
  - Editable **Device Name** `RagoTextField` (defaults to `QSysInfo::machineHostName()`, sanitized to
    `1..64` printable chars) + **Sign In** primary `RagoPillButton`.
- **Pending Device Code State (`AwaitingBrowserApproval`)**:
  - Large monospace `user_code` badge (`XXXX-XXXX`), **Copy Code** button, **Open Browser** button
    (opens validated `verification_uri_complete`), live expiry countdown (`mm:ss`), and **Cancel** button.
- **Signed-In State**:
  - Account email, device name + short `device_id` prefix (`first 8 chars`), **Plan** badge (`Free`,
    `Pro`, `Team`) and **Status** badge (`Active`, `Trial`, `Offline grace`, `Past due`, `Canceled`).
  - Feature summary row: Encrypted Sync (`Enabled` / `Not included`) and entitlement validity / offline
    grace timestamp. (The hosted AI quota row was removed: the AI assistant is bring-your-own-key only.)
  - Actions: **Refresh Status**, **Manage Account** (`!Platform.isMobile`, opens `Brand.portalUrl`), and
    **Sign Out**.

---

## 4. Step `A3`: Minisign Verifier, 24h Update Throttle & `RAGO_STORE_BUILD`

### 4.1 Pure OpenSSL 3 Minisign Verifier (`src/ragoshell/core/utils/Minisign.{h,cpp}`)

Matches `ci/lib/minisign.mjs` and `docs/ReleasePipeline.md` §3 & §6 byte-for-byte with zero external
dependencies beyond `OpenSSL::Crypto` (already linked by `rago-core`):

1. **Public Key Parsing (`Minisign::parsePublicKey`)**:
   - Accepts 1-line raw Base64 (`RW...`) or 2-line `.pub` (`untrusted comment: ...\nRW...`).
   - Strict Base64 decode -> exact `42` bytes:
     - `[0..1] == "Ed"` (`0x45, 0x64`)
     - `[2..9]` = 8-byte `keyId`
     - `[10..41]` = 32-byte Ed25519 raw public key
2. **Signature Parsing (`Minisign::parseSignature`)**:
   - Parses 4-line `.minisig` (`\r\n` normalized to `\n`; rejects extra non-empty lines or NUL/control bytes):
     - Line 0: `untrusted comment: ...`
     - Line 1: Base64 -> exact `74` bytes: `[0..1]` (`"ED"` prehashed BLAKE2b-512 or `"Ed"` legacy raw),
       `[2..9]` 8-byte `keyId`, `[10..73]` 64-byte `rawSig`.
     - Line 2: `trusted comment: <payload>` (exact 17-byte prefix `"trusted comment: "`).
     - Line 3: Base64 -> exact `64` bytes `globalSig`.
3. **Cryptographic Verification (`Minisign::verify`)**:
   - RAII `OpenSslErrorScope` clears thread-local OpenSSL error stack on entry and exit.
   - Constant-time `keyId` comparison: `CRYPTO_memcmp(pub.keyId.data(), sig.keyId.data(), 8) == 0`.
   - Constructs `EVP_PKEY` via `EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.publicKey.data(), 32)`.
   - **File signature check**:
     - If `"ED"`: computes 64-byte `EVP_blake2b512()` digest over `message`, then verifies `rawSig` over
       that 64-byte digest with `EVP_DigestVerifyInit` + `EVP_DigestVerify`.
     - If `"Ed"`: verifies `rawSig` directly over `message` when `options.allowLegacyUnprehashed == true`
       (default `true`; callers can set `false` to require prehashed `"ED"`).
   - **Global signature check (mandatory)**:
     - Concatenates `rawSig (64 bytes) || trustedCommentPayloadUtf8` and verifies `globalSig` via pure
       Ed25519 `EVP_DigestVerify` using the same `EVP_PKEY`. This makes `trusted comment` cryptographically
       immutable!
   - **Trusted comment validation**:
     - Parses tab-delimited `timestamp:<sec>`, `file:<name>`, `release:<semver>` tokens.
     - Enforces optional `expectedFile`, `expectedRelease`, `maxFutureSkewSec`, and `maxAgeSec`.

### 4.2 Signed Manifest Verification in `UpdateService`

- Effective Minisign public key: `productEnv("MINISIGN_PUBKEY")` if non-empty, else `BrandInfo::minisignPubkey`.
- When effective pubkey is **empty** (pre-`0.7` before release signing key generation): `UpdateService`
  behaves identically to Phase 3 (`POST /v1/release-check` validated via `UpdatePulse::parseReleaseCheckResponse`).
- When effective pubkey is **non-empty**:
  - Whenever an update check returns `updateAvailable == true` or `mandatory == true` (or when checking a
    direct `releases.json` URL), `UpdateService` fetches the raw `releases.json` bytes (up to `64 KiB`) and
    `releases.json.minisig` (up to `4 KiB`) from `effectiveReleasesManifestUrl()` (`RAGOSHELL_RELEASES_MANIFEST_URL`
    or `BrandInfo::releasesManifestUrl`, or `<endpoint>` + `<endpoint>.minisig` if endpoint ends with `.json`).
  - `UpdateService` runs `Minisign::verify` requiring:
    - Cryptographically valid Ed25519 file signature + `global_sig` under `effectiveMinisignPubkey`.
    - `UpdatePulse::parseReleaseCheckResponse` succeeds on the **verified raw `releases.json` bytes**.
    - Trusted comment `file:` (if present, required for production manifests) equals `"releases.json"`, and
      `release:` (if present) equals `verifiedManifest.latestVersion`.
  - Only the `ReleaseCheckResult` parsed from those **verified raw bytes** is applied to `m_result`
    (`updateAvailable`, `mandatory`, `downloadUrl`, `latestVersion`, `releaseNotes`). If verification fails,
    `UpdateService` logs a security warning, surfaces a clean status message on manual checks, and **refuses**
    to set `updateAvailable = true` or `mandatory = true`!

### 4.3 24-Hour Automatic Check Throttle & `RAGO_STORE_BUILD`

- `kAutoCheckIntervalSec` in `UpdateService.cpp` is updated from `12 * 3600` to `24 * 3600` (`86,400` seconds).
- Automatic checks read/write `lastAutoUpdateCheckUtc` (epoch seconds, falling back to legacy
  `lastUpdateCheckEpochSec` on upgrade) and respect `SettingsManager::checkForUpdates()`.
- Manual checks (`AboutDialog` -> `Check for Updates`, i.e., `UpdateService::checkNow()`) **always** execute
  immediately regardless of `lastAutoUpdateCheckUtc`, while recording `lastUpdateCheckEpochSec` without
  blocking manual re-checks.
- `SettingsManager::shareAnonymousUsage()` default is untouched (deferred to `0.9` `(insan)`).
- When `BrandInfo::isStoreBuild` (`-DRAGO_STORE_BUILD=ON`) is true:
  - `UpdateService::scheduleStartupCheck()` and `UpdateService::checkNow()` are immediate no-ops (zero network
    requests).
  - `PreferencesDialog.qml` hides the `UPDATES & USAGE` self-update card (or self-update toggle),
    `AboutDialog.qml` hides the `Check for Updates` panel, and `MandatoryUpdateDialog.qml` stays hidden
    (`visible: !Brand.isStoreBuild && UpdateService.mandatory`).

---

## 5. Step `A4`: `AboutDialog.qml` & Installer Metadata

1. **`AboutDialog.qml`**:
   - Displays product icon, `Brand.productName`, version/platform/Qt runtime, localized summary, and a
     clean, keyboard-accessible link group reading exclusively from `Brand`:
     - **Website** (`Brand.productUrl`)
     - **Privacy Policy** (`Brand.privacyUrl`)
     - **Open-Source Licenses** (opens `LicensesDialog` via signal `openLicensesRequested()`)
     - **Source Code** (`Brand.sourceRepoUrl`)
     - **Support / Donate** (`Brand.donateUrl`, visible only when `!Platform.isMobile && Brand.donateUrl.length > 0`)
   - Keeps Robomongo / Paralect / 3T attribution notice (`Brand.md` §1) intact.
   - Hides self-update check card when `Brand.isStoreBuild` is true.
2. **Windows & Linux Installer URLs**:
   - `install/windows/ragoshell.wxs` already sets `ARPURLINFOABOUT` from `$(var.ProductUrl)` (`RAGO_PRODUCT_URL`).
   - `install/windows/ragoshell.nsi` & `ci/build-windows.ps1`: pass `/DAPP_URL=$productUrl` from
     `RAGO_PRODUCT_URL` and write `URLInfoAbout` under `Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}`
     so NSIS Add/Remove Programs matches MSI.
   - `install/linux/tr.com.ragoshell.app.metainfo.xml` already uses `https://ragoshell.com.tr` (verified).

---

## 6. Threat Model & Mitigations

| # | Threat | Vector | Mitigation |
|---|---|---|---|
| T1 | **Phishing / Open-Redirect via `verification_uri`** | Compromised or MITM dev endpoint returns `javascript:...`, `file:///...`, or `https://evil.example/device` | Strict `QUrl` validation in `CloudClient::isAllowedVerificationUrl`: `https://` only (loopback `http://` only when API URL is loopback), exact host match against `BrandInfo::portalUrl`, no userinfo, max 512 chars. |
| T2 | **Token leakage into plaintext config or logs** | Writing `refresh_token` / `access_token` into `ragoshell.json` or `qWarning()` | Secrets stored strictly via `ICloudCredentialStore` (`KeyStore`); `access_token` RAM-only + `OPENSSL_cleanse`; never logged; `CloudClient_test` asserts `ragoshell.json` contains zero token substrings. |
| T3 | **Forged paid entitlement response** | Compromised CDN/proxy returns `{"plan": "pro", "features": {"sync": true}}` without valid signature | `CloudClient` verifies `token` (`entitlement+jwt`) via `EntitlementVerifier` (OpenSSL P-256 ES256) bound to `iss == apiUrl` and `did == deviceId`; unsigned outer JSON fields never grant paid features. |
| T4 | **Refresh token race / reuse revocation** | Sync + AI + UI trigger simultaneous token refreshes on an expired `access_token`, sending the same single-use `refresh_token` twice | Single-flight queue (`m_refreshInFlight` + `m_pendingTokenWaiters`) ensures only one `/v1/auth/refresh` runs at a time; rotated `refresh_token` is written to `KeyStore` before waking waiters. |
| T5 | **Tampered update manifest / fake mandatory update** | Compromised `/v1/release-check` or mirror serves malicious `downloadUrl` or bogus `mandatory: true` | When `minisignPubkey` is configured, `UpdateService` fetches raw `releases.json` + `.minisig`, verifies Ed25519 + BLAKE2b-512 + `global_sig` + `trusted comment` (`file:releases.json`, `release:<ver>`), and parses `downloadUrl`/`mandatory` solely from the verified bytes. |
| T6 | **Redirect stripping or leaking `Authorization` header** | `301`/`302`/`307` redirect on API endpoint forwards Bearer token to third-party host | Redirects disabled (`maxRedirects = 0`) on `CloudClient` requests; HTTPS enforced for non-loopback endpoints. |
| T7 | **Memory exhaustion via oversized HTTP response** | Rogue server streams multi-gigabyte response body | Hard `64 KiB` (`65,536` bytes) cap enforced incrementally on `QIODevice::readyRead` + `15s` transfer timeout across `CloudClient` and `UpdateService`. |
| T8 | **Revoked session blocks local database work** | `401 invalid_grant` on refresh locks UI or deletes saved MongoDB connections | `401` clears only `cloud/*` `KeyStore` slots and resets plan to `Free`; local connections, SSH/TLS keys, and scripts remain completely untouched. |
| T9 | **Update manifest rollback / replay attack** | Compromised mirror or DNS replays an older validly signed `releases.json` + `.minisig` | `UpdateService` persists `cachedManifestTimestampSec` monotonically and passes it as `Minisign::VerifyOptions::minTimestampSec`; older signed manifests and unsigned `/v1/release-check` downgrades are rejected. |
| T10 | **Missing or temporarily locked OS KeyStore** | Portable/headless build without `libsecret`/Keychain or temporarily locked login keychain | When `!KeyStore::isAvailable()`, `m_volatileRefreshToken` stays strictly in RAM for the running session (`cloudSignedIn: false` on disk, cleansed on exit/logout); transient `KeyStore::Result::Error` preserves local session metadata instead of revoking. |

---

## 7. Security Verification Plan

In accordance with repository security guidelines and secure coding verification practices:

1. **Cryptographic Verification Tests (`Minisign_test.cpp` & `CloudClient_test.cpp`)**:
   - Positive test vector generated with real Ed25519 keypair + BLAKE2b-512 (`ED`) and legacy (`Ed`)
     signatures, plus cross-check with `ci/lib/minisign.mjs` wire format.
   - Negative tests: 1-bit flip in manifest body, 1-bit flip in `rawSig`, 1-bit flip in `globalSig`,
     1-bit edit in `trusted comment` (`release:9.9.9`), mismatched 8-byte `keyId`, truncated Base64,
     wrong public key, `expectedFile` / `expectedRelease` mismatch.
   - Entitlement token verification tests: valid ES256 `entitlement+jwt` bound to `device_id` accepted;
     tampered payload, wrong `device_id`, wrong `iss`, or expired offline grace rejected -> stays `Free`.
2. **Authentication & Session Lifecycle Tests (`CloudClient_test.cpp`)**:
   - Zero network traffic on construction and startup when unsigned-in.
   - Full RFC 8628 device flow against in-process `QTcpServer` (`127.0.0.1:0`): `authorization_pending` ->
     `slow_down` (`+5s` interval increase) -> `200` token exchange -> `/v1/me` + `/v1/me/entitlements`.
   - Phishing `verification_uri` (`https://evil.example/device`, `javascript:alert(1)`) rejected.
   - Concurrent `withValidAccessToken` calls execute **single-flight** (`refreshHits == 1`).
   - `401 invalid_grant` on refresh wipes `InMemoryCloudCredentialStore` secrets and degrades cleanly to `Free`.
   - Offline `503` / network drop during refresh preserves cached `refresh_token` and `OfflineGrace` entitlement.
   - `signOut()` sends `{"refresh_token": "..."}` without `Authorization` header and wipes secrets even when
     server returns `500` or closes socket.
3. **Update Throttle, Store Build & Minisign Integration Tests (`UpdatePulse_test.cpp` / `Minisign_test.cpp`)**:
   - `RAGO_STORE_BUILD` suppresses startup and manual checks.
   - 24h auto-check throttle prevents duplicate background pings within 24 hours while allowing manual checks.
   - Tampered `releases.json` or missing `.minisig` when `RAGOSHELL_MINISIGN_PUBKEY` is set blocks both
     `updateAvailable` and `mandatory`.
4. **UI, Accessibility, Responsive Geometry & i18n Gates**:
   - `RagoShell --check-qml` exits `0`.
   - `dev/ui-shots/geometry.sh` verified across `360x800 Touch`, `768x1024 Touch`, and desktop sizes for
     `prefs` (including `Account` section) and `about` (`0` items beyond edge / cut).
   - `node ci/check-i18n.mjs` passes with zero unfinished strings across all 15 `.ts` catalogs.
   - Independent security review subagent audit on full `git diff` prior to ticking `5.1`.
