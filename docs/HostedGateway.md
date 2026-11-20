# Mod B Hosted Gateway — Pro cloud bridge (roadmap 8.5, WebAssembly.md Faz 4.3)

Written: 2026-11-03 · High tier (auth/tokens + money + public network path).
Plan + threats before code.

Mod B (8.4) ships a self-hosted `ragoshell-web-gateway` guarded by a shared
pairing token. 8.5 turns the same binary + protocol into the **hosted Pro
feature**: instead of a pairing token, the client's `hello` carries its
`entitlement+jwt` — the ES256 token `ragoshell-api` already mints for
verified, signed-in users (`internal/auth/entitlement.go`, client-side
`EntitlementVerifier` in rago-core). No new API surface is required.

## 1. Architecture

```
browser ──wss://app.ragoshell.com.tr/ws──▶ nginx (TLS+per-IP limits)
        ──▶ ragoshell-web-gateway  RAGOSHELL_GW_AUTH=hosted
              hello.token = entitlement+jwt
              ├─ EntitlementVerifier (kid→P-256 SPKI ring)
              │    exp/nbf±skew, offline_grace_until, typ, iss, aud
              ├─ features.hosted_bridge == true       ← Pro/Team gate
              ├─ status ∈ {active, trialing}
              └─ DeviceSessionLedger.tryAcquire(did)  ← per-device cap
        ──▶ EgressPolicy (public targets only, TLS-required in hosted mode)
        ──▶ NativeMongoTransport → user's MongoDB
```

Key material: the verifier ring comes from `RAGOSHELL_GW_ENT_KEYRING`
(`kid:b64-spki;…`) or falls back to `BrandInfo::entitlementPubkeys` (the
same embedded ring the desktop client verifies with — `fromEmbeddedDefaults`).

## 2. Changes

| Piece | Where | Notes |
|---|---|---|
| `HostedAuthConfig` | `GatewaySession` ctor (optional) | `{verifier, options, ledger, maxPerDevice}` — unset → pairing-token path (8.3/8.4 unchanged) |
| `DeviceSessionLedger` | `core/transport` *(new)* | `QHash<QString,int>`: `tryAcquire(didOrSub)`/`release` — counts live authed sessions per device |
| `EgressPolicy::setRequireTls` | `core/transport` | hosted mode: `mongodb://` URI must carry `tls=true`/`ssl=true`; denied → `Egress denied: target must use TLS.` (`mongodb+srv://` is refused on public gateways since the 2026-11-04 review R6 — see §8) |
| `CloudClient::entitlementJwt()` | `core/cloud` | exposes the cached token from the KeyStore slot (in-memory read; nothing new persisted) |
| `QmlBackend::connectToServer` | app | `transportKind=="hosted"` → injects `transportToken=entitlementJwt()` into the *in-memory clone* — never written to settings |
| `MongoWorker` factory | `core/mongodb` | `hosted` → `WebSocketGatewayTransport` at `wss://app.<domain>/ws` (`BrandInfo::portalUrl` host) — cleartext-ws rule already exempts `wss://` |
| ConnectionDialog | QML | "RagoShell Cloud (Pro)" channel + TLS-target hint, visible only when `CloudService.hostedBridgeEnabled` |
| Infra | `ragoshell-infra` | `web-gateway` compose service + nginx `/ws` location — High-tier per infra rules, deployment `(insan)` |

## 3. Env (hosted additions to §WebGateway)

- `RAGOSHELL_GW_AUTH=hosted` — enables JWT mode (default `token`).
- `RAGOSHELL_GW_ENT_KEYRING` — `kid:spki-b64;…`; default: embedded
  `BrandInfo::entitlementPubkeys` ring.
- `RAGOSHELL_GW_ENT_ISS` / `RAGOSHELL_GW_ENT_AUD` — expected iss/aud;
  defaults `EntitlementVerifier::defaultApiIssuer()/defaultAppAudience()`.
- `RAGOSHELL_GW_MAX_PER_DEVICE` — concurrent authed sessions per `did`
  (default 2).
- `RAGOSHELL_GW_REQUIRE_TLS=1` — refused non-TLS Mongo targets (default on
  in hosted mode).

## 4. Threats

| # | Threat | Mitigation |
|---|---|---|
| H1 | **Free-tier bypass**: forged/expired token, hosted_bridge=false, canceled plan | ES256 signature + typ/iss/aud + exp/nbf+skew + `hosted_bridge` + status ∈ {active, trialing} all enforced at hello; failure → close `unauthorized`. The session itself ends at the token's `exp` (closes `entitlement expired`, frames after `exp` refused — review R8); a plan canceled before `exp` is not revoked (no `jti` check) and stays usable until `exp` |
| H2 | **Key rotation mismatch**: API rotates `kid`, gateway holds stale ring | kid-indexed ring + `GW_ENT_KEYRING` accepts multiple entries; restart-less pickup is an ops reload (documented — 8.6) |
| H3 | **Token replay across devices**: JWT stolen from device A used from device B | `did` embedded in the token — sessions counted by `did`; theft window bounded by 7-day TTL (offline grace does not extend minted validity for auth here — `isUsable()` allows grace; the gateway accepts `ValidOnline` **only**: `offline_grace_until` exists for offline client features, not a hosted network service) |
| H4 | **Per-device DoS**: one user opens all 8 global slots | `kMaxSessions` global cap + `maxPerDevice` (2) — one account can never starve others |
| H5 | **SSRF / plaintext exfil**: hosted gateway must never dial internal nets or ship creds over plaintext | `EgressPolicy` public-only: one host, forced `directConnection`, no `+srv`, no host-side auth/file options, clamped timeouts (review R1/R3/R5/R6) + `requireTls` (deny non-TLS URI) |
| H6 | **Token in logs** | JWT never logged; session close reason is generic |
| H7 | **Replay of `at+jwt` access token instead of entitlement** | verifier pins `typ="entitlement+jwt"` — `at+jwt`/`refresh` rejected by type |
| H8 | **did-less tokens** | `did` empty → fall back to `sub` as the ledger key (per-user cap semantics documented); missing both → deny |
| H9 | **QML exposes token** | JWT injected into an in-memory settings clone at connect time; `transportTokenEncrypted` path never sees it; field uses PasswordEchoOnEdit already |

## 5. Client flow (hosted)

`transportKind="hosted"` is a named target: MongoWorker resolves it to
`wss://app.<domain>/ws`; `QmlBackend::connectToServer` injects the current
cached `entitlement+jwt` as `transportToken` on the cloned
`ConnectionSettings` (memory only). If the user is signed out or
`hosted_bridge` is false the dialog hides the channel (defense-in-depth —
the gateway is authoritative anyway). UI hint: "Pro plan requires TLS
targets (mongodb+srv:// or ?tls=true)". (Outdated since the 2026-11-04
review: the hosted gateway refuses `mongodb+srv://` for now — the hint
text is a QML string and is left for a translation pass.)

## 6. Tests

- `DeviceSessionLedger_test`: acquire/release/exhaust/different-keys.
- `WebSocketGatewayTransport_test` (hosted block): mint ES256 entitlement
  JWTs with the existing test helpers — valid pro token connects; free
  plan (`hosted_bridge=false`) denied; expired denied; `at+jwt`-typ denied;
  second concurrent session same `did` denied while first lives, allowed
  after disconnect; `requireTls` → non-TLS URI denied.
- `EgressPolicy_test`: requireTls cases.

## 7. Out of scope / human

- Deploying the service behind nginx on `app.ragoshell.com.tr` `(insan)`.
- JWKS fetch endpoint consumption (`GET /v1/auth/jwks.json`) — v1 uses the
  embedded/env ring; live JWKS pull is an 8.6 consideration.
- Per-IP rate limiting at nginx (infra config, not code).

## 8. Implementation status — 2026-11-04

All §2 pieces landed:

- `GatewaySession` hosted path: `hello.token` verified via
  `EntitlementVerifier` — `ValidOnline` only (offline grace does not open a
  hosted network session), `features.hostedBridge` + status ∈
  {active, trialing} enforced, ledger key = `did` else `sub`, missing key →
  deny. Slot released on disconnect and in the destructor.
- `DeviceSessionLedger` (header-only, single event-loop) +
  `RAGOSHELL_GW_MAX_PER_DEVICE` (default 2).
- `EgressPolicy::setRequireTls` — `mongodb://` requires `tls=true`/`ssl=true`;
  `mongodb+srv://` implies TLS (but is refused on public gateways since
  review R6). Independent of `allowPrivate`; default on in hosted mode
  (`GW_REQUIRE_TLS=0` opts out for testing only).
- `ragoshell-web-gateway`: `RAGOSHELL_GW_AUTH=hosted` loads the ring from
  `GW_ENT_KEYRING` or `EntitlementVerifier::fromEmbeddedDefaults`
  (`BrandInfo::entitlementPubkeys`); iss/aud overridable via
  `GW_ENT_ISS`/`GW_ENT_AUD`. Token-mode startup checks are skipped in hosted
  mode (a pairing token is not used there).
- Client: `CloudClient::entitlementJwt()` (usable states only — stale/expired
  cache never leaves the KeyStore), `CloudService` passthrough +
  `hostedBridgeEnabled` property, `QmlBackend::connectToServer` injects the
  JWT into the in-memory settings clone (never persisted), `MongoWorker`
  maps `hosted` → `wss://app.<brand-domain>/ws`. Cleartext `ws://` to
  non-loopback stays refused.
- QML: channel list is dynamic — "RagoShell Cloud (Pro)" appears only while
  `CloudService.hostedBridgeEnabled`; choosing it never persists a pairing
  token.

Tests (§6) landed as `DeviceSessionLedgerTest` + a `hosted_*` block in
`WebSocketGatewayTransport_test` minting real ES256 tokens (valid connects;
free plan/expired/wrong-typ denied; per-device cap enforced and frees on
disconnect) and `EgressPolicyTest.require_tls_enforces_tls_targets`.

Security review (2026-11-04) fixes applied: `+srv` URIs with an explicit
`?tls=false`/`ssl=false` opt-out are denied (no scheme exemption in
`requireTls` — `parsed.tls()` already reflects libmongoc's finalizer); the
Test-Connection path injects the JWT like `connectToServer`; editing a
hosted connection while signed out keeps a selectable placeholder instead
of silently rewriting the kind to `memory`; `transportTokenEncrypted` is
stripped from cloud sync like the other device-bound ciphertexts (plain
token only under the `includePasswords` opt-in); `entitlementJwt()` /
`hostedBridgeEnabled` are gated on `ValidOnline` so the offline-grace state
can't expose a channel that always denies; hosted-mode startup skips the
pairing-token checks and warns accurately for `GW_OPEN`/`GW_ORIGINS='*'`.

`tlsInsecure` / `tlsAllowInvalid*` URI options are refused on public
gateways since the 2026-11-04 review (R5; they used to pass `requireTls`
as an accepted residual). The same review made `mongodb+srv://` an
explicit refusal on public gateways ("not supported on public gateways
yet"): Atlas users connect with a single-host `mongodb://…?tls=true` URI
until the gateway dials through its own checked connector
(`docs/WebGateway.md` §2). Ledger
slots are per WebSocket — only the primary `MongoWorker` opens one, so a
client can hold two concurrent server connections per device under the
default cap; a third server connection gets `unauthorized` until a slot
frees (corrected by the 8.6 review — shell/monitor tabs currently do NOT
open a WebSocket because secondary workers bypass `IMongoTransport`;
they are refused client-side until a transport-routed shell ships).
