# Transport Seam — MongoWorker/MongoClient → IMongoTransport refactor plan

Roadmap 8.1 · companion to `docs/WasmBuildReport.md` §3 · status: plan ready,
implementation pending a high-effort session.

## 1. Goal

All wire traffic between RagoShell and a MongoDB server crosses
`IMongoTransport` (`src/ragoshell/core/transport/`), so the same UI+worker code
runs on:

- `NativeMongoTransport` — desktop, mongocxx, TCP+TLS (done: adapter written,
  4 tests; not yet wired into MongoWorker).
- `InMemoryDemoTransport` — WASM Mod A playground, canned dataset (done: 7 tests).
- `WebSocketGatewayTransport` — Mod B/C (Phase 8.4, not started).

The seam is the **client factory**, not the BSON stack: bsoncxx stays in the
build on every platform (domain objects, events, utils all use it). Only
"how bytes reach a server" changes.

## 2. Chosen design — MongoClient as a dual-backend façade

`MongoClient` keeps its ~28-method API (handlers in `MongoWorker*.cpp` stay
unchanged). Internally it holds either `mongocxx::client*` (today) **or**
`IMongoTransport*` (new ctor). Each method translates to an EJSON wire command
when the transport backend is active; the native path is byte-identical to
today's code.

Rejected alternatives:

- **Rewrite handlers to EJSON.** ~25 handlers × typed MongoClient calls →
  duplicated parsing on both sides, bigger blast radius.
- **Compile-out mongocxx on WASM.** Wrong split — bsoncxx/mongocxx headers are
  woven through ~60 files; bsoncxx itself is Emscripten-safe. What must not
  reach WASM is the *socket open*, i.e. the factory.
- **Second MongoWorker.** Two workers to keep in sync forever.

## 3. Method → wire-command mapping

| MongoClient method | Wire command(s) |
|---|---|
| `getCollectionNamesWithDbname`, `listCollectionInfos` | `{listCollections:1,nameOnly}` |
| `getDatabaseNames` | `{listDatabases:1}` or `client.list_databases` |
| `getVersion`, `dbVersionStr`, `getStorageEngineType` | `{serverStatus:1}` / `{hello:1}` |
| `getUsers` | `{usersInfo:1}` (already via `runCommandChecked`) |
| `createUser`, `dropUser` | `{createUser:…}` / `{dropUser:…}` |
| `getFunctions` | `{find:"system.js"}` on the db |
| `getIndexes` | `{listIndexes:<coll>}` cursor |
| `addEditIndex`, `renameIndexFromCollection`, `dropIndexFromCollection` | `{dropIndexes:…}`, `{createIndexes:…}`, `{collMod:…}` |
| `createFunction`, `dropFunction` | `insert`/`delete` on `system.js` |
| `createDatabase`, `dropDatabase` | `insert`+`drop` (temp collection) / `{dropDatabase:1}` |
| `createCollection`, `renameCollection`, `duplicateCollection`, `dropCollection` | `{create:…}`, `{renameCollection:…}`, `find`+`insert` copy, `{drop:…}` |
| `insertDocument` | `{insert:<coll>, documents:[…]}` |
| `saveDocument` | `{update:<coll>, updates:[{q:{_id:…},u:…,upsert:true}]}` |
| `removeDocuments` | `{delete:<coll>, deletes:[{q:…,limit:0|1}]}` |
| `query` | `{find:<coll>,filter,skip,limit,batchSize,projection}` + `getMore` paging loop |
| `runCollStatsCommand` | `{collStats:…}` (currently stub-ish anyway) |
| `ping` / monitor | `{ping:1}`, `{serverStatus:1}` via `runCommand` |
| `killOp` | `{killOp:1,op:<id>}` |

## 4. Worker-side changes

- `MongoWorker` gains `std::unique_ptr<IMongoTransport> _transport` and a
  `setTransportFactory(std::function<…>)` hook (default: native).
- `getConnection()`: native → unchanged `mongocxx::client` creation;
  transport → `connect({uri,timeoutMS})` EJSON payload; `MongoClient` then
  constructed with the transport.
- `pingDatabase`, `getReplicaSetInfo`, `connectAndGetReplicaSetName`:
  route `{ping:1}`/`{hello:1}`/`{replSetGetStatus:1}` through the transport.
- `ScriptEngine`/`JsShellBackend` (`BsonJsConverter`) evaluate shell scripts
  against `MongoClient`-shaped calls — keep them behind the façade, not the
  transport.
- ConnectionSettings gains `transportKind` (`native|memory|ws://…`); the demo
  playground sets `memory` so no URI/credentials are ever sent.

## 5. Threats / edge cases (High-tier list, per rules)

- **Error fidelity**: wire `{ok:0,errmsg,code,codeName}` must surface as the
  same exceptions callers catch today (`mongocxx::exception` shape) or error
  text/telemetry regress. Convert `Result`→a `TransportException` carrying the
  triplet; `runCommandChecked` must still throw on `ok==0`.
- **Cursor semantics**: `find`/`getMore`/`killCursors` round-trips lose
  server-side cursor state in `InMemoryDemoTransport` (stateless canned data —
  return single-batch cursors only; `ns`+`id` must still be EJSON-shaped so
  the gateway can swap in real cursors later).
- **BSON fidelity**: relaxed EJSON loses `$date`/`$oid`/`$binary` typing.
  Use **canonical** extended JSON on the wire (not relaxed — fix
  `NativeMongoTransport` to `k_canonical` during wiring) so ObjectIds and
  dates round-trip; `from_json`/`to_json` must agree on both ends.
- **Auth/session state**: transport `connect()` is called once per worker.
  As implemented (8.3/8.4), the `connect` frame carries the full Mongo URI —
  credentials transit the `ws(s)://` channel, which is why remote gateways
  are `wss://`-only. `killOp`, replica-set commands, `system.js` writes are
  admin-scoped and execute server-side through `runCommand` — the gateway
  enforces egress/TLS/auth but does not scope-check command names (8.6:
  documented residual; the client's own command surface is what it sends).
- **Secrets**: never serialize connection passwords/SSH keys into transport
  payloads beyond the initial `connect` — and in demo mode never at all.
- **Threading**: transports are synchronous on the worker thread (documented
  in `IMongoTransport`); `InMemoryDemoTransport` must not be shared across
  worker threads without a mutex (catalog maps are plain std:: containers).
- **Long-running ops**: `duplicateCollection` copies document-by-document —
  over a gateway each doc is a frame; cap or document the limit.
- **`getMore` lifetime**: in-memory transport has no cursor registry —
  `query()` over transport must drain `firstBatch` fully within one call.

## 6. Implementation order

1. `MongoClient` dual-backend internals + canonical EJSON wire — **done**
   (all methods carry a `_transport` branch; native path untouched;
   `runCommandChecked` maps the `{errmsg,code,codeName}` triplet onto
   `std::runtime_error`). Multi-batch cursors are single `firstBatch` for now;
   `getMore` paging remains for the gateway work.
2. `query()` via `find` over transport — **done** (`execCursor` drains
   `cursor.firstBatch`; demo returns one batch).
3. `MongoWorker` factory hook — **partial**: `setTransport()` installed and
   `getClient()` wraps it; `ConnectionSettings.transportKind` + demo-mode
   connect path remain.
4. UI affordance: `isLive()==false` hides account/admin-only actions.
5. Mod A playground wiring (site page + `.wasm.br`), needs Emscripten SDK.

## 7. Test plan

- Existing `MongoWorker`/handler unit tests must pass unchanged on the native
  path (regression guard).
- New: `MongoClient` over `InMemoryDemoTransport` — `getDatabaseNames`,
  `listCollectionInfos`, `query` (filter+limit), `insertDocument`+
  `removeDocuments` round-trip on the demo catalog, error path (`ok:0`).
- Canonical-EJSON round-trip test: ObjectId/date/binary survive
  `bson→ejson→bson`.
- WASM configure smoke (`emcmake cmake --preset wasm_singlethread`) once the
  Emscripten SDK is installed — not installed on this machine yet.

## 8. Gate list for the implementation session

`rago_unit_tests` (all), `ragoshell --check-qml` (if QML touched), preset
build (`macos-release` + `wasm_singlethread` configure when SDK exists),
security-review subagent on the diff, then §11 tick.
