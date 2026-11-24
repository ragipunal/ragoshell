# Bridge Security Review — consolidated report (roadmap 8.6)

Date: 2026-11-04 · Scope: `ragoshell-agent` (8.3), `ragoshell-web-gateway`
self-hosted (8.4) and hosted mode (8.5), the client
`WebSocketGatewayTransport`, `EgressPolicy`, `GatewayProtocol` /
`GatewaySession` / `DeviceSessionLedger`, pairing-token and entitlement-JWT
lifecycles, sync/export handling, Dockerfile, and the docs that describe
the system (`LocalAgent.md`, `WebGateway.md`, `HostedGateway.md`,
`TransportSeam.md`, `WebAssembly.md`).

> Note 2026-11-24: the hosted bridge is free for every verified account (no
> Pro plan), and the per-device cap of 2 became **1 session per account**
> with session, idle and monthly limits — see `HostedGateway.md` §9. The
> findings below are the 2026-11-04 review as it was.

Review method: four passes — per-phase diff reviews during 8.3/8.4/8.5
(each SHIP-WITH-FIXES, all findings closed in phase) plus a final holistic
review of the committed system at `28a7eaa` covering cross-component trust
assumptions. Verdict: **ship with reservations** — no HIGH findings
remain; the fixed items below and the residual list are the state of
record.

## 1. Threat model

The bridge places a WebSocket relay between the browser (WASM client) or
desktop client and the user's own MongoDB. The gateway's `connect` frame
carries the full Mongo URI including credentials, and `runCommand`
executes arbitrary server-side commands — by design it is a transparent
Mongo relay. Attacker classes considered:

| # | Threat | Control |
|---|--------|---------|
| T1 | Drive-by web page talks to the local agent (DNS rebinding / CSWSH) | Origin allowlist + loopback `Host` check, both before any session slot is handed out |
| T2 | Public gateway used as SSRF springboard | `EgressPolicy`: private/reserved ranges denied for every resolved address of the (single, on public gateways) seed host; discovery, `+srv` and host-side URI options refused; ambiguous IPv4 literals refused; failures closed (§6 R1/R5/R6) |
| T3 | Token/JWT theft, replay, or downgrade | 256-bit pairing token over `0600` file + constant-time compare; hosted mode verifies ES256 `entitlement+jwt` with typ/iss/aud/exp + `hosted_bridge` claim + active/trialing status; no offline-grace acceptance on a network service |
| T4 | One user starves the shared gateway | 8-socket global cap + per-device ledger cap (default 2) + `maxTimeMS` cap on every `runCommand` + clamped driver timeouts + backpressure/reply caps (§6 R3/R4/R7) |
| T5 | Cleartext credential transit | `ws://` refused to non-loopback targets; hosted mode additionally requires TLS targets (`requireTls`) and `wss://` client-side |
| T6 | Secret leakage into logs/sync/export | no token/JWT/URI logging; device-bound `*Encrypted` fields stripped from sync; connect-error text strips URI userinfo before reaching UI/log |
| T7 | Web-root escape / static-file abuse | canonicalized root + per-request containment check, `..`/NUL rejected after single decode, `.br` only on real Brotli negotiation |

## 2. Findings closed during implementation reviews

**8.3 agent** (1 HIGH + 5 med/low): rejected sockets leaking session slots
(signals now armed before the origin/host gate); idle timeout was dead
code; RNG fail-open closed; file permissions set before write; a
use-after-scope crash (stale signal into a dead `QEventLoop`) fixed.

**8.4 self-hosted gateway** (3 HIGH + 6 med/low): octal/leading-zero IPv4
literals parsed differently by Qt vs `getaddrinfo` (denied outright);
duplicate `uri` keys disagreeing between `QJsonDocument` (last wins) and
libbson (first wins) — the connect payload is now read through libbson so
the checked URI is the dialed URI; nonexistent `RAGOSHELL_WEB_ROOT`
degenerating containment to "serve CWD" (startup now fails); plus
`Referrer-Policy` header, `br;q=0` Brotli handling, port-range validation,
wildcard-origin warnings, minimum token length, generic client-facing
egress errors.

**8.5 hosted mode** (1 MED + 8 low/nit): `mongodb+srv://…?tls=false`
bypassed `requireTls` — the scheme exemption was removed because
`parsed.tls()` already reflects libmongoc's finalizer; Test-Connection
injected no JWT; editing a hosted connection while signed out silently
rewrote its kind; `transportTokenEncrypted` leaked into sync payloads;
offline-grace state exposed a channel that can only deny.

## 3. Findings from the consolidated review — closed in 8.6

| # | Sev | Finding | Fix |
|---|-----|---------|-----|
| M1 | med | One blocking `runCommand` (e.g. `$function`, `sleep`, long aggregation) stalled every session on the shared event loop with no server-side bound | `NativeMongoTransport::runCommand` clamps/inserts `maxTimeMS` at 50 s — a smaller user value wins, anything larger or absent is capped |
| M2 | med | Shell tabs and the server monitor bypass `IMongoTransport` entirely (secondary workers open raw `mongocxx::client`) — opaque failure on wasm, silent channel-bypass on desktop | `App::openShell` + both monitor open paths refuse `hasTransport()` connections with a clear UI error; transport-routed shells are future work (see §5) |
| M3 | med-low | `transportKind:"memory"` accepted on desktop — a synced/imported record could serve the canned demo catalog under a real server's name | `memory` is now `#ifdef Q_OS_WASM`-gated; other kinds keep working cross-platform |
| L4 | low | `DeviceSessionLedger`/`EntitlementVerifier` held as raw pointers into `main`'s stack — latent use-after-scope if a graceful shutdown ever unwinds | `HostedAuthConfig` now holds `shared_ptr`s; sessions keep the objects alive |
| L5 | low | Custom-CA / client-cert / CRL TLS options silently dropped over the bridge | `MongoWorker` refuses gateway connects carrying `caFile`/`pemKeyFile`/`pemPassPhrase`/`crlFile` with a clear error |
| L6 | low | Agent token file: `setPermissions` unchecked; loose existing file only warned | permission failure now aborts startup; loose existing file is chmod'd or refused |
| L7 | low | Driver error text could echo the URI (with embedded password) into UI and `ragoshell.log` | `mongodb://userinfo@` is masked in connect-error replies |
| L8 | low | `failAndClose` did not latch — frames in flight before the close could still be processed | `_closing` flag drops subsequent frames |
| L9 | low | Documentation truth drift | `WebGateway.md` origin note + known-limits, `HostedGateway.md` slot accounting, `WebAssembly.md` 3.2 claim, `TransportSeam.md` stale session-id line all corrected |

## 4. Verified clean (checked in code, no issue)

- **Pairing token**: `RAND_bytes(32)`, refuses to start on RNG failure or
  unreadable/unfixable permissions, `0600` before write, constant-time
  compare, `PasswordEchoOnEdit` in QML, `RagoCrypt`-encrypted persistence
  (`transportTokenEncrypted`), hosted connections never persist it.
- **Session state machine**: hello-first enforced, ops rejected pre-hello
  and pre-`connect`, binary frames refused, 1 MiB frame cap (enforced by Qt while reading since §6 R2), 30 s hello /
  30 min idle timers, ledger release on disconnect *and* in destructor.
- **Origin/Host**: agent requires allowlisted Origin AND loopback Host;
  web gateway checks Origin twice (HTTP verifier + session); `*` in
  `GW_ORIGINS` fails closed at the verifier and warns.
- **Cleartext `ws://`**: refused to non-loopback hosts; userinfo/octal/
  trailing-dot/bracket-IPv6 tricks all fail closed.
- **Egress**: range list covers loopback, RFC1918, CGNAT, link-local,
  metadata, ULA, multicast, reserved/documentation, NAT64/Teredo/6to4/
  v4-mapped/v4-compatible; every resolved address must be public; DNS
  failure closes; denials return a generic client message while details
  stay server-side. (Corrected by §6 R6: SRV answers were never checked —
  public gateways now refuse `+srv`.)
- **Entitlement JWT**: `alg`/`typ` pinned, kid-indexed asymmetric ring,
  iss/aud enforced (empty env overrides fall back to defaults), nbf/iat
  skew ≤300 s, `exp−iat` ≤7 d, claims parsed only after signature
  verification, `did`→`sub` ledger fallback, `ValidOnline`-only.
- **JWT secrecy**: injected into an in-memory settings clone only, never
  persisted/synced/exported; reachable only on the pinned
  `wss://app.<domain>/ws` host.
- **Static serving**: traversal/symlink escapes denied, security headers
  on denials too, `index.html` no-cache, Brotli only on explicit `br`.
- **Sync**: all `*Encrypted` fields stripped; plaintext secrets only under
  the explicit `includePasswords` opt-in.
- **Docker**: non-root uid 10001, `aqtinstall==3.3.0`/Qt 6.12.0/pinned
  vcpkg commit, whitelist `COPY` + `.dockerignore`, `docker build --check`
  clean.

## 5. Accepted residuals and future work

- **Transport-routed shells/monitors**: `ExecuteScriptRequest` /
  `ServerMonitorController` need a ScriptEngine path over `IMongoTransport`
  before shell tabs and monitoring work on wasm — a real feature item, not
  a patch. Until then gateway/demo connections hard-fail those affordances
  with a clear message (M2).
- **DNS-check → dial TOCTOU**: egress validates resolved addresses, then
  libmongoc re-resolves at connect; a hostile-DNS flip could land on a
  private IP. Full fix = connect-to-validated-IP with explicit SNI via a
  libmongoc stream initiator (§6 R1). Documented in `WebGateway.md` §2.
- **Blocking ops bounded but not async**: commands cap at 50 s
  `maxTimeMS`, yet a hanging `connect`/list still runs on the shared loop
  for up to the clamped driver timeouts (10 s connect / 55 s socket / 15 s
  server selection, §6 R3 — before that the URI could raise them without
  bound); per-session worker threads are the
  structural fix (nginx per-IP limits are the interim mitigation).
- **Pre-auth sockets count toward the 8-session cap** for the 30 s hello
  window.
- **Hosted concurrency headroom**: 2 sessions/device × 8 global means 4
  devices saturate the pool — raise caps with real traffic; nginx
  per-IP limits documented as the outer layer.
- **`tlsInsecure`/`tlsAllowInvalid*` URI options** — refused on public
  gateways since §6 R5; a LAN gateway (`allowPrivate`) still accepts them.
- **`RagoCrypt`/`SimpleCrypt` at rest is obfuscation-grade** — consistent
  with the existing `*Encrypted` convention.
- **Loopback `ws://` carries URI + token in cleartext** on `127.0.0.1` —
  the documented agent model.
- **Qt buffers whole frames** (bounded 1 MiB in, `kMaxResponseBytes` out
  — the 1 MiB bound only holds since §6 R2) and whole static files.
- **Docker base tags are mutable** (`ubuntu:24.04` unpinned digest);
  runtime filesystem is not read-only — compose `read_only:`+`tmpfs` is
  ops guidance.
- **Debug harness ships in production** (`--open-view`,
  `previewDebugState`) — UI-state fakes only; the gateway stays
  authoritative.
- **TLS client-file options unsupported over the bridge** — refused with
  a clear error (L5); supporting them needs a protocol extension.

## 6. Review 2026-11-04 (second pass — what the consolidated review missed)

A read-only pass over the committed bridge found the items below; the
statements in §1/§4/§5 that they contradict are corrected in place. Fixes
are server-side (the gateway is authoritative — client checks such as L5
are UX only, a hostile client skips them).

- **R1 (high) SSRF via topology discovery.** Bug: `EgressPolicy` checked
  only the seed hosts written in the URI; `replicaSet=`, `directConnection=false`
  or several seeds make libmongoc dial every member a server lists in its
  `hello` reply — a public fake mongod can point the gateway at
  `10.x`/`127.0.0.1`/`169.254.169.254`, and the driver's per-host error text
  came back as a port-scan oracle. Fix (interim): on a public gateway
  (`allowPrivate` off) the URI must name exactly one host, `replicaSet` /
  `loadBalanced` / `directConnection=false` are refused, and the URI handed
  to the driver is rewritten with `directConnection=true`. The rewrite is
  re-parsed and compared with the original (hosts, credentials, database,
  every other option) — any difference fails closed. Edge cases: replica
  sets are reachable through a public gateway only member-by-member; LAN
  gateways (`allowPrivate`) keep discovery. Planned structural fix: dial
  through a libmongoc stream initiator (`mongoc_client_set_stream_initiator`)
  that resolves, checks and connects to the checked IP — that also closes
  the DNS TOCTOU (§5) and makes SRV possible (R6).
- **R2 (high) 1 MiB frame cap enforced too late.** Bug: Qt's default
  incoming limit is ~2 GiB; the session's size check ran only after Qt had
  assembled the whole message, so 8 pre-auth sockets could make the
  gateway buffer gigabytes. Fix: `setMaxAllowedIncomingMessageSize` /
  `FrameSize(kMaxFrameBytes)` on every session socket (Qt closes with 1009
  while reading); the client caps responses at `kMaxResponseBytes`. The
  post-assembly check stays as a second line.
- **R3 (high) URI-controlled driver timeouts.** Bug: `connectTimeoutMS` /
  `socketTimeoutMS` / `serverSelectionTimeoutMS` came from the client's URI
  (up to `INT32_MAX` ms), and `maxTimeMS` binds only a cooperating server —
  a fake mongod that never answers held the shared event loop for days.
  Fix: the gateway clamps them to 10 s / 55 s / 15 s (a smaller user value
  wins; `0` means "no limit" and is clamped), and refuses URIs with more than
  16 hosts before any DNS lookup. Edge: `socketTimeoutMS` stays above the
  50 s `maxTimeMS` cap so a legitimate slow command gets the server's error,
  not a socket timeout.
- **R4 (medium) `maxTimeMS: 0` bypassed M1.** Bug: a user value of `0`
  (MongoDB: no limit) or a negative/NaN/fractional value was forwarded; NaN
  hit an undefined `double→int64` cast. Fix: any non-numeric, non-finite or
  `< 1` value is replaced by the cap; the effective value is always an int32
  in `[1, 50000]`, written once at the end of the command (duplicates
  dropped).
- **R5 (medium) URI options that act on the gateway host.** Bug:
  `authMechanism=MONGODB-AWS` makes libmongoc fetch the *gateway's* cloud
  credentials (env, ECS, EC2 IMDS) and sign a request to the target;
  `tls*File` options read files on the gateway host. Fix: on public
  gateways only `SCRAM-SHA-1` / `SCRAM-SHA-256` (or no mechanism) are
  accepted, `authMechanismProperties` is refused, and every `tls*`/`ssl*`
  option other than `tls`/`ssl` itself is refused (`tlsCAFile`,
  `tlsCertificateKeyFile*`, `tlsInsecure`, `tlsAllowInvalid*`,
  `tlsDisable*`). This also closes the `tlsInsecure` residual for hosted
  mode. Edge: custom-CA targets need a LAN/self-hosted gateway.
- **R6 (medium) `mongodb+srv://` on public gateways.** Bug: libmongoc does
  not resolve SRV while parsing, so `hosts()` was empty and every `+srv`
  URI failed with a misleading "lists no hosts"; the docs claimed SRV
  answers were checked. Checking the seed name instead would validate the
  wrong host (answers and SRV polling are dialed unchecked). Fix for now:
  public gateways refuse `+srv` with an explicit "not supported on public
  gateways yet" error; LAN gateways keep it. Real support waits for the
  stream initiator (R1).
- **R7 (medium) slow reader → unbounded buffering, leaked slot.** Bug:
  replies were queued without backpressure or size cap; a client that
  stops reading grows gateway memory, and Qt's graceful close then waits
  forever for the queue to drain, so `finished()` never fired and the
  session/ledger slot leaked. Fix: a new frame is refused (socket aborted)
  while more than 8 MiB are still unsent; a reply larger than
  `kMaxResponseBytes` becomes an error frame; every close (ours or Qt's
  1009) arms a 5 s timer that `abort()`s a peer which has not gone away, so
  the slot is always freed.
- **R8 (low-medium) hosted session outlived the JWT.** Bug: the
  entitlement was checked only at hello and any frame renewed the idle
  lease, so a session lived on after `exp` / plan cancellation. Fix: the
  session closes at `exp` and refuses frames after it. Edge: revocation
  before `exp` (no `jti` check) remains — bounded by the token TTL.
- **R9 (low) `.br` sibling escaped the web root.** Bug: only the requested
  file was canonicalized; `x.js.br` was opened directly, so a symlink
  sibling pointing outside the root was served to `br` clients. Fix: the
  `.br` path goes through the same canonical containment check
  (`WebRoot::containedPath`).
- **R10 (low) client desync after an op timeout.** Bug: the late reply of
  a timed-out request arrived during the next call and failed it with
  "Response id mismatch.". Fix: replies with an older, non-zero id are
  dropped and the call keeps waiting (id 0 — the session's own close
  error — still ends the call).
- **R11 (perf, not fixed)** — every result is parsed and re-serialized by
  `encodeOk` and again by `decodeResponse`; splicing the EJSON text into
  the envelope is future work.

## 7. Terms-of-service addendum

`ragoshell-site/content/legal/{en,tr}/terms.md` gained **§4.1 Hosted
WebSocket Bridge (`{bridgeHost}`)**: transiting-data disclosure, TLS-only
and public-IP target restrictions, per-device session caps, operational
logging scope (no credentials/URIs/payloads), and the self-hosted
alternative. Site gates: `npm run check`, `npm run build`,
`check:dist` — all clean.

## 8. Ops checklist left to `(insan)`

- Deploy hosted gateway behind nginx with TLS termination, per-IP
  connection/rate limits, and `RAGOSHELL_GW_AUTH=hosted` + real
  `GW_ENT_KEYRING`.
- `docker build` smoke test on CI; pin `ubuntu:24.04` by digest; consider
  `read_only`+`tmpfs` in the deployment compose.
- Browser smoke matrix (Chrome/Safari/Firefox) for the real WASM client
  against each channel.
- Plan the transport-routed shell/monitor work when it is scheduled
  (§5 first bullet) — it removes the M2 client-side refusals.
