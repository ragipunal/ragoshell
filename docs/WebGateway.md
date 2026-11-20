# Mod B Web Gateway — `ragoshell-web-gateway` + self-hosted Docker (roadmap 8.4, WebAssembly.md Faz 3)

Written: 2026-11-03 · High tier (public-reachable network service: SSRF, auth,
quotas). Plan + threats before code.

Mod C (8.3) put a loopback agent between the WASM build and a local MongoDB.
Mod B is the same protocol seam pointed outward: a small Qt service that
serves the static WASM bundle over HTTP **and** terminates `wss://` gateway
sessions, so a browser client can drive a remote MongoDB (Atlas, company
server) without installing anything. Ships as one Docker image
(`ragoshell/web`) for self-hosted use; the hosted SaaS variant (8.5) reuses
it behind the API's auth.

## 1. Components

| Piece | Where | Reused from 8.3? |
|---|---|---|
| Wire envelope + limits | `core/transport/GatewayProtocol` | yes, unchanged |
| Session handler | `core/transport/GatewaySession` | yes + **egress hook** |
| Native DB path | `core/transport/NativeMongoTransport` | yes |
| Client transport | `core/transport/WebSocketGatewayTransport` | done in 8.3 (Faz 3.2) |
| **SSRF egress policy** | `core/transport/EgressPolicy` *(new)* | — |
| Gateway binary | `src/ragoshell-web-gateway/` *(new)* | agent's skeleton |
| Docker image | `src/ragoshell-web-gateway/docker/` | — |

## 2. `EgressPolicy` (SSRF guard, Faz 3.3)

Applied **in `GatewaySession` on the `connect` op**, before the transport is
touched: `params.settings` is `{"uri":"mongodb(srv)://…"}`. The policy:

- parses the URI with `mongocxx::uri`; more than 16 hosts are refused
  before any lookup (each lookup blocks the event loop);
- on a public gateway (no `ALLOW_PRIVATE`) the URI must name **exactly one
  host** and `replicaSet` / `loadBalanced` / `directConnection=false` are
  refused: with discovery the driver dials whatever members a server lists
  in its `hello` reply, none of which this check saw (2026-11-04 review R1,
  `BridgeSecurityReview.md` §6);
- `mongodb+srv://` is refused on public gateways ("not supported on public
  gateways yet"): libmongoc does not resolve SRV while parsing, so there is
  no host list to check, and the SRV answers it dials later would bypass
  the address check (R6). LAN gateways (`ALLOW_PRIVATE`) still accept it;
- public gateways accept only `SCRAM-SHA-1` / `SCRAM-SHA-256` (or no
  `authMechanism`) and refuse `authMechanismProperties` plus every
  `tls*`/`ssl*` option other than `tls`/`ssl` (`tlsCAFile`,
  `tlsCertificateKeyFile*`, `tlsInsecure`, `tlsAllowInvalid*`,
  `tlsDisable*`) — `MONGODB-AWS` would fetch the gateway host's own cloud
  credentials and the file options read the gateway's disk (R5);
- resolves the host via `QHostInfo::fromName` — **every** resolved address
  must be public; a lookup failure fails closed;
- the driver never sees the client's URI: it gets a rewrite with
  `connectTimeoutMS` ≤ 10 s, `socketTimeoutMS` ≤ 55 s,
  `serverSelectionTimeoutMS` ≤ 15 s (a smaller user value wins, `0` is
  clamped) and, on public gateways, `directConnection=true`. The rewrite is
  re-parsed and must match the original in hosts, credentials, database and
  every other option, or the connect fails closed (R1/R3);
- rejects (default): loopback `127.0.0.0/8`/`::1`, RFC1918 `10/8`,
  `172.16/12`, `192.168/16`, link-local `169.254/16` + `fe80::/10` (cloud
  metadata), CGNAT `100.64/10`, ULA `fc00::/7`, IPv4-mapped forms of the
  same, `0.0.0.0`, multicast/reserved;
- `RAGOSHELL_GW_ALLOW_PRIVATE=1` disables the range check for self-hosted
  LAN deployments (logged loudly at startup).
- Non-canonical IP literals (octal `0177.0.0.1` = `127.0.0.1` to
  `getaddrinfo` but `177.0.0.1` to Qt) never take the literal fast path —
  anything Qt's parser canonicalizes differently is resolved the same way
  the driver resolves it. IPv6 transition mechanisms that embed IPv4
  (NAT64 `64:ff9b::/96`, 6to4 `2002::/16`, Teredo `2001::/32`, site-local
  `fec0::/10`, v4-compatible `::/96`) are blocked outright.
- The `settings` JSON is parsed with **libbson — the same parser
  `NativeMongoTransport` reads** — so a duplicate `uri` key can never make
  the checked URI disagree with the dialed one (QJsonDocument keeps the
  last key, libbson the first).
- Address/DNS denials return the generic `Egress denied.` to the client;
  the resolved IP / DNS detail stays in the server log (private-namespace
  oracle). Policy refusals (TLS, `+srv`, options, host count) carry their
  specific reason — they reveal nothing about the network.

Residual TOCTOU: `mongocxx` re-resolves inside the driver after our check —
a fast-flux DNS name can in theory flip between the two. Planned structural
fix: dial through a libmongoc stream initiator
(`mongoc_client_set_stream_initiator`) that resolves, checks and connects
to the checked IP itself (keeping the hostname for TLS SNI). That one
change also lifts the single-host rule (members are checked as they are
dialed) and makes SRV possible on public gateways.

## 3. `ragoshell-web-gateway` binary

`QCoreApplication` hosting a single `QAbstractHttpServer` (`QHttpServer` +
`QTcpServer`) on `RAGOSHELL_GW_PORT` (default `8080`): Qt 6.12 multiplexes
static GET and WebSocket upgrade on one port via
`addWebSocketUpgradeVerifier` + `nextPendingWebSocketConnection()` — the
upgraded `QWebSocket` goes straight into the same `GatewaySession` used by
the local agent. `RAGOSHELL_GW_BIND` picks the listen address (default
`0.0.0.0` — self-hosted; `127.0.0.1` for dev).

- **WS path** `/ws` (constant `RAGOSHELL_GW_WS_PATH`, default `/ws`): the
  upgrade verifier re-checks path + Origin allowlist at the HTTP layer
  before `Accept` (defense-in-depth under `GatewaySession`'s own gate,
  which reads `origin()`/`request()` from the upgraded socket — verified
  populated at runtime). The Host header is intentionally NOT checked on
  this deployment: a public gateway cannot enumerate every name it is
  reached by, and the Origin allowlist carries the CSWSH threat alone.
- **HTTP** (everything else): serves the static WASM bundle from
  `RAGOSHELL_WEB_ROOT` (default `./web`, Docker: `/srv/web`). Sanitized path
  (single decode, no `..`/NUL, canonicalized under web-root) → file.
  Headers on **every** response:
  - `Cross-Origin-Opener-Policy: same-origin`
  - `Cross-Origin-Embedder-Policy: require-corp` (SharedArrayBuffer /
    `wasm-multithread` precondition)
  - `Cross-Origin-Resource-Policy: same-origin`
  - `Content-Security-Policy: default-src 'self'; script-src 'self'
    'wasm-unsafe-eval'; connect-src 'self' wss: ws:; worker-src 'self'
    blob:; img-src 'self' data:; style-src 'self' 'unsafe-inline';
    font-src 'self'; frame-ancestors 'none'; base-uri 'self'`
  - `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer`,
    `X-Frame-Options: DENY`
  - `Cache-Control`: `no-cache` for `index.html`, `max-age=86400` for the
    hashed assets (`.br` siblings are served with `Content-Encoding: br`
    only when the client lists `br` with q>0 in `Accept-Encoding`)
  - MIME: `.wasm → application/wasm`, `.js`, `.css`, `.html`, `.json`,
    `.png/svg/ico/woff2`, default `application/octet-stream`.
- **TLS**: Qt exposes no public SSL-terminating `QTcpServer` for
  `QAbstractHttpServer`, so the gateway itself speaks cleartext HTTP/WS —
  designed for TLS termination at a reverse proxy (Caddy/nginx/CF Tunnel),
  which is also the only way `wss://` works from an `https://` page anyway.
  `EXPOSE 8080` only.

## 4. AuthN/Z (self-hosted scope)

- `RAGOSHELL_GW_TOKEN` — required, **≥ 32 chars** (shorter → the binary
  refuses to start). Empty → refuse unless `RAGOSHELL_GW_OPEN=1` is set
  explicitly (dev escape hatch, logged as a warning). Same hello handshake
  as Mod C: first frame `{"op":"hello","params":{"token":…}}`,
  constant-time compare.
- `RAGOSHELL_WEB_ROOT` must exist — an unresolvable web root would
  degenerate the static path check into "serve anything under CWD", so
  startup fails fast instead.
- Known limits for a public deployment: DNS resolution in `EgressPolicy`
  is synchronous on the event-loop thread (one `connect` op can stall all
  sessions for the resolver timeout); `NativeMongoTransport` ops run on
  the same loop — a single command is capped server-side at `maxTimeMS`
  50 s (8.6 review M1; `0`/NaN no longer bypass it, R4), and every driver
  wait is bounded by the clamped URI timeouts above (a hostile server that
  never answers can still hold the loop for up to 55 s per op); the
  8-session cap
  counts **pre-auth** sockets; each static GET buffers the whole file in
  memory (`readAll()` — bounded by bundle size); and Qt's built-in 404
  for non-GET methods / paths deeper than four segments carries no
  security headers (cosmetic — the bodies are static). Put the gateway
  behind a reverse proxy that adds per-IP connection/rate limits; the
  hosted variant (8.5) owns these.
- Session limits are the 8.3 constants: 8 sockets, 1 MiB frame (enforced
  by Qt while reading, R2), 30 s hello, 30 min idle, per-session
  `NativeMongoTransport`; plus 48 MiB per reply (larger → error frame),
  8 MiB of unsent replies before a non-reading peer is aborted, and a 5 s
  abort after any close so a stalled peer always frees its slot (R7).
- TLS: none inside the gateway — Qt 6.12 has no public SSL-terminating
  `QTcpServer` for `QAbstractHttpServer`, so `wss://` is always reached
  through a terminating reverse proxy (§3).
- `RAGOSHELL_GW_ORIGINS` — comma-separated extra allowed origins on top of
  the built-in list (`ragoshell.com.tr`/`app.`/`www.` + `http(s)://localhost*`
  /127.0.0.1 dev defaults). The gateway does **not** derive its own served
  origin automatically (8.6 review L9): a self-hosted deployment behind a
  custom host name must list it here or every upgrade is denied `403`.

## 5. Dockerfile (`src/ragoshell-web-gateway/docker/Dockerfile`)

Multi-stage, `ragoshell/web`:

1. **builder** — `ubuntu:24.04`. `addWebSocketUpgradeVerifier` requires
   Qt ≥ 6.8 but Ubuntu 24.04 ships 6.4.2, so the stage installs the dev
   build's pinned Qt **6.12.0** via `aqtinstall` (base + httpserver +
   websockets + declarative + 5compat) and vcpkg for
   `mongo-cxx-driver`/openssl/argon2/libssh2 → configures with
   `-DRAGO_GATEWAY_ONLY=ON` (skips Qt Quick, the desktop app, tests) and
   builds only `ragoshell-web-gateway`. Long but reproducible; gated by
   CI, not run locally every phase.
2. **runtime** — `ubuntu:24.04` + the Qt runtime copied from the builder;
   `COPY` the stripped binary and the WASM bundle produced locally by
   `cmake --preset wasm-single && cmake --build --preset wasm-single`
   (`build/wasm-single/src/ragoshell-app/{ragoshell.{js,wasm,wasm.br},
   qtloader.js, qtlogo.svg}` — `ragoshell.html` is copied in as
   `index.html`). `EXPOSE 8080`, non-root uid 10001, `ENTRYPOINT
   ["/usr/local/bin/ragoshell-web-gateway"]`.

## 6. Threat list

| # | Threat | Mitigation |
|---|---|---|
| G1 | **SSRF**: browser asks gateway to connect to `169.254.169.254` (cloud metadata), internal DBs, `localhost:22`… | `EgressPolicy` resolve+range check on the single URI host before connect, forced `directConnection`, no discovery/`+srv`/host-side auth or file options; `ALLOW_PRIVATE` only when the operator opts in |
| G2 | **Open relay**: anyone reaching the gateway rides it into arbitrary MongoDBs | `GW_TOKEN` required (unless explicit `GW_OPEN`); hello deadline + constant-time compare (8.3) |
| G3 | **CORS/CSWSH** | Origin allowlist (`GW_ORIGINS` + dev loopback) pre-auth; WS has no CORS — Origin is the control |
| G4 | **XSS via static server** | strict CSP (`script-src 'self' 'wasm-unsafe-eval'` — no inline/eval), `nosniff`, path sanitization under web-root |
| G5 | **Credential leakage** | token/URI never logged; `connect` payload carries the full `mongodb://` URI in-frame only; `transportTokenEncrypted` on the client (8.3) |
| G6 | **Resource exhaustion** | 8 sockets, 1 MiB frames enforced while reading, 48 MiB replies, 8 MiB write backlog, close→abort grace, idle/hello/op timeouts (8.3), clamped driver timeouts, ≤16 URI hosts (2026-11-04 review) |
| G7 | **Clickjack/mixed content** | `frame-ancestors 'none'`, `X-Frame-Options`, COOP/COEP CORP headers |
| G8 | **Cleartext credentials on the wire** | `wss://` only via a terminating reverse proxy (no TLS inside the gateway); HTTP static content is harmless to sniff (public bundle) but tokens must never cross `ws://` off-loopback — `MongoWorker` already refuses non-loopback `ws://` (8.3) |
| G9 | **Path traversal** in static route | single decode, `..`/NUL rejected, canonicalized under web-root — the `.br` sibling too (`WebRoot::containedPath`, R9) |
| G10 | **docker build context leak** | `.dockerignore` (repo secrets, build dirs) |

## 7. Tests (`rago_unit_tests`)

- `EgressPolicy_test`: private/loopback/link-local/CGNAT/ULA/IPv4-mapped
  rejected; public IP + public hostname allowed; `allowPrivate` flips;
  bad URI / unresolvable host fail closed; seedlist with one bad host
  rejects the whole URI.
- `WebSocketGatewayTransport_test` additions: session with egress policy →
  `connect` to `mongodb://127.0.0.1` returns an error frame; same session
  allows `mongodb://<public>` — skipped if DNS unavailable (CI-safe:
  use literal public IPs for allow-path, literal private for deny-path).

## 8. Out of scope

- Hosted/Pro auth via `ragoshell-api` entitlements (8.5).
- TLS-automation (ACME) — operator supplies cert or reverse proxy.
- Session admission quotas beyond the 8.3 constants (per-IP rate limits —
  8.5, where the gateway is public).
