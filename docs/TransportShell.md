# Transport-Backed JavaScript Shell & Enriched In-Memory Demo Engine (`W4`)

Status: design & threat model for Roadmap **§11 `W4`** (companion to `docs/TransportSeam.md`,
`docs/Shell.md`, and `docs/BridgeSecurityReview.md`).

---

## 1. Goal & Scope

Phase `8.1`–`8.6` routed the Explorer tree and `MongoClient` C++ helpers through `IMongoTransport`, while
leaving the embedded JavaScript shell (`ScriptEngine` → `JsShellBackend` → `ShellHost`) bound to a native
`mongocxx::client` and refusing shell tabs whenever `ConnectionSettings::hasTransport()` was set (`8.6`
finding `M2`).

`W4` completes the transport seam across the interactive shell and enriches `InMemoryDemoTransport` so that:

1. **Mod A (WASM Live Playground on `demo.<domain>`):** Opening a collection or shell tab runs the real
   `QJSEngine` + `shell-runtime.js` (`db.products.find(...)`, `aggregate(...)`, `insertOne`/`updateOne`/
   `deleteOne`/`findOneAndUpdate`/`bulkWrite`, `distinct`, `createIndex`/`getIndexes`, `stats()`,
   `explain()`, autocomplete, Tree/Table/Text result views, paging, and `DocumentEditorDialog`) completely
   in browser RAM with **lossless BSON types** (`ObjectId`, `NumberLong` / `int64`, `NumberDecimal` /
   `Decimal128`, `ISODate`, `RegExp`, `BinData`).
2. **Mod B / Mod C (Self-Hosted WebSocket Gateway & Local Agent):** Opening a shell tab over an `agent` or
   `ws://`/`wss://` connection drives `ShellHost` and secondary-worker C++ paging/CRUD over
   `WebSocketGatewayTransport` on the worker's `QThread`, never bypassing the configured channel.
3. **`ServerMonitor` remains native-only:** `QmlBackendUtils.cpp` keeps refusing `ServerMonitor` on
   `hasTransport()` connections (`serverStatus` live polling is not part of the Playground or gateway UX).

---

## 2. Step-by-Step Architecture

### 2.1 Secondary `MongoWorker` Transport Lifecycle (`MongoWorker`)

`App::openShell` clones `ConnectionSettings` and opens a `ConnectionSecondary` `MongoServer` owning its own
`MongoWorker` on a dedicated `QThread`. Previously, `MongoWorker::handle(EstablishConnectionRequest *)`
returned early for `ConnectionSecondary` *before* constructing `_ownedTransport`, which meant any subsequent
`getClient()` call on a secondary worker would have fallen through to `getConnection()` (`mongocxx::client`).

- Extract `IMongoTransport *MongoWorker::ensureTransportConnected(std::string *outError = nullptr)`:
  - Invoked only on `MongoWorker::_thread` when `_connSettings->hasTransport()` is `true`.
  - Constructs `_ownedTransport` using the exact channel rules of `EstablishConnectionRequest`:
    - `"memory"`: `#if defined(Q_OS_WASM)` constructs
      `std::make_unique<InMemoryDemoTransport>(InMemoryDemoTransport::sharedStore())`; on non-WASM builds
      stays `nullptr` so a synced/imported `"memory"` record on desktop never masquerades as a real server
      (`8.6` `M3`).
    - `"agent"`, `"hosted"`, `"ws://…"`, `"wss://…"`: constructs `WebSocketGatewayTransport` on
      `MongoWorker::_thread` (where a `QThread` event dispatcher is active for `QWebSocket` + `QEventLoop`).
  - Serializes connection settings via `buildTransportSettingsJson(*_connSettings)` and calls
    `_ownedTransport->connect(...)`. On failure, strips `mongodb(+srv)://userinfo@` credentials (`8.6` `L7`),
    resets `_ownedTransport`, and returns `nullptr` with `*outError`.
- **Fail-closed callers on `MongoWorker::_thread`:**
  - `EstablishConnectionRequest` (primary / test / refresh) delegates transport setup to
    `ensureTransportConnected(&err)`.
  - `MongoWorker::getClient()` checks `if (_connSettings->hasTransport())`: calls
    `ensureTransportConnected(&err)` and throws `std::runtime_error` if `nullptr` — **never** falling through
    to `getConnection()` (`mongocxx::client`).
  - `MongoWorker::ensureScriptEngine()` checks `if (_connSettings->hasTransport())`: calls
    `ensureTransportConnected(&err)` and passes `(_transport, QString::fromStdString(err))` to
    `ScriptEngine::setTransport(...)` so `ScriptEngine::init()` fails closed (`failedScope() == true`) if
    transport setup fails. `MongoWorker::handle(ExecuteScriptRequest *)` and `retry(ExecuteScriptRequest *)`
    both route through `ensureScriptEngine()` under an RAII `ScriptFlightGuard` (`_inFlightOps` +
    `_scriptRunning`), suppressing `keepAlive()`, `AutocompleteRequest`, and mid-call `updateLocalServerPort`
    re-entrancy while a nested `QEventLoop` RPC is in flight on `WebSocketGatewayTransport`.
  - Member declaration and destructor order in `MongoWorker`: `_scriptEngine` (`~ShellHost` →
    `~CursorState::closeRemote()`) is destroyed strictly **before** `_ownedTransport` so remote cursor cleanup
    never touches a freed transport.

### 2.2 `ScriptEngine` → `JsShellBackend` → `ShellHost` Transport Seam

- **`ShellConnectOptions` (`ShellBackend.h`):**
  - Add `IMongoTransport *transport = nullptr;` (non-owning; lifetime owned by `MongoWorker` or unit test on
    the same thread) and `QString transportAddress;` (e.g. `"demo"` for `"memory"` or host/gateway label).
- **`ScriptEngine` (`ScriptEngine.h` / `.cpp`):**
  - Add `void setTransport(IMongoTransport *transport, const QString &transportError = {});` and populate
    `options.transport` + `options.transportAddress` in `buildConnectOptions()`. When
    `_connSettings->hasTransport()` is `true` and `_transport == nullptr`, `ScriptEngine::init()` fails closed
    immediately without constructing `mongocxx::client`.
- **`JsShellBackend` (`JsShellBackend.cpp`):**
  - When `options.transport != nullptr`:
    - Skip spawning a detached `std::thread` (which would violate `QWebSocket` thread affinity and is
      unnecessary because `ensureTransportConnected()` has already connected on `MongoWorker::_thread`).
    - Verify liveness via `options.transport->ping()`, call `_host->adoptTransport(options.transport, options)`,
      and run `_shell.property("configure")` (`dbName`, `batchSize`, `startupScript`).
  - In `stopRunningScript()`:
    - `_engine->setInterrupted(true)` still interrupts long/infinite JS loops from the UI thread or timeout
      watchdog.
    - `ShellHost::KillTarget` keeps `appName` empty in transport mode, so `ShellHost::killOperations` returns
      immediately without touching `_transport` across threads.
- **`ShellHost` (`ShellHost.h` / `.cpp`):**
  - `adoptTransport(IMongoTransport *transport, const ShellConnectOptions &options)` stores `_transport`,
    resets `_client`, sets `_seedHosts`, and clears `_appName`. `isConnected()` is
    `_client != nullptr || _transport != nullptr`.
  - **Canonical EJSON Bridge:**
    - Commands converted by `_bson.toDocument(jsCmd)` are serialized with
      `bsoncxx::to_json(cmd.view(), bsoncxx::ExtendedJsonMode::k_canonical)` and sent to
      `_transport->runCommand(db, ejson)`.
    - Replies are parsed back via `bsoncxx::from_json(...)` and converted to JS values with `_bson.toJs(...)`,
      preserving `ObjectId`, `Int32`, `Int64` (`NumberLong`), `Double`, `Decimal128` (`NumberDecimal`),
      `Date` (`ISODate`), `RegExp`, `BinData`, `MinKey`, `MaxKey`, `Timestamp`, and key order in
      `InMemoryDemoTransport` (note: over `WebSocketGatewayTransport`, `GatewayProtocol` wraps payloads in
      `QJsonObject`, which sorts keys alphabetically on wire frames).
    - When `IMongoTransport::Result::ok` is `false`, `throwTransportError(res)` throws a JS `Error` with
      `name = "MongoServerError"`, `.code` (when non-zero), and `.codeName` (when non-empty), matching native
      `throwQueryException`.
  - **Wire Command Mapping on `_transport`:**
    - `ping()` → `_transport->ping().ok`.
    - `runCommand(db, command)` → `_transport->runCommand(db, canonicalEjson)`. All shell writes
      (`insertOne`/`Many`, `updateOne`/`Many`, `replaceOne`, `deleteOne`/`Many`, `findOneAnd*`, `bulkWrite`),
      DDL (`createCollection`, `drop`, `createIndexes`, `dropIndexes`), stats, and `explain` already flow
      through `runCommand` in `shell-runtime.js`.
    - `find(db, collection, filter, options)` → builds `{ find: collection, filter, projection?, sort?,
      skip? (>0), limit? (>0, or <0 → abs(limit) + singleBatch: true), batchSize? (>0), maxTimeMS?,
      collation?, hint?, comment?, allowDiskUse? }` and opens a transport `CursorState`.
    - `aggregate(db, collection, pipeline, options)` → builds `{ aggregate: collection, pipeline,
      cursor: { batchSize? }, allowDiskUse?, maxTimeMS?, collation?, hint?, comment?,
      bypassDocumentValidation?, let? }` and opens a transport `CursorState`.
    - `CursorState` transport pagination & cleanup: holds `batch` (`std::vector<bsoncxx::document::value>`),
      `index`, `remoteCursorId`, `db`, `collection`, and `batchSize`. When `index >= batch.size()` and
      `remoteCursorId != 0`, issues `{ getMore: NumberLong(remoteCursorId), collection, batchSize? }`. On
      `cursorClose` / `closeAllCursors`, sends best-effort
      `{ killCursors: collection, cursors: [NumberLong(remoteCursorId)] }` when `remoteCursorId != 0`.
    - `countDocuments(db, collection, filter, options)` → runs `aggregate` with
      `[{ $match: filter }, { $skip }?, { $limit }?, { $group: { _id: 1, n: { $sum: 1 } } }]` and returns `n`
      (`0` when empty).
    - `estimatedDocumentCount(db, collection)` → runs `{ count: collection }` and returns `n`.
    - `distinct(db, collection, field, filter, options)` → runs
      `{ distinct: collection, key: field, query: filter, maxTimeMS?, collation?, comment? }` and returns
      `reply["values"]`.
    - `listCollections(db, filter)` / `listIndexes(db, collection)` → runs `{ listCollections: 1, filter }` /
      `{ listIndexes: collection }` (draining `getMore` if `cursor.id != 0`) and returns JS arrays.
    - `serverVersion()` → runs `{ buildInfo: 1 }` on `"admin"` and returns `"version"`.
    - `serverAddress()` → returns `_seedHosts`.

### 2.3 Lifting UI Shell-Tab Refusal & Guarding `SchemaAnalysis`

- Lift the `hasTransport()` early-return in `App::openShell`, `QmlBackend::openShell`, and
  `QmlBackend::openShellWithScript`. Keep `QmlBackend::openMemberShell` (replica-set direct member shell)
  and `ServerMonitor` (`QmlBackendUtils.cpp`) guarded on `hasTransport()`.
- **Channel hygiene in `SchemaAnalysis` / `QmlBackend::schemaOptions`:**
  - Previously, `SchemaAnalysis::{analyzeCollection,estimatedDocumentCount,listCollections}` unconditionally
    constructed a raw `mongocxx::client`, which bypassed `hasTransport()` in the background whenever a
    database or collection was used!
  - Fix:
    1. In `QmlBackend::schemaOptions`: return `false` when `hasTransport()` is `true` and
       `transportKind() != "memory"`, so gateway connections do not open background schema jobs or consume
       `kMaxSessions` / `DeviceSessionLedger` slots.
    2. In `SchemaAnalysis`: add transport-backed overloads accepting `IMongoTransport &` (testable on every
       platform); when `connection.hasTransport()` is `true`, use
       `InMemoryDemoTransport(InMemoryDemoTransport::sharedStore())` `#if defined(Q_OS_WASM)` for `"memory"`
       (giving instant field/type autocomplete & Schema Analysis in the WASM Playground), and otherwise fail
       closed without constructing `mongocxx::client`.

### 2.4 BSON-Native `InMemoryDemoTransport` & Shared `InMemoryDemoStore`

- Replace `QJsonObject` storage with `bsoncxx::document::value` inside a mutex-protected `InMemoryDemoStore`:
  - Exact BSON type fidelity (`ObjectId`, `Int32`, `Int64`, `Double`, `Decimal128`, `Date`, `Bool`, `Null`,
    `String`, `Document`, `Array`, `RegExp`, `Binary`) and preserved key order.
  - Default `InMemoryDemoTransport()` creates an isolated `std::make_shared<InMemoryDemoStore>()` so unit
    tests never pollute one another.
  - `InMemoryDemoTransport::sharedStore()` + `InMemoryDemoTransport::resetSharedStore()` provide a single
    thread-safe store shared across Primary Explorer, Shell Tabs, and `SchemaAnalysis` in WASM Playground,
    ready for `W5`'s one-click demo reset.
- **Supported Commands:**
  - Admin/server: `ping`, `hello`, `isMaster`/`ismaster`, `buildInfo`/`buildinfo`, `serverStatus`,
    `hostInfo`, `listDatabases`, `connectionStatus`, `whatsmyuri`, `getCmdLineOpts`, `getLog`, `currentOp`,
    `killOp`.
  - DDL/stats/indexes: `listCollections`, `create`, `drop`, `dropDatabase`, `renameCollection`, `collStats`,
    `dbStats`, `listIndexes`, `createIndexes`, `dropIndexes`, `explain` (valid `queryPlanner` +
    `executionStats` document compatible with `ExplainPlan`).
  - Reads: `find` (filter, inclusion/exclusion `projection`, multi-key BSON-ordered `sort`, `skip`, `limit`),
    `count`, `distinct`.
  - Filters: implicit equality, dotted nested paths & array element traversal, cross-numeric BSON equality &
    ordering (`int32`/`int64`/`double`/`decimal128`), regex values & `$regex`/`$options`, `$eq`, `$ne`,
    `$gt`, `$gte`, `$lt`, `$lte`, `$in`, `$nin`, `$exists`, `$type`, `$not`, `$all`, `$size`, `$elemMatch`,
    `$and`, `$or`, `$nor`.
  - Writes (wire-compatible `writeErrors` envelope for `shell-runtime.js` & `MongoClient`):
    - `insert` (`documents`, `ordered`, auto `_id` `ObjectId` when omitted, unique `_id` + unique secondary
      index enforcement with `code: 11000` `DuplicateKey` in `writeErrors`),
    - `update` (`updates` with `q`, `u`, `multi`, `upsert`; replacement documents + `$set`, `$unset`, `$inc`,
      `$mul`, `$min`, `$max`, `$rename`, `$currentDate`, `$setOnInsert`, `$push`/`$each`,
      `$addToSet`/`$each`, `$pull`, `$pop`; immutable `_id` error `code: 66`),
    - `delete` (`deletes` with `q`, `limit: 0 | 1`),
    - `findAndModify` (`query`, `sort`, `remove`, `update`, `new`, `fields`, `upsert`).
  - Aggregation (`aggregate`): `$match`, `$sort`, `$project`, `$unset`, `$addFields`/`$set`, `$skip`,
    `$limit`, `$count`, `$unwind`, `$sample`, `$sortByCount`, and `$group` (`_id` literal/`null`/field/object
    with `$sum`, `$avg`, `$min`, `$max`, `$first`, `$last`, `$push`, `$addToSet`, `$count`).

---

## 3. Threats & Edge Cases

| ID | Threat / Edge Case | Mitigation |
|---|---|---|
| **T1** | **Channel bypass on `hasTransport()` connections** — Secondary `MongoWorker` (`App::openShell`), `CollectionTransfer` (Export/Import), `AggregationPipeline::runPreview`, `SchemaAnalysis`, `McpGuards`/`McpBackend`, SSH/PEM dialogs, or replica-set helpers falling through to `mongocxx::client` and opening raw TCP outside the configured gateway/demo channel. | `MongoWorker::{getConnection,getClient,ensureScriptEngine}`, `SchemaAnalysis`, `CollectionTransfer::{exportCollectionToFile,importFileToCollection}`, `AggregationPipeline::runPreview`, `McpGuards::unsupportedReason`, `McpBackend`, `MongoServer`, `App`, and `QmlBackend` check `hasTransport()` first and **fail closed** on error — never constructing `mongocxx::client`. |
| **T2** | **Desktop `"memory"` spoofing (`8.6` `M3` regression)** — A synced or imported `ConnectionSettings` with `transportKind == "memory"` serving fake demo data under a production connection name on desktop. | `MongoWorker::ensureTransportConnected`, `QmlBackend::schemaOptions`, `SchemaAnalysis`, and `AggregationPipeline::runPreview` allow `kind == "memory"` **strictly under `#if defined(Q_OS_WASM)`**. Desktop unit tests inject `InMemoryDemoTransport` explicitly via C++ API (`ShellConnectOptions::transport` / `SchemaAnalysis` overload). |
| **T3** | **Cross-thread `QWebSocket` access & nested `QEventLoop` re-entrancy** — `JsShellBackend::connect` touching `WebSocketGatewayTransport` off-thread, or `keepAlive()` / `AutocompleteRequest` / `updateLocalServerPort` re-entering `WebSocketGatewayTransport::call()` during an inner `QEventLoop::exec()`. | `JsShellBackend::connect` runs synchronously on `MongoWorker::_thread`; `MongoWorker` tracks `_inFlightOps` and `_scriptRunning` to skip `keepAlive`/`AutocompleteRequest` and defer `updateLocalServerPort`; `WebSocketGatewayTransport` ignores mismatched correlation IDs; `ShellController` suppresses autocomplete while `_busy`. |
| **T4** | **Browser heap / stack / CPU exhaustion & WASM float→int traps** — `while(true){}`, deep recursion, `limit(Infinity)` / `skip(NaN)` (`i64.trunc_f64_s` trap), `INT64_MAX` `$inc` signed overflow, ReDoS `$regex`, or oversized pipeline documents. | `doubleToInt64` clamps non-finite/out-of-range doubles before casting; `checkedAddInt64`/`checkedMulInt64` promote to `double` without UB; recursion & write-path depth capped at `64`; pipeline stages capped at `64`; `256 KiB` doc size enforced across `insert`/`update`/`findAndModify`/`aggregate`; nested-quantifier `$regex` patterns rejected; `8` DBs, `32` colls/DB, `5,000` docs/coll, `10,000` pipeline docs. |
| **T5** | **BSON precision loss & wire-shape mismatch** — `NumberLong("9007199254740993")` or `NumberDecimal("124.50")` losing precision via `QJsonDocument` (`double`), `{ find, limit: 0 }` failing on MongoDB 5.0+, or duplicate `_id` not surfacing `.code == 11000` in JS `try/catch`. | Canonical EJSON (`k_canonical`) end-to-end between `ShellHost` and `InMemoryDemoStore` (`bsoncxx::document::value`); `{ find }` omits `limit` when `0` and translates `< 0` to `abs(limit) + singleBatch: true`; write errors use standard `{ ok: 1, writeErrors: [{ index, code: 11000, codeName: "DuplicateKey", errmsg }] }` plus `MongoClient::insertDocument`/`saveDocument` checking `writeErrors`. |
| **T6** | **Credential leakage in secondary-worker error messages (`8.6` `L7`)** — Gateway connect error echoing `mongodb(+srv)://user:pass@host`. | `MongoWorker::ensureTransportConnected` applies the same `QRegularExpression` credential redaction before logging or surfacing errors to `ExecuteScriptResponse` / `EstablishConnectionResponse`. |

---

## 4. Verification Plan

1. **`InMemoryDemoTransport_test.cpp`:** Seed compatibility, CRUD (`insert`, `update` operators, `delete`,
   `findAndModify`), duplicate key `11000` (primary `_id` + unique secondary index), immutable `_id` `66`,
   rich filters & cross-numeric comparisons, `sort`/`projection`/`skip`/`limit`, `distinct`, `aggregate`
   (`$match`, `$group`, `$project`, `$unwind`, `$sample`, `$count`), `listIndexes`/`createIndexes`/
   `dropIndexes`, `collStats`/`dbStats`/`explain`, BSON `Int64` & `Decimal128` round-trip, shared-store
   visibility across instances + `reset()`, and collection/document/size quota enforcement.
2. **`ShellHost_test.cpp` (transport suite):** Full `ScriptEngine` / `JsShellBackend` / `ShellHost` execution
   over `InMemoryDemoTransport` (no live `mongod` required — always runs in CI!), covering `find().sort().limit()`,
   `insertOne`/`updateOne`/`deleteOne`/`findOneAndUpdate`/`bulkWrite`, `aggregate` `$group`, `distinct`,
   `countDocuments`, `getIndexes`/`createIndex`, `explain()`, `NumberLong` & `NumberDecimal` round-trip,
   `MongoServerError` `.code == 11000`, `complete("db.")`, `SchemaAnalysis` transport overload, and live
   loopback `WebSocketGatewayTransport` shell execution (`getMore` cursor batching included).
3. **Mandatory Repository Gates:** `windows` preset build (`rago_unit_tests`, `ragoshell`), `rago_unit_tests`
   100% green, `ragoshell --check-qml` exit `0`, independent security review on the diff.
