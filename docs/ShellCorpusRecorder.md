# Shell Corpus Normalization and Recorder (`M18`)

Design and threat analysis for decoupling `ShellCorpusTest` from a single machine's imported `ObjectId` values and providing a self-contained, MongoDB 4.0+-compatible corpus seeder and recorder under `dev/shell-corpus/`.

## 1. Problem statement

`src/ragoshell-unit-tests/shell-corpus/` stores golden expectations for the embedded JavaScript shell (`reads.json` 28 scripts, `scripting.json` 31, `errors.json` 12, `writes.json` 36, and `completions.json` 64 prefixes):

1. **Dynamic `_id` coupling (`TodoMac.md` M18a).** Documents in `samples.restaurants` have no explicit `_id` in MongoDB's sample dataset; `mongoimport` assigns fresh `ObjectId` values at import time. Both `item.documents` and `item.output` (for `type == "custom"`, such as `.toArray()`) recorded those one-off `{"$oid": "..."}` strings (`6ada7d441bcace55d9a1e10a`, …), causing `ShellCorpusTest` to fail against any freshly seeded server.
2. **Server-version `listIndexes` drift.** `reads.json` entry 17 (`db.restaurants.getIndexes()`) was recorded on MongoDB 4.0, where `listIndexes` includes `"ns": "samples.restaurants"`. MongoDB 4.4+ (`SERVER-41696`) omits `"ns"` from `listIndexes` descriptors while keeping `v`, `key`, and `name` identical.
3. **Hard-coded endpoint and missing recorder (`TodoMac.md` M18b).** `ShellCorpus_test.cpp` hard-coded `localhost:27017` and compile-time `RAGO_SHELL_CORPUS_DIR`. No tracked seed or recorder workflow existed in the repository (`dev/mongo-test/` is git-ignored author state and must never be read or modified).

## 2. Phase 1 — Isomorphic `_id` normalization and strict comparison (`ShellCorpus_test.cpp`)

### 2.1 Ordinal `ObjectId` canonicalization

Normalization applies **only** to JSON object members whose key is literally `"_id"` and whose value is a single-key Extended JSON `ObjectId` object `{"$oid": "<24 hex digits>"}` (`[0-9a-fA-F]{24}`, exact member count `1`):

- Within each `ItemSummary` (`documents` and, when `type == "custom"`, `output`), distinct valid `ObjectId` hex strings (lowercased before lookup) are mapped in order of first appearance to `"000000000000000000000001"`, `"000000000000000000000002"`, … (`24` hex digits).
- Ordinal mapping preserves **relational isomorphism** inside every result item: two returned documents with distinct `_id` values never match two documents that accidentally share the same `_id`, while remaining completely independent of import timestamps, machine identifiers, and counters.
- **`item.output` lexical normalization (preserving field order).** `QJSEngine` / `ejsonTree` preserves BSON and JavaScript property order in `item.output` (`docs/Shell.md`), whereas `QJsonObject` sorts keys alphabetically. Therefore, `item.output` for `type == "custom"` is normalized by a dedicated lexical JSON token scanner that skips string literals (including `\"` and `\\` escapes) and rewrites only object member `"_id": {"$oid": "<24-hex>"}` hex payloads in place, leaving key order, numbers, whitespace, and string contents byte-for-byte intact.

### 2.2 What is never normalized

- **Scalar and composite `_id` values:** aggregation group keys (`"_id": "American"`, `"_id": "AK"`), explicit integer/string keys in `writes.json` (`"_id": 1`, `"_id": 7`), `null`, booleans, arrays, and multi-field `_id` objects (`{"state": "NY"}`) are compared verbatim.
- **Non-`_id` `ObjectId` fields and string outputs:** `{"refId": {"$oid": "..."}}`, `ObjectId('507f...').toString()`, and JSON strings containing `"_id":{"$oid":"..."}` inside text are untouched.
- **Malformed `$oid` values:** wrong length (`!= 24`), non-hex characters, non-string `$oid`, or extra sibling keys (`{"$oid": "...", "extra": 1}`) are left untouched and fail comparison.
- **Query and pipeline metadata:** `db`, `collection`, `limit`, `skip`, `batchSize` (for `query` items), `query`, `fields`, `aggrCollection`, `pipeline`, and `options` are compared verbatim.

### 2.3 MongoDB 4.0–8.0 `listIndexes` and `drop()` compatibility

- **`listIndexes` `"ns"` field (`SERVER-41696`):** For index descriptor objects matching the exact `listIndexes` shape (`v` integer, `key` non-empty object, `name` non-empty string, and legacy MongoDB ≤ 4.2 `ns` string containing `.`), the redundant `"ns"` field is stripped in both `documents` and `custom` JSON `output` before comparison and recording. Changes to `v`, `key`, `name`, or extra index properties still fail strictly.
- **`db.coll.drop()` return value on MongoDB 7.0+ (`SERVER-43894`):** MongoDB ≤ 6.0 throws `NamespaceNotFound` (`code: 26`) when dropping a non-existent collection, whereas MongoDB 7.0+ returns `{ok: 1}` without `ns` or `nIndexesWas` (and includes `ns` when a collection or view actually existed). `src/ragoshell/core/engine/js/02-cursors-collection.js` checks `Boolean(reply && (reply.ns !== undefined || reply.nIndexesWas !== undefined))` so `db.coll.drop()` returns `true` only when a collection or view was dropped across MongoDB 4.0–8.0.

### 2.4 Offline unit tests (`ShellCorpusCompareTest`)

`ShellCorpus_test.cpp` includes deterministic offline `gtest` cases that always run in `rago_unit_tests` with no MongoDB server reachable:

- Isomorphic fresh `ObjectId` `_id` values match across `documents` and `custom` `output`.
- Duplicate vs. distinct `ObjectId` `_id` sequences are rejected.
- Malformed `$oid` payloads (23/25 chars, non-hex, extra sibling key, plain string `_id`) are rejected.
- Scalar/group `_id` mismatches (`"American"` vs. `"Chinese"`, `1` vs. `2`), non-`_id` `$oid` mismatches, string-literal lookalikes, missing/extra fields, array length mismatches, and altered key order in `custom` `output` are rejected.
- MongoDB 4.0 vs. 4.4+ `listIndexes` `"ns"` normalization passes while altered `key`/`name`/`v` fails.
- `RAGOSHELL_TEST_MONGO` endpoint parser validates `host[:port]` and `[ipv6]:port` and rejects out-of-range ports, credentials, query strings, and whitespace.

## 3. Phase 2 — Deterministic seed and recorder (`dev/shell-corpus/`)

### 3.1 Test-only environment hooks (`productEnv`)

Read exclusively inside `ShellCorpus_test.cpp` (`rago_unit_tests`; absent from `ragoshell` and `ragoshell-mcp`):

| Variable | Purpose |
|---|---|
| `RAGOSHELL_TEST_MONGO` | Target `host[:port]` (default `localhost:27017`). |
| `RAGOSHELL_SHELL_CORPUS_DIR` | Directory from which `ShellCorpusTest` reads `*.json` (default `RAGO_SHELL_CORPUS_DIR`). |
| `RAGOSHELL_SEED_SHELL_CORPUS` | Path to a JS seed script (`dev/shell-corpus/seed-samples.js`) executed once in `SetUpTestCase` via `ScriptEngine` before corpus tests run. |
| `RAGOSHELL_RECORD_SHELL_CORPUS` | Output directory where `ShellCorpusTest` writes canonicalized, deterministic `reads.json`, `scripting.json`, `errors.json`, `writes.json`, and `completions.json` (`0`-initialized non-query paging fields, ordinal `$oid` placeholders). |

### 3.2 MongoDB 4.0+-compatible deterministic seed (`dev/shell-corpus/seed-samples.js`)

Written in plain ES5 JavaScript (compatible with legacy `mongo` 4.0, `mongosh`, and `QJSEngine` — no `$$NOW`, no 4.2+ pipeline `$set`, no object spread, no `async`/`await`, no `BigInt`) with zero external network downloads:

- Populates `samples` with the exact seven collections expected by `db.getCollectionNames()` (`books`, `companies`, `countries`, `grades`, `inspections`, `restaurants`, `zips`).
- `restaurants` (`25,359` documents): inserted sequentially in ordered batches with freshly generated `ObjectId()` `_id` values so `sort({_id: 1})` preserves exact insertion order while testing dynamic `ObjectId` normalization; preserves the canonical NYC records, top alphabetical Italian names, `address`/`borough`/`grades` structure, default `_id_` index, and exact cuisine counts (`American: 6183`, `Chinese: 2418`, `Café/Coffee/Tea: 1214`, `Italian: 1069`, plus 15 additional cuisines totaling `25,359`).
- `zips` (`29,353` documents across 51 US states/DC): 5-digit string `_id` values, uppercase `city`, `loc`, `pop`, and exact per-state counts (`AK: 195` through `WV: 656`, `WY: 140`).
- `books` (`431` documents with categorical `status` `"PUBLISH"` / `"MEAP"`) and non-empty `companies`, `countries`, `grades`, and `inspections` collections.

### 3.3 Recorder and verifier CLI (`dev/shell-corpus/record-corpus.sh`)

- `--ephemeral-docker [image]` (default `mongo:7`): starts a `tmpfs`-backed container bound strictly to `127.0.0.1::27017` (ephemeral OS-assigned loopback port, never `0.0.0.0` and never fixed `27017`), seeds `samples`, runs recording and/or verification, and removes the container unconditionally via `trap cleanup EXIT INT TERM`.
- `--mongo <host:port>`: uses an existing test server; refuses non-loopback hosts unless `--allow-remote` is passed, and drops/reseeds `samples` **only** when `--seed` is explicitly supplied.
- `--verify-only` vs. `--out <dir>`: verifies an existing corpus directory or records all five JSON files into `<dir>` and immediately re-verifies them with a clean `ShellCorpusTest` run.

## 4. Threats and isolation guarantees

| Risk | Mitigation |
|---|---|
| Touching author data or local port `27017` | `dev/mongo-test/` is never read or referenced by `record-corpus.sh`; `--ephemeral-docker` binds `127.0.0.1::27017` (dynamic high port) with `--tmpfs /data/db`. |
| Accidental `dropDatabase()` on a non-test server | `--mongo` requires loopback (`localhost` / `127.0.0.1` / `[::1]`) unless `--allow-remote` is given, and never seeds (`dropDatabase`) without explicit `--seed`. |
| Shell / URI injection via arguments or env vars | No `eval`; strict regex validation on `host:port` and image names; `parseTestMongoEndpoint` rejects `/`, `?`, `@`, `#`, whitespace, and control characters. |
| Shipping test hooks in production binaries | `RAGOSHELL_TEST_MONGO`, `RAGOSHELL_*_SHELL_CORPUS*` live solely in `ShellCorpus_test.cpp` (`rago_unit_tests`). |
