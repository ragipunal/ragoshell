# Client & Gateway Security Hardening

This document records the design, migration rules, invariants, and threat model for hardening
client `KeyStore` credential storage, hosted bridge monthly quota accounting, local HTTP endpoint
scoping, bounded file reads, and air-gapped `disableHttpsFeatures` enforcement.

---

## 1. Hosted bridge monthly quota exhaustion on partial remainder

- **Affected components:** `BridgeQuota` (`src/ragoshell/core/transport/BridgeQuota.{h,cpp}`),
  `GatewaySession` (`src/ragoshell/core/transport/GatewaySession.cpp`).
- **Root cause:** Previously, `GatewaySession::sendFrame()` and `onTextMessage()` called
  `BridgeQuota::consume()`, which returns `false` without mutating `_bytes` when `bytes > _limit - used`.
  If an account had a small positive remainder (larger than an ~80-byte request frame / `hello`, yet
  smaller than a large MongoDB reply buffered up to `_maxResponseBytes = 48 MiB`), `sendFrame()` closed
  the socket with `"monthly transfer limit reached"` without consuming the remaining `monthlyLimit - used`
  bytes. An attacker could reconnect repeatedly and pull large volumes of MongoDB traffic before the
  counter ever reached `monthlyLimit`.
- **Change:** Add `BridgeQuota::consumeOrExhaust(account, bytes, nowSec)` (while preserving existing
  `consume()` semantics for callers/tests expecting unmodified counters on failure):
  - When `bytes <= _limit - used`, record the transfer normally and return `true`.
  - When `bytes > _limit - used`, set the account's counter to `_limit` (marking `_dirty = true` if
    `used < _limit`) and return `false`.
  - Use `consumeOrExhaust()` across `GatewaySession::sendFrame()`, `chargeDiscarded()`, and
    `onTextMessage()` so any refused reply or incoming overspend immediately marks the account
    exhausted for subsequent `hello` handshakes.

---

## 2. BYOK AI API keys in OS `KeyStore` instead of `ragoshell.json`

- **Affected components:** `SettingsManager` (`src/ragoshell/core/settings/SettingsManager.{h,cpp}`).
- **Root cause:** Previously, `anthropic`, `gemini`, and `openai_compat` API keys were persisted inside
  `ragoshell.json` via `RagoCrypt` (`SimpleCrypt`), a 61-bit repeating XOR stream with single-byte CBC
  chaining. Moreover, `openai_compatApiKeyEncrypted` prepended deterministic plaintext
  `urlOrigin(baseUrl) + "\n"` inside the ciphertext while storing `openai_compatBaseUrl` in plaintext next
  to it, allowing trivial known-plaintext recovery of the shared 8-byte `RagoCrypt::_KEY` from
  `ragoshell.json` alone.
- **Change:**
  - Inject an optional `std::shared_ptr<KeyStore>` into `SettingsManager` (defaulting to
    `createPlatformKeyStore()`) so production persists keys in OS-protected storage while unit tests can
    inject an isolated in-memory `KeyStore`.
  - Partition slot names using the same pattern as `KeyStoreCloudCredentialStore`: `"ai/<provider>"` for
    the default profile, or `"ai:<sha256(appDataDir).left(16)>/<provider>"` when `RAGOSHELL_CONFIG_DIR`
    is overridden, isolating tests and alternate profiles.
  - Only sync modified keys to `KeyStore` inside `SettingsManager::save()` (not on in-memory setters), so
    cancelling Preferences leaves stored credentials untouched.
  - One-way migration on `load()`: read from `KeyStore` first; if absent in `KeyStore` and a legacy
    `apiKeyEncrypted` exists in `ragoshell.json`, decrypt it, cleanse temporary plaintext copies with
    `OPENSSL_cleanse()`, and **only** scrub `apiKeyEncrypted` from `ragoshell.json` (and any schema `.bak-*`
    backup) when `syncAiKeysToKeyStore()` confirms `KeyStore` is available and every write succeeds. If
    `KeyStore` is temporarily unavailable or fails, warn and leave legacy `ragoshell.json` untouched on disk
    so credentials are never silently lost.
  - Never serialize `apiKeyEncrypted` in `convertToMap()`.
  - When no `KeyStore` is available (e.g., headless Linux without Secret Service, or `RAGOSHELL_KEYSTORE=file`),
    keep AI API keys in process RAM for the active session only and log a warning without writing keys
    to disk. Cleanse in-memory keys on overwrite, clear, and destruction.

---

## 3. BYOK AI plaintext `http://` link-local and cloud metadata exclusion

- **Affected components:** `AiQueryAssistantEndpoint.cpp` (`isPrivateHttpHost()`).
- **Important invariant:** Do **not** alter `LocalNetwork.cpp` (`isLocalNetworkHost()`), which is still
  used for iOS local-network permission prompts where link-local detection is expected and tested.
- **Change:** In `isPrivateHttpHost()`, alongside carrier-grade NAT (`100.64.0.0/10`), explicitly reject:
  - IPv4 link-local / cloud IMDS (`169.254.0.0/16`, including `169.254.169.254`)
  - IPv6 link-local (`fe80::/10`)
  - AWS IPv6 IMDS (`fd00:ec2::/32`)
  so unencrypted `http://` requests to those ranges are rejected as `BaseUrlError::PlainHttpPublic` /
  `HostScope::Internet`, while RFC 1918 LAN addresses (`10/8`, `172.16/12`, `192.168/16`), non-IMDS ULA
  (`fdxx::`), and `.local` mDNS continue working per design when no key is set.

---

## 4. Air-gapped `disableHttpsFeatures` enforcement and read-time `entitlementJwt` expiry

- **Affected components:** `CloudService` (`src/ragoshell-app/CloudService.cpp`), `CloudClient`
  (`src/ragoshell/core/cloud/CloudClient.cpp`).
- **Root cause:** `CloudService` enabled `setAutoRefreshEnabled(true)` unconditionally, `CloudClient`
  did not consult `SettingsManager::disableHttpsFeatures()` in `checkEntitlementRefresh()`,
  `refreshAccountAndEntitlements()`, `withValidAccessToken()`, `startDeviceSignIn()`, or `signOut()`'s
  `/v1/auth/logout` call, and `CloudService::hostedTokenMissingMessage()` could trigger refresh calls
  despite `disableHttpsFeatures == true`. Additionally, `CloudClient::entitlementJwt()` and
  `CloudService::hostedBridgeEnabled()` did not check `m_claims.expiresAt` at read time between timer
  ticks, and `signOut()` / `handleSessionRevoked()` left `m_autoRefreshBackoffSec` /
  `m_autoRefreshNextAttemptAt` non-zero.
- **Change:**
  - Gate auto-refresh startup (`CloudService`), `hostedTokenMissingMessage()`,
    `CloudClient::checkEntitlementRefresh()` (after local offline expiry downgrade),
    `refreshAccountAndEntitlements()`, `withValidAccessToken()`, `startDeviceSignIn()`, and the remote
    `/v1/auth/logout` POST in `signOut()` on `!settings->disableHttpsFeatures()`, ensuring local
    sign-out state and credential cleansing still run even when network calls are blocked.
  - Check `m_claims.expiresAt > 0 && nowUnixSec() < m_claims.expiresAt` in `CloudClient::entitlementJwt()`
    and avoid exposing hosted bridge as active when expired.
  - Reset `m_autoRefreshBackoffSec = 0` and `m_autoRefreshNextAttemptAt = 0` in both `signOut()` and
    `handleSessionRevoked()`.

---

## 5. Bounded regular-file reads, `2^53 - 1` JSON precision, AI response cap, and key redaction

- **Bridge quota / deny file reads (`BridgeQuota.cpp`):** Reject symlinks/junctions and non-regular files
  (FIFOs, directories, devices) before reading; read in bounded `64 KiB` chunks capped at `maxBytes + 1`
  instead of calling unbounded `readAll()`. Cap `BridgeQuota` counters and `_limit` to
  `kMaxSafeJsonBytes = (1LL << 53) - 1` (IEEE-754 exact `double` integer ceiling) and use saturating
  addition in `Totals`.
- **AI response size enforcement (`AiAssistant.cpp`):** Enforce limits on both `readyRead` and
  `downloadProgress`, and verify `body.size() <= max` after `reply->readAll()` in both
  `AiAssistant::onFinished` and `AiModelCatalog::refresh`.
- **Active secret redaction (`AiQueryAssistant.{h,cpp}`, `AiAssistant.cpp`):** Accept an optional
  `activeSecret` parameter in `redactSecrets(text, activeSecret)` so shorter (`>= 4` chars) or
  non-alphanumeric custom OpenAI-compatible API keys are explicitly redacted in addition to pattern-based
  token redaction before any error text reaches `LOG_MSG`.
- **Public docs hygiene (`docs/WebAssembly.md`):** Fix stale `app.ragoshell.com` references to
  `app.ragoshell.com.tr`.

---

## 6. Threats and edge cases

| # | Threat / Edge Case | Mitigation |
|---|---|---|
| H1 | Attacker keeps small quota remainder and reconnects repeatedly to pull ~48 MiB MongoDB results | `consumeOrExhaust()` zeroes remaining quota (`used = _limit`) on any refused reply or incoming overflow; next `hello` fails closed immediately |
| H2 | Known `urlOrigin(baseUrl) + "\n"` plaintext inside `openai_compatApiKeyEncrypted` reveals 8-byte `RagoCrypt::_KEY` | API keys move out of `ragoshell.json` into OS `KeyStore`; `convertToMap()` never emits `apiKeyEncrypted` |
| H2a | Existing users upgrading still have `apiKeyEncrypted` in `ragoshell.json` | One-way migration on `load()` decrypts once, writes to `KeyStore`, cleanses plaintext, and scrubs `ragoshell.json` |
| H2b | Headless Linux / missing `KeyStore` | Keys live only in RAM for the current process, warned in log, cleansed on clear/destruct, never written plaintext to JSON |
| H2c | Tests or alternate `RAGOSHELL_CONFIG_DIR` clobbering real user `KeyStore` entries | `aiKeyAccount()` scopes slots with directory digest when `appDataDirOverridden()` is true, and `SettingsManager` injects `FakeKeyStore` in tests |
| M3 | Plaintext `http://` probes link-local / cloud metadata (`169.254.169.254`, `fe80::/10`, `fd00:ec2::/32`) | Explicitly excluded in `isPrivateHttpHost()` without weakening `LocalNetwork::isLocalNetworkHost()` |
| M4 | Air-gapped enterprise profile (`disableHttpsFeatures: true`) phones home on timer or connect | Blocked at `CloudService` startup, `hostedTokenMissingMessage()`, and all `CloudClient` network entrypoints; local `signOut()` cleanup still works |
| L5 | Malicious symlink / FIFO or `> 2^53 - 1` quota value causes OOM or precision loss | `readBoundedRegularFile` rejects symlinks/non-files and bounds read chunks; JSON limits clamped to `(1LL << 53) - 1`; saturating sum in `totals()` |
| L6 | Chunked HTTP body arrives before `downloadProgress`, or short custom key echoed by upstream API | `readyRead` + post-`readAll()` cap enforced; `redactSecrets()` receives active `apiKey` |
| L7 | Machine sleeps past `exp` and triggers hosted connect before 15m timer; leftover backoff on `signOut` | `entitlementJwt()` checks `nowUnixSec() < m_claims.expiresAt` directly; `signOut`/`handleSessionRevoked` reset backoff |
