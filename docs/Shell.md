# The embedded shell

The shell tab runs JavaScript inside the application, on Qt's own engine (`QJSEngine`), and talks to
MongoDB through the C++ driver (`mongocxx`). It is the same code on Windows, Linux, macOS, Android and
iPadOS. It replaced a child Node.js process; Node.js is gone from the application and its packages, and
there is no fallback to it.

The target is the API of the legacy `mongo` shell, not full `mongosh`; all calls are synchronous. MongoDB 4.0
is the oldest server it is tested against (4.0.28).

## Architecture

```
ShellTabPage.qml
  ShellController            Execute, Stop, autocomplete, paging
    MongoShell               builds the requests, numbers the operations (opGen)
      event bus -> MongoWorker      one QThread per worker, owns one ScriptEngine
        ScriptEngine                synchronous API, connect options, failedScope
          ShellBackend (interface) -> JsShellBackend      QJSEngine, time limit, Stop
            shell-runtime.js                              the shell API, in JavaScript
              __host = ShellHost  -> mongocxx -> server   driver calls, cursors, killOp
              __bson = BsonJsConverter                    BSON <-> JavaScript, both directions
```

- `src/ragoshell/core/engine/ScriptEngine.cpp` keeps the API `MongoWorker` uses (`init`, `exec`,
  `interrupt`, `complete`, `ping`, `use`, `changeTimeout`). It builds `ShellConnectOptions` from
  `ConnectionSettings` (URI from `buildMongoUri`, TLS files, CRL, `directConnection` unless replica set or
  `mongodb+srv`, timeouts, the startup script), maps `ShellItem`s to `MongoShellResult`s and decides what a
  failure means. An SSH tunnel is set up before the worker starts; the settings then point at its local port.
- `src/ragoshell/core/engine/JsShellBackend.cpp` creates the `QJSEngine` on the thread that connects (the
  worker thread), evaluates the runtime once and calls its `evalScript`, `complete`, `use`, `configure`.
- `src/ragoshell/core/engine/js/shell-runtime.js` is compiled into `rago-core` as a byte array
  (`src/ragoshell/CMakeLists.txt`; editing it re-runs CMake). It holds the cursor classes, `collectionProxy`,
  `makeDb`, the `show` / `use` / `rs` / `sh` helpers, statement splitting, result building, the Extended JSON
  text and the autocomplete lists.
- `src/ragoshell/core/engine/ShellHost.cpp` is the hidden global `__host`. Reads that must follow the URI's
  read preference are driver calls (`find`, `aggregate`, `countDocuments`, `estimatedDocumentCount`,
  `distinct`, `listCollections`, `listIndexes`). Everything else, writes included, is `runCommand`: the
  runtime builds the `insert` / `update` / `delete` / `findAndModify` / `createIndexes` commands and shapes
  the replies (`{acknowledged, insertedId}`, `{matchedCount, modifiedCount, upsertedId}`,
  `{deletedCount}`, bulk counts). A failure becomes a JavaScript `Error` with the server's `code`/`codeName`.
- **Cursors** stay in C++: `ShellHost` holds the `mongocxx::cursor`, JavaScript an integer id. When a
  statement's value is a cursor, JavaScript does not read it; the result item carries the id and
  `JsShellBackend::toItem` takes the first page from `bsoncxx` straight into `MongoDocument`, with no
  JavaScript object and no JSON text. Every eval starts by closing all cursors.
- **Paging** of a `find` does not use the shell: `MongoShell::query` sends an `ExecuteQueryRequest` that
  `MongoClient` runs with the skip and limit of the recorded `MongoQueryInfo`. An aggregate is paged by
  re-running a script that `aggregatePageScript` (`core/utils/AggregatePaging.cpp`, used by `ShellController::loadPage`)
  builds: the recorded pipeline plus `$skip` and `$limit`, written as canonical Extended JSON and read back by the
  runtime's `EJSON.parse`, so ObjectIds, dates and int64 keep their types (as plain `{"$oid": ...}` objects they matched
  nothing: the next page of such a pipeline came back empty on 4.0).
- **MCP.** `ragoshell-mcp` does not use the engine; it parses shell-style filters with `MongoJsonUtils`.

Every `MongoWorker` has a `QThread` of its own and owns one `ScriptEngine`: one `QJSEngine`, one client and
one set of cursors; shell tabs use workers of their own. The engine is used only from its worker thread.
Two things reach it from other threads: `interrupt()` (`MongoShell::stop` calls `MongoWorker::interrupt`
directly, because the busy worker could not read an event) and the watchdog of the running script.

## Decisions and their reasons

- **QJSEngine.** QML already runs on it, so it adds no dependency; it exists on every target, including
  iOS, where JIT is forbidden and it falls back to its interpreter; it can be stopped from another thread
  (`setInterrupted`); `QObject` and `QJSValue` bind to C++ without glue. The price is its language level
  (below). QuickJS was the fallback plan and was not needed.
- **Synchronous host calls on the worker thread.** The worker already blocks on driver calls, and the UI
  thread must not. No promises, no event loop in the engine, so `for (...) { var d = db.c.findOne(...); }`
  works as in the legacy shell.
- **64-bit integers are never JavaScript numbers.** `NumberLong` is a `Long` object holding the decimal
  text; `valueOf()` gives a number, so `doc.n + 1` and `doc.n > 5` work, while the stored value keeps every
  digit (the result of arithmetic is an ordinary number). `Timestamp` and `Decimal128` are wrapper objects too.
- **A fresh scope per run, not a fresh engine.** The runtime is evaluated once per engine. Each `exec` builds
  a new `sandbox` object (`db`, `print`, constructors, `rs`, `sh`, ...) and runs the statements as
  `with (sandbox) { eval(statement) }` inside one generated function: statements of a script share variables,
  the next script sees none. Every statement is its own `eval`, so a leading `let` / `const` / `class` is
  rewritten to `var` to reach the next statement.
- **The client name tags operations.** The URI carries `appName=ragoshell-shell-<random>`, so `currentOp`
  can tell the shell's operations from all others without changing any command.

## The language level

`QJSEngine` is an engine at about the ES2017 level, plus `?.` and `??`. Measured with `qml` of Qt 6.12.0 (the
same engine) on 2026-10-29:

| Works | Missing or broken |
|---|---|
| arrow functions, `let` / `const`, template literals, `for...of`, generators, `Symbol.iterator` | `async` / `await`, async generators, `BigInt` |
| array destructuring, object destructuring without rest, array spread, rest and default parameters | object spread `{...o}`, object rest |
| classes with getters, static methods, `extends` / `super` | class fields, private `#members` |
| `?.`, `??`, `**`, `Map`, `Set`, `Proxy`, `Reflect`, `JSON`, `Date`, `Object.entries` / `values` / `assign`, `Array.from` / `includes` | `??=`, `catch {}` without binding, `globalThis`, `structuredClone`, `setTimeout`, `Intl`, `Object.fromEntries`, `Array.prototype.flat` / `at`, `String.prototype.replaceAll` / `trimStart` |
| RegExp flags `i m s u y` | named groups, lookbehind (compiles, never matches), flag `d` |

The runtime uses only the left column. A user script that uses the right column fails with QJSEngine's own
`SyntaxError` / `TypeError` / `ReferenceError` text, followed by a line `Hint: <feature> is newer than the JavaScript of this
shell ...` (`unsupportedFeature` in `shell-runtime.js`; it names async / await, BigInt, `??=`, `catch {}`, object spread and
rest, class fields, `replaceAll`, `flat`, `at`, `Object.fromEntries`, `globalThis` and the timers); a script that does not compile is not split into statements (the splitter
checks pieces with `new Function(src)`), so the whole script reports the error.

## The shell API

- **Scope of a script:** `db`, `use <name>` / `use(name)`, `show dbs | databases | collections | tables |
  users | roles | profile | logs | log [name]`, `rs`, `sh`, `print`, `printjson`, `printjsononeline`,
  `tojson`, `console.log | info | warn | error` (all print), `EJSON` (`stringify`, `parse`, `serialize`,
  `deserialize`), `Date`, and the constructors `ObjectId` / `ObjectID`, `ISODate`, `NumberLong`, `NumberInt`,
  `NumberDecimal`, `Timestamp`, `UUID`, `BinData`, `HexData`, `MinKey`, `MaxKey`, `DBRef`.
- **Collections:** `db.name`, `db['name']`, `db.getCollection('name')`, dotted names (`db.fs.files`,
  `db.system.js`). Reads: `find`, `findOne`, `aggregate`, `count`, `countDocuments`,
  `estimatedDocumentCount`, `distinct`. Writes: `insert*`, `save`, `replaceOne`, `update*`, `findAndModify`,
  `findOneAnd*`, `remove`, `delete*`, `bulkWrite`. Indexes: `getIndexes`, `getIndexKeys`, `createIndex`,
  `ensureIndex`, `createIndexes`, `dropIndex`, `dropIndexes`, `hideIndex`, `unhideIndex`, `reIndex`. Other:
  `drop`, `renameCollection`, `validate`, `compact`, `convertToCapped`, `stats` and its size helpers,
  `latencyStats`, `explain([verbosity])` followed by `find` / `findOne` / `aggregate` / `count` /
  `distinct` / `remove`, `getName`, `getFullName`, `getDB`.
- **Cursors:** `find` returns a `QueryCursor`, `aggregate` an `AggCursor`; nothing is sent until the cursor
  is read or is the statement's value. `QueryCursor`: `sort`, `limit`, `skip`, `batchSize`, `hint`,
  `maxTimeMS`, `collation`, `count(applySkipLimit)`, `size`, `itcount`, `toArray`, `forEach`, `map`,
  `hasNext`, `next`, `close`, `explain`, `for...of`. `AggCursor`: `toArray`, `forEach`, `map`, `hasNext`,
  `next`, `close`, `explain`. `find(query, projection)` takes a projection, not an options object.
- **Database:** `getSiblingDB`, `getCollection`, `runCommand`, `adminCommand`, `getCollectionNames` /
  `Infos`, `createCollection`, `createView`, `dropDatabase`, `stats`, `printCollectionStats`,
  `serverStatus`, `hostInfo`, `version`, `serverBuildInfo`, `currentOp`, `killOp`, `isMaster`, `hello`, the
  user methods (`getUsers`, `createUser`, `dropUser`, `grantRolesToUser`, ...), `getRoles`, profiling,
  `getMongo()` (only `host`, `getDBNames`, `getDBs`). `rs`: `status`, `conf`, `config`, `isMaster`, `hello`.
  `sh`: `status`, `getBalancerState`.
- **Not supported** (defined nowhere in the runtime): sessions and transactions, change streams, `Mongo()`,
  `connect()`, `db.auth`, `sleep`, `quit`, `load`, `require`, `help`, `it`, `createRole` and other role
  management, the cursor methods `min`, `max`, `comment`, `allowDiskUse`, `tailable`, and every
  `mongosh`-only helper. An unknown method is a `TypeError`.

Quirks:

- `db.<name>` is a collection for any name that is not a `db` method and does not start with `_`. Use
  `db.getCollection('_x')` for such names and for names like `stats` or `version`. A line that starts with
  `show`, `use` or `set` plus a word is always a shell command, even inside a multi-line literal.
- A whole-number JavaScript number is stored as `int32` if it fits, a fractional one as `double`; there is
  no user constructor for `double`. Reading a `double` `2.0` and writing it back stores `int32` `2`.
  `NumberInt(x)` is `x | 0`, a plain number.
- Impossible constructor arguments throw (`NumberLong('abc')`, `ObjectId('xyz')`). `print` and `printjson`
  return nothing, so a `print` statement gives one output and no extra result.
- Assigning to an undeclared name (`x = 5`) creates a property of the engine's real global object and
  outlives the script. The sandbox is not a security boundary: a user script can reach `__host`.

## Statements, results and BSON

`evalScript` replaces `show` / `use` / `set` lines by `shellHelper(...)` calls, splits the script at top-level
`;` and at top-level newlines where both sides compile alone, and evaluates the statements in turn. The
script stops at the first error. Each statement's value becomes a result item:

| Value | `type` |
|---|---|
| `QueryCursor` | `query`, with `MongoQueryInfo` (db, collection, filter, projection, limit, skip, batch size) |
| `AggCursor` | `aggregate`, with `AggrInfo` (collection, pipeline, options) |
| array of documents, one document | `custom` (grid); `stats()` and `printCollectionStats()` give `collectionStats` |
| anything else, or only `print` output | `text` |
| an exception | `error`, text `Name: message` |

The text of a value is relaxed Extended JSON on one line (`{"d":{"$date":"2020-01-02T03:04:05Z"}}`; no `.000`
on a whole second; an int64 is a number when exact as one, else `{"$numberLong":...}`). A string value is
quoted, `print('x')` is not. The text is written in JavaScript (`ejsonTree`), not by the `bsoncxx` writer.
The elapsed time of a script is split evenly over its statements.

| BSON | JavaScript |
|---|---|
| double, int32, string, bool, null, document, array | the same types; field order is kept |
| int64 | `Long` (`.value` is the decimal text) |
| date | `Date` (milliseconds, also before 1970) |
| regex | `RegExp` for the flags `i m s u`; `BsonRegExp` when JavaScript cannot hold it (`x`, `l`, PCRE-only syntax) |
| ObjectId, Timestamp, Decimal128, Binary (all subtypes; UUID is 4), MinKey, MaxKey, Code, Symbol, DBPointer | classes of `__bson` with a `_bsontype` tag |
| DBRef | an ordinary document (`$ref`, `$id`, `$db`) |
| `undefined` | JavaScript `undefined` is written as null; a BSON undefined reads as `undefined` |

JavaScript to BSON refuses, with a message that names the problem: a `Long` outside 64 bits, bad decimal text,
an invalid `Date`, a RegExp flag other than `i m s u` (`g` is dropped, it means nothing to a query), a binary
subtype outside 0-255, a field name with a null character, nesting deeper than 100 levels (this also catches
an object that contains itself). A function inside an object is left out, inside an array it becomes null.

## Stop, time limit, errors

**Stop.** `JsShellBackend::interrupt()` (any thread) calls `QJSEngine::setInterrupted(true)`, which ends
JavaScript that loops, even inside `try / catch`, within about 10 ms. While a script runs it also starts one
killer per script: a thread with a second connection (`appName` `ragoshell-killer-...`) that finds the
shell's operations in `currentOp` (`$ownOps` for a user who may not see others) and ends them with `killOp`,
up to ten looks 150 ms apart, because an operation may not have reached the server yet. That ends a call the
driver is blocked in. On 4.0.28 a Stop during a 30-second `$where` sleep ends the script about 200 ms after it
was pressed and leaves nothing in `currentOp`.

**Time limit.** The shell timeout (Preferences, default 15 s) is a watchdog thread per `eval`: when the whole
script, startup script included, runs longer, it does what Stop does and the result is "Timeout reached"
(`timeoutReached()`). Behind it, the host's reads get a server-side `maxTimeMS` of the limit plus 500 ms (so
the watchdog's message wins) and the connection a `socketTimeoutMS` of twice the limit, at least 30 s. Writes
and `runCommand` have no server-side limit. Connecting: `serverSelectionTimeoutMS` is max(15 s, limit) minus
2 s, `connectTimeoutMS` at most 10 s, `serverSelectionTryOnce=false` (otherwise an unreachable host waits out
the TCP timeout whatever the selection time says).

**After a Stop or a timeout** `ScriptEngine` drops the connection (the client and all cursors), sets
`failedScope`, and the next script reconnects; the `QJSEngine` is kept. `MongoWorker` retries a script once
when `failedScope` is set and it was neither cancelled nor timed out. A 60 s `ping()` marks the scope failed
when the server stops answering. A connect can be stopped too: `connect()` opens the client on a thread of
its own and polls it every 25 ms; a thread that is given up closes its client when it finishes.

**Errors.** A script error is only an `error` result and leaves the shell ready. Names: `SyntaxError`,
`TypeError`, `ReferenceError`, `Error`; `MongoServerError` (the server's `errmsg`, with `code` and `codeName`
on the error object); `MongoBulkWriteError` (with `result`, `writeErrors`), `MongoWriteConcernError`,
`MongoInvalidArgumentError`. A server error met while the first page of a cursor is read is an `error` item
too. A failure of the backend itself (not connected, the runtime did not load) is an error of `exec`.

## The startup script

The Preferences "startup script" reaches `configure` at every connect and is syntax-checked with
`new Function`; if it does not compile it is not used and a warning is logged (the connection works). A valid script runs before every `exec`, statement by statement, in
the user's scope, so what it declares is visible (`const` and `class` become `var` as above); an exception
in one of its statements is swallowed. Settings from versions with the "load `~/.mongorc.js`" switch get that
file's text copied into the startup script once (`SettingsManager.cpp`).

## Autocomplete

Editor IntelliSense runs synchronously in C++ via `ShellCompletion` (`core/utils/ShellCompletion.h/.cpp`),
called from `ShellController::requestCompletion` on the text before the caret (up to 8,000 UTF-16 code units).
It models two orthogonal layers:

1. **Receiver return-type chains:** `db` (`Database`), `db.getSiblingDB(...)` (`SiblingDatabase`), `db.system`
   (`SystemNamespace`), `db.getMongo()` (`Mongo`), `db.<coll>` / `db.getCollection(...)` / `db[...]`
   (`Collection`), `.find(...)` + chainable modifiers (`sort`, `limit`, `skip`, `projection`, `hint`,
   `collation`, `batchSize`, `maxTimeMS`, `pretty`, `readPref`) -> `QueryCursor`, `.aggregate(...)` ->
   `AggCursor`, `.explain(...)` -> `Explainable`, `rs`, `sh`, `EJSON`, `console`, and local variable bindings
   (`const cur = db.orders.find({}); cur.`).
2. **MQL document, stage, operator, and string-argument scopes:** `PipelineStageKey`, `UpdateOperatorKey`,
   `FieldConditionKey`, `FilterKey`, `FieldsKey`, `GroupOutputKey`, `GroupFieldValue`, `AggExpressionKey`,
   `SpecKey` (`$lookup`, `findAndModify`, `bulkWrite`), `QuotedFieldKey`, `FieldReference`, `DistinctFieldArg`,
   `CollectionNameArg`, `DatabaseNameArg`, `ExplainVerbosityArg`, `ShowCommand`, `UseCommand`,
   `StatementStart`, and `Expression`, enriched with schema fields from `SchemaCatalog`
   (`docs/SchemaIntelligence.md`).

When a context benefits from live collection names not yet cached for the tab's active database,
`ShellController` requests `db.` once in the background through `MongoShell::autocomplete` ->
`ScriptEngine::complete` -> `buildCompletions` in `shell-runtime.js` (invalidated after script execution).
Both `ShellCompletion` and `ScriptEngine::complete` honor `AutocompleteAll`, `AutocompleteNoCollectionNames`,
and `AutocompleteNone`, format non-identifier or colliding collection names as `db.getCollection('...')`, and
never offer `db.<collection>` outside `db.` / `CollectionNameArg` positions. Call tips live in
`core/utils/ShellSignatures.cpp`.

## Testing

The engine tests are in `rago_unit_tests` (gtest). Tests that need a server use `localhost:27017`
(`$RAGOSHELL_TEST_MONGO` overrides `host[:port]` for `ShellCorpusTest`), MongoDB 4.0+
with a `samples` database (`restaurants`, `zips`, `books`, …), and print `[ SKIP ]` and pass when none answers.
Deterministic seed and ephemeral Docker verification live in `dev/shell-corpus/` (`seed-samples.js`,
`record-corpus.sh`; see `docs/ShellCorpusRecorder.md`); the author's local container is `dev/mongo-test/`
(git-ignored; `README.md`, "Test data"). The tests create and drop
the scratch databases `rago_parity`, `rago_corpus_c`, `rago_js_only`, `rago_js_host_test`, `rago_selftest`.

```
# Windows (Qt's bin directory on PATH); on Linux and macOS: ./build/<preset>/src/ragoshell-unit-tests/<config>/rago_unit_tests
build\windows\src\ragoshell-unit-tests\Debug\rago_unit_tests.exe --gtest_filter="JsEngineCapabilities*:BsonJsConverter*:ShellHost*:ShellCorpus*:ShellJs*:ScriptEngine*:ShellBackend*"
```

| In `src/ragoshell/core/engine/` | Covers | Server |
|---|---|---|
| `JsEngineCapabilities_test.cpp` | what the runtime needs from `QJSEngine`: language features, `Proxy` for `db.x.y`, a fresh scope per run, Stop of a loop from another thread, the engine on a worker thread, synchronous `QObject` calls, error name and line, a CPU smoke test | no |
| `ShellHost_test.cpp` | `BsonJsConverter` (every BSON type byte for byte, int64 digits, number typing, refusals, depth) and `ShellHost` (cursors, server errors with code, aggregate / count / distinct, int64 / decimal / date through a write) | live half |
| `ScriptEngine_test.cpp` | `ScriptEngineLiveTest`, `ScriptEngineCancelTest` (Stop before an exec and during a connect to an unroutable address), `ShellBackendTest`, URI building | live half |
| `ShellCorpus_test.cpp` | `ShellCorpusCompareTest` (offline `_id` normalization, strictness, and endpoint parsing) and `ShellCorpusTest` (below, plus corpus seeding/recording) | live half |
| `JsShellBackendLive_test.cpp` | `ShellJsTest`: server results inside a statement, `print`, helpers, int64, scopes, startup script, paging, Unicode, Stop of a loop and of a server call, the time limit, a script that catches every exception, error kinds | yes |

`ragoshell --shell-selftest=host:port` (`src/ragoshell-app/ShellSelfTest.cpp`) is the same idea without
gtest, for devices (`adb logcat`, `xcrun devicectl ... --console`; `docs/Debug.md`, `docs/Platforms.md`;
verified on the iPad 8th generation and Lenovo TB-X606X Android tablet).
It uses the database `rago_selftest`, prints `SHELLSELFTEST PASS|FAIL` lines and returns the number of failed
checks. After the connect time it runs 19: writes, find and aggregate results, int64 digits, Unicode, a
server result in a loop, scopes, print, two error kinds, `show collections`, autocomplete, and a runaway
loop that must end at a 1 s limit.

### The golden corpus

`src/ragoshell-unit-tests/shell-corpus/` holds recorded results: `reads.json` (28 scripts),
`scripting.json` (31), `errors.json` (12), `writes.json` (36), `completions.json` (64 prefixes). They were
recorded from the Node shell this one replaced, on the seeded `samples` database. A file is
`{ "database", "group", "entries": [ { "script", "run": { "error", "errorMessage", "items": [ { "type",
"output", "documents" (relaxed Extended JSON), "db", "collection", "limit", "skip", "batchSize", "query",
"fields", "pipeline", "options", "aggrCollection" } ] } } ] }`; in `completions.json` an entry is `{ "prefix", "items" }`.

`ShellCorpusTest` runs the scripts of a group in order on one `ScriptEngine` (the `writes` group in the
scratch database `rago_parity`, after a `dropDatabase`) and compares each result: error flag, number of
items, `type`; for an `error` item only the name before the first colon (engine message wording is the
engine's own); otherwise the exact output text, documents, query info (`limit`, `skip` and `batchSize` for
`query` items) and aggregate info. Dynamic `ObjectId` `_id` values (`{"$oid": "<24-hex>"}`) are canonicalized
ordinally within each item so freshly seeded `samples` databases match isomorphically while scalar/composite
`_id`s, non-`_id` `ObjectId`s, field order, values and array lengths remain strict; legacy MongoDB <= 4.2
`listIndexes` `"ns"` fields are also normalized across server versions (`docs/ShellCorpusRecorder.md`).

To verify against a disposable container or re-record after adding a case to `ShellCorpus_test.cpp`:

```
./dev/shell-corpus/record-corpus.sh --ephemeral-docker --verify-only
./dev/shell-corpus/record-corpus.sh --ephemeral-docker --out /tmp/rago-corpus-preview
```

Always check diffs of new or changed entries against legacy `mongo` / `mongosh` expectations before updating
`src/ragoshell-unit-tests/shell-corpus/*.json`. Changing a word list of the runtime updates `completions.json` too.

## Measurements

Windows, Debug build (Qt's libraries are unoptimized), MongoDB 4.0.28 on the same PC; one `ScriptEngine` is
one shell tab. Re-measured on 2026-10-29:

```
RAGO_MEASURE_TABS=3 rago_unit_tests --gtest_also_run_disabled_tests --gtest_filter=ShellCorpusTest.DISABLED_measure_first_result_and_memory
```

- Connect of a tab 60-77 ms; first script (a `find` of 20 documents) 5-7 ms, the second 4 ms.
- Three tabs grew the process by about 24 MB, 8-9 MB per tab (Windows only, `PrivateUsage`). The runtime
  script is 57 KB inside the executable; Qt's engine was already part of the app (QML).
- 3,000,000 loop iterations and 100,000 objects take about 2 s here (printed by `JsEngineCapabilities_test.cpp`).
  A Release build and the iOS interpreter were not measured.

## Open items

- **`docs/ManualTests.md` section 32 has not been run by a person** (17 open items: TLS, SSH tunnels,
  replica sets, authentication, `mongodb+srv`, five tabs, the tablets). There is no fallback, so a
  regression found there is fixed forward.
