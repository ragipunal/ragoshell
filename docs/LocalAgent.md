# Mod C Local Agent — `ragoshell-agent` on 127.0.0.1 (roadmap 8.3, WebAssembly.md Faz 4.1–4.2)

Written: 2026-11-03 · High tier (new network surface: a loopback WebSocket
service reachable by every website in the browser). Plan + threats before code.

Mod A (8.1) gave the browser build an in-memory demo. Mod C lets the same WASM
UI drive a real MongoDB on the user's own machine: a small desktop-side binary
(`ragoshell-agent`) bridges browser `WebSocket` ↔ native `mongocxx` on
`127.0.0.1` only. Credentials never leave the machine (Postman-agent model).

## 1. Wire protocol (`core/transport/GatewayProtocol`)

One JSON text frame per message, symmetrical with `IMongoTransport` ops:

- Request  `{"id": <u32>, "op": <name>, "params": {<op args as EJSON>}}`
- Response `{"id": <u32>, "ok": true,  "result": <IMongoTransport::Result shape>}`
- Error    `{"id": <u32>, "ok": false, "error": "<message>"}`

Ops: `hello` (auth handshake, §2), `connect`, `runCommand`, `listDatabases`,
`listCollections`, `ping`, `killOp`, `disconnect`, `isLive` — names and arg
shapes come straight from `IMongoTransport` so `NativeMongoTransport` on the
agent and `WebSocketGatewayTransport` on the client stay byte-compatible.

## 2. Agent binary (`src/ragoshell-agent/`)

`QCoreApplication` + `QWebSocketServer`:

- **Bind**: `127.0.0.1` only — never `0.0.0.0`. Port `17371` (constant in
  `GatewayProtocol.h`, overridable via `RAGOSHELL_AGENT_PORT` for dev).
- **Pairing token**: 32-byte CSPRNG hex, generated once, written
  `0600` to `~/.ragoshell/agent-token` (`appDataDir()`), printed to stdout.
  The user pastes it into the web UI's *Yerel Ajan* channel. First frame must
  be `{"op":"hello","params":{"token":…}}` within a short deadline or the
  socket closes; compared in constant time. The agent refuses to start when
  the RNG fails — an empty token would pass the constant-time compare.
- **Origin check** (DNS-rebinding defence): `QWebSocket::origin()` must be in
  the allowlist — `https://ragoshell.com.tr`, `https://app.ragoshell.com.tr`,
  `https://www.ragoshell.com.tr`, `http(s)://localhost[:port]`,
  `http(s)://127.0.0.1[:port]` (dev). Anything else → close before hello.
- **Host check**: `request().rawHeader("Host")` must be
  `127.0.0.1:<port>` or `localhost:<port>` — a rebinding attack that spoofs
  Origin still can't spoof the loopback Host.
- **Limits** (constants): max 8 sockets, max 1 MiB text frame, 30 s hello
  deadline, 30 min idle timeout (reset per frame), one
  `NativeMongoTransport` per socket — DB state is per-session. Rejected
  sockets still emit `finished()` on close, so a refused probe never leaks a
  session slot. Ops run synchronously on the agent's single event loop — one
  slow `runCommand` stalls all sessions (accepted for a local dev tool).
- **No TLS**: loopback plaintext is the model; `wss` would need a browser-
  trusted localhost cert nobody has. Token keeps other local users/sites out.

`GatewaySession` (core/transport) is the testable core: constructed with a
`QWebSocket*`, an `IMongoTransport` factory and the expected token; the agent
`main.cpp` only binds the port, mints/loads the token and wires sessions.

## 3. Client transport (`core/transport/WebSocketGatewayTransport`)

Implements `IMongoTransport` over `QWebSocket` (works native *and* on wasm —
Qt maps it to the browser WebSocket API). Ops are synchronous per the
interface: each request spins a short-lived `QEventLoop` until the matching
`id` response or socket close (timeout per op constant). Socket is created
inside `connect()` so it lives on the worker thread that calls it.

`ConnectionSettings`: `transportKind` gains `agent` (= `ws://127.0.0.1:17371`)
and `ws://`/`wss://<url>` forms; `transportToken` feeds the hello frame and
is serialized as `transportTokenEncrypted` (same `RagoCrypt` `*Encrypted`
convention as `userPasswordEncrypted`; a plaintext `transportToken` key is
still read for migration). `MongoWorker`'s factory creates the transport for
these kinds (`TransportSeam.md` §4 already reserves this path) and **refuses
cleartext `ws://` to any non-loopback host** — the URI and token would cross
the wire unencrypted; remote gateways must use `wss://`.

## 4. QML (item 4.2 — Bağlantı Kanalı)

`ConnectionDialog` gains a channel combo, **wasm-only**
(`visible: Qt.platform.os === "wasm"`): `Demo (in-browser)` /
`Yerel Ajan (localhost)` / `Özel Gateway (ws://…)`. Picking agent reveals a
token field → `transportKind`/`transportToken`; custom reveals a URL field.
Desktop UI unchanged (combo hidden; native driver stays the channel).

## 5. Threat list

| # | Threat | Mitigation |
|---|---|---|
| T1 | **DNS rebinding**: evil.com resolves to 127.0.0.1, browser WS from hostile origin | Origin allowlist (checked pre-auth) + Host must be loopback |
| T2 | **Port squatting**: another local service on 17371 | Pairing token required on first frame; a squatter without the token serves nothing useful; agent refuses to start without binding 127.0.0.1 specifically |
| T3 | **Token brute-force** | 256-bit token, constant-time compare, 30 s handshake window, socket close on failure — no retry oracle worth having |
| T4 | **Token theft** from disk | File `0600` under the per-user config dir (permissions set before content is written; agent warns if an existing file is group/other-readable); the token itself is printed once at startup for pairing, never logged elsewhere |
| T5 | **Oversized frame** memory exhaustion | 1 MiB text-frame cap → close. Residual: Qt assembles the whole message before the check fires, so the cap bounds per-op memory, not receive-buffer memory. Binary frames are refused outright (`PolicyViolated` close) |
| T6 | **Socket exhaustion** | 8 concurrent sockets; idle 30 min timeout; rejected sockets emit `finished()` and free their slot |
| T7 | **CORS/CSRF** | WS has no CORS — Origin check is the control; no HTTP endpoints exist |
| T8 | **Credentials in logs** | Agent logs connection lifecycle only; command bodies never logged (same discipline as CloudClient) |
| T9 | **MITM on loopback** | Out of scope by model (no TLS on 127.0.0.1); documented |
| T10 | **Malformed JSON / unknown op** | Parse failure → error frame, not crash; unknown op → `ok:false` |
| T11 | **EJSON type confusion** crossing into mongocxx | `NativeMongoTransport` already validates/canonicalises via bsoncxx; agent adds nothing new |
| T12 | **Cleartext `ws://` to a remote host** leaks the Mongo URI + token on the wire | `MongoWorker`'s factory refuses non-loopback `ws://` hosts; remote gateways must be `wss://` (8.4/8.5) |

## 6. Test plan (native, `rago_unit_tests`)

- `GatewayProtocol_test`: encode/decode round-trips, malformed frames,
  id echo, unknown op rejection.
- `WebSocketGatewayTransport_test` (in-process E2E): spin a
  `QWebSocketServer` + `GatewaySession` with an `InMemoryDemoTransport`
  factory on `127.0.0.1:0`; a real `WebSocketGatewayTransport` client:
  hello→connect→runCommand/find→listDatabases→disconnect. Wrong token →
  close; wrong Origin → close + slot freed; wrong Host → close; >1 MiB
  frame → close; binary frame → close; idle → close; pipelined ids → each
  answered by id; disconnect→reconnect works.
- Native suite stays green; wasm build must link (QWebSocket exists there).

## 7. Out of scope

- Packaged installers/auto-start (`LaunchAgent`/service) for the agent
  binary — a follow-up packaging item.
- `wss://` public gateway semantics — that is 8.4/8.5.
- Agent auto-update — the binary ships in desktop releases, updated with them.
