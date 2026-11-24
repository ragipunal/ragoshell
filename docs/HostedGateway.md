# Mod B Hosted Gateway — hosted cloud bridge (roadmap 8.5, WebAssembly.md Faz 4.3)

Written: 2026-11-03 · High tier (auth/tokens + money + public network path).
Plan + threats before code.

> Note 2026-11-24: the hosted bridge is free for every verified account
> (decision 2026-11-23; no Pro/Team plans exist) and the per-device cap of 2
> became **1 session per account** (ledger keyed by `sub`,
> `RAGOSHELL_GW_MAX_PER_DEVICE` ignored with a warning). §1–8 are the
> 2026-11-03/04 design and keep their original wording where it is history;
> §9 is current.

Mod B (8.4) ships a self-hosted `ragoshell-web-gateway` guarded by a shared
pairing token. 8.5 turns the same binary + protocol into the **hosted
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
              ├─ features.hosted_bridge == true       ← hosted_bridge feature gate
              ├─ status ∈ {active, trialing}
              └─ DeviceSessionLedger.tryAcquire(did)  ← per-device cap (per account since 2026-11-24, §9.1)
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
| ConnectionDialog | QML | "RagoShell Cloud" channel + TLS-target hint, visible only when `CloudService.hostedBridgeEnabled` |
| Infra | `ragoshell-infra` | `web-gateway` compose service + nginx `/ws` location — High-tier per infra rules, deployment `(insan)` |

## 3. Env (hosted additions to §WebGateway)

- `RAGOSHELL_GW_AUTH=hosted` — enables JWT mode (default `token`).
- `RAGOSHELL_GW_ENT_KEYRING` — `kid:spki-b64;…`; default: embedded
  `BrandInfo::entitlementPubkeys` ring.
- `RAGOSHELL_GW_ENT_ISS` / `RAGOSHELL_GW_ENT_AUD` — expected iss/aud;
  defaults `EntitlementVerifier::defaultApiIssuer()/defaultAppAudience()`.
- `RAGOSHELL_GW_MAX_PER_DEVICE` — concurrent authed sessions per `did`
  (default 2). Ignored with a warning since 2026-11-24: replaced by
  `RAGOSHELL_GW_MAX_PER_ACCOUNT` (1, keyed by `sub`; §9.1).
- `RAGOSHELL_GW_REQUIRE_TLS=1` — refused non-TLS Mongo targets (default on
  in hosted mode).

## 4. Threats

| # | Threat | Mitigation |
|---|---|---|
| H1 | **Unauthorized use**: forged/expired token, account without `hosted_bridge`, canceled status | ES256 signature + typ/iss/aud + exp/nbf+skew + `hosted_bridge` + status ∈ {active, trialing} all enforced at hello; failure → close `unauthorized`. The session itself ends at the token's `exp` (closes `entitlement expired`, frames after `exp` refused — review R8); access ended before `exp` is not revoked (no `jti` check) and stays usable until `exp` (the operator deny list, §9.4, now covers urgent cases) |
| H2 | **Key rotation mismatch**: API rotates `kid`, gateway holds stale ring | kid-indexed ring + `GW_ENT_KEYRING` accepts multiple entries; restart-less pickup is an ops reload (documented — 8.6) |
| H3 | **Token replay across devices**: JWT stolen from device A used from device B | `did` embedded in the token — sessions counted by `did`; theft window bounded by 7-day TTL (offline grace does not extend minted validity for auth here — `isUsable()` allows grace; the gateway accepts `ValidOnline` **only**: `offline_grace_until` exists for offline client features, not a hosted network service) |
| H4 | **Per-device DoS**: one user opens all 8 global slots | `kMaxSessions` global cap + `maxPerDevice` (2) — one account can never starve others (since 2026-11-24: 1 session per account, §9.1) |
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
the gateway is authoritative anyway). UI hint (original 2026-11-03 wording, since replaced): "Pro plan requires TLS
targets (mongodb+srv:// or ?tls=true)". (Outdated since the 2026-11-04
review: the hosted gateway refuses `mongodb+srv://` for now; the hint now
reads "Signs in with your RagoShell account. Targets must use TLS
(mongodb+srv:// or ?tls=true).")

## 6. Tests

- `DeviceSessionLedger_test`: acquire/release/exhaust/different-keys.
- `WebSocketGatewayTransport_test` (hosted block): mint ES256 entitlement
  JWTs with the existing test helpers — valid token connects; account
  without `hosted_bridge` denied; expired denied; `at+jwt`-typ denied;
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
  `RAGOSHELL_GW_MAX_PER_DEVICE` (default 2; replaced by one session per
  account on 2026-11-24, §9.1).
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
- QML: channel list is dynamic — "RagoShell Cloud" (originally labelled "(Pro)"; the label is "%1 Cloud"
  since 2026-11-24) appears only while
  `CloudService.hostedBridgeEnabled`; choosing it never persists a pairing
  token.

Tests (§6) landed as `DeviceSessionLedgerTest` + a `hosted_*` block in
`WebSocketGatewayTransport_test` minting real ES256 tokens (valid connects;
no hosted_bridge/expired/wrong-typ denied; per-device cap enforced and frees on
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
client could hold two concurrent server connections per device under the
then-default cap of 2 (one per account since 2026-11-24, §9.1); a further
server connection gets refused until a slot
frees (corrected by the 8.6 review — shell/monitor tabs currently do NOT
open a WebSocket because secondary workers bypass `IMongoTransport`;
they are refused client-side until a transport-routed shell ships).

## 9. Free tier quotas — 2026-11-24 (plan before code)

The cloud services are free (decision 2026-11-23), so the hosted bridge is
open to every verified account and limits are the only brake on cost and
abuse. Approved values: **1** concurrent session per account, **2 h** per
session, **15 min** idle, **1 GB** per account per calendar month (UTC, both
directions). The gateway verifies tokens offline and has no database, so the
counters live in the gateway process plus one local file (single instance;
a second instance needs the API-side counter instead, see 9.6).

### 9.1 Session limits

| Limit | Env (default) | Behaviour |
|---|---|---|
| Sessions in total | `RAGOSHELL_GW_MAX_SESSIONS` (8, `kMaxSessions`) | size it for the server; see Q8 |
| Sessions per account | `RAGOSHELL_GW_MAX_PER_ACCOUNT` (1) | ledger key is the token's `sub` (canonical UUID, verified), no longer `did`; a second hello for the same account gets `session limit reached: one cloud bridge session per account` |
| Session length | `RAGOSHELL_GW_SESSION_MAX_SEC` (7200) | the lease timer fires at the earlier of the JWT `exp` and hello + 2 h; closes `session time limit reached`; frames after it are refused. The 2 h limit runs on the monotonic clock, so a wall-clock step cannot lengthen it |
| Idle | `RAGOSHELL_GW_IDLE_SEC` (900) | hosted mode only; token mode keeps `GatewayProtocol::kIdleTimeoutMs` |

`RAGOSHELL_GW_MAX_PER_DEVICE` is dropped: one session per account already
caps every device. A deployment that still sets it gets a startup warning.
Every limit variable that is set but is not a whole number, or is out of
range, stops the start (a typo like `1GB` must not fall back silently).

### 9.2 Monthly transfer

- Counted: the UTF-8 size of every WebSocket text frame after a successful
  hello, in both directions (requests in, replies out). The hello exchange,
  the close-reason frame and WebSocket pings are not counted (small,
  bounded). A reply the gateway fetched but replaced with the short
  "result too large" error still counts with its size, so oversized queries
  cannot pull unlimited data from MongoDB for the price of an error frame;
  apart from that, gateway↔MongoDB traffic tracks the frames closely.
- Limit: `RAGOSHELL_GW_MONTHLY_BYTES` (1 000 000 000). Checked at hello (an
  account at the limit is refused with `monthly transfer limit reached`),
  on every incoming frame, and before every outgoing frame: a reply that
  would cross the limit is not sent; the session closes with the same
  reason. The limit is never exceeded by more than one incoming frame
  (bounded by `kMaxFrameBytes`).
- Reset: the counters belong to a month key (`YYYY-MM`, UTC); the first use
  in a later month drops every account's counter. A clock that steps back
  into an earlier month changes nothing (counting continues in the current
  month), so a clock glitch cannot hand out fresh quotas.

### 9.3 Counter file

- `RAGOSHELL_GW_QUOTA_FILE` is **required** in hosted mode; without it the
  gateway refuses to start (a restart must not reset everyone's quota).
- Format: `{"version":1,"month":"2026-11","accounts":{"<sha256(sub) hex>":bytes}}`.
  Account ids are stored hashed; the file holds no token, IP or target.
- Written atomically (`QSaveFile`: temp file + rename, owner-only
  permissions) at most once a minute while counters changed, when a session
  ends, and at shutdown (SIGTERM/SIGINT only set a flag; the event loop quits
  and saves). A crash loses at most a minute of counting (accepted: bounded
  under-count).
- Startup: a missing file starts empty; a file that exists but cannot be
  read or parsed stops the start (fail closed; the operator inspects or
  deletes it). A failed write is logged and retried; open sessions continue,
  but after 3 failed writes in a row new sessions are refused (`cloud bridge
  is temporarily unavailable`) until a write succeeds — otherwise a restart
  would silently reset the month.
- The hash is unsalted SHA-256: whoever already knows an account id can
  confirm from the file that it used the bridge. Accepted while the file
  stays in the gateway's private volume; an HMAC with a gateway secret is
  the upgrade if the file ever leaves it.
- Size: one entry per account that used the bridge this month; the file is
  rewritten whole, so it stays small (about 80 bytes per account).

### 9.4 Account deny list (revocation)

A token stays valid until `exp` (7 days) and the gateway cannot ask the API,
so switching the bridge off for an account (`{"hosted_bridge":false}` on its
`free` row) used to reach the gateway only when the token expired (F1
review). New: `RAGOSHELL_GW_DENY_FILE` (optional) names a text file with one
account UUID per line (`#` comments allowed). The gateway re-reads the whole
file once a minute (at most 4 MiB). A listed account is refused at hello,
and its open session closes on its first frame after the reload, so within
about a minute of activity (an idle one closes within 15 minutes anyway).
Once a list has loaded, a file that becomes unreadable or disappears (a
lost mount) keeps the previous list and logs a warning; to clear the list,
empty the file. Lines that are not UUIDs are ignored with a warning. The runbook step joins the bridge deployment
(infra), together with the free-row switch.

### 9.5 Threats

| # | Threat | Mitigation |
|---|---|---|
| Q1 | One account opens many sessions to multiply bandwidth | ledger keyed by `sub`, 1 per account; global `kMaxSessions` unchanged |
| Q2 | Long-lived tunnel kept open with keep-alive frames | 2 h hard lease regardless of activity; 15 min idle |
| Q3 | Bulk transfer through the bridge | 1 GB per month per account, checked before each reply |
| Q4 | Restart or crash resets quotas | counter file required, atomic writes, corrupt file fails the start |
| Q5 | Counter file leaks who uses the bridge | hashed account ids, byte counts only; file lives in the gateway's private volume |
| Q6 | Disabled account keeps using a valid token | deny list file, checked at hello and on every frame |
| Q7 | Many accounts (sign-up abuse) multiply the quota | out of scope here: Turnstile, sign-up rate limits and the registration switch (F2) |
| Q8 | A handful of free accounts hold every global slot with a ping every few minutes, reconnecting after each 2 h lease | **accepted residual**: `RAGOSHELL_GW_MAX_SESSIONS` sized for the server, per-IP limits at nginx (deployment), slot-usage alarm (F7), registration switch; a per-IP cap or a cooldown after a timed-out session are the next steps if it happens |
| Q9 | Oversized queries pull data from MongoDB that the client never receives | discarded replies are charged with their size (9.2) |
| Q10 | Clock steps lengthen leases or reset quotas | monotonic session deadline; month counters only move forward |

### 9.6 Out of scope

- Deployment: done in `ragoshell-infra` on 2026-11-24 (`docs/bridge.md`):
  compose service `web-gateway` behind the profile `bridge`, nginx
  `location = /ws` on the app host, a host state directory for the quota and
  status files and a read-only one for the deny list (`read_only` root
  stays). Still open there: an image (no CI publishes one) and the human
  steps of its runbook. Client keepalive: Cloudflare may close an idle
  WebSocket after about 100 s and the client sends no WebSocket ping.
- Several gateway instances: move the monthly counter to the API (an
  internal endpoint), keep the per-instance session ledger.
- ~~The client fetches a new entitlement token only at sign-in or on the
  manual refresh in Preferences; the client strings still say "Pro".~~
  Both done 2026-11-24 (§9.8): automatic refresh at half-life, channel
  label "%1 Cloud", no "Pro" wording.

### 9.7 Status file for monitoring — 2026-11-24

`RAGOSHELL_GW_STATUS_FILE` (optional, hosted mode only): once at start and
every minute the bridge writes aggregate `key=value` lines, integers only,
atomically (`QSaveFile`), mode 0644 — `version`, `written_at`, `month`,
`sessions`, `max_sessions`, `full_refusals_1h` (connections refused because
every slot was taken, in the last hour: the signal for threat Q8),
`monthly_bytes` (all accounts), `monthly_limit`, `accounts`,
`accounts_exhausted`, `quota_healthy`. No account id or hash, no target.
A malformed month is written as `0000-00`, negative values as 0. The host
check `scripts/resource-alert.sh` in `ragoshell-infra` reads it and mails
the operator (thresholds and threats: that repository's
`docs/monitoring.md`, "Free cloud tier alarms").

### 9.8 Client: automatic entitlement refresh — 2026-11-24 (plan before code)

Until now the app fetched a new entitlement token only at sign-in or on the
manual refresh in Preferences, and it never re-checked the expiry of the
cached token while running, so a 7-day-old token went into the hello and the
bridge closed the session with "entitlement expired".

- `CloudClient` gets an opt-in auto-refresh (the app enables it; tests and
  headless runs do not). A 15-minute check timer (wall clock, so it also
  catches up after sleep) and one check 10 s after start:
  1. a cached token that was `ValidOnline` but whose `exp` has passed is
     verified again, so `entitlementJwt()` stops returning it (the state
     becomes offline grace or expired);
  2. when the token is past half of its lifetime (`iat + (exp − iat) / 2`,
     3.5 days today), or not usable at all, it calls the existing
     `refreshAccountAndEntitlements()` (single-flight access token refresh,
     `/v1/me`, `/v1/entitlements`).
- Failures back off: 5 minutes, doubling up to 1 hour; a successful
  `entitlementsUpdated` resets it. Nothing runs while signed out ("never
  phones home when unsigned-in" stays true) or while a request is in flight.
- A revoked device (refresh `401 invalid_grant`) is signed out in the
  background, exactly as a manual refresh does today.
- Connecting through the hosted channel without a usable token: signed in →
  start a refresh and say "Your cloud access is being renewed…"; signed out
  → "Sign in to your cloud account…". The "Pro" wording leaves the channel
  label and both messages (the cloud services are free, 2026-11-23).

Threats: no new data leaves the device (same three requests as the manual
refresh); a broken server cannot cause a request storm (backoff, at most one
attempt per check, single-flight); a stale token is no longer offered.
