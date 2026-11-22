# RagoShell — Schema Intelligence

Status: implemented. The app learns the shape of the collections you work with, in
the background and gently, and uses it for field-name autocomplete in the shell
editor, the AI query assistant (including `$lookup` into related collections) and the
Schema dialog. Manual checklist: `docs/ManualTests.md` § 26.

## Pieces

In `src/ragoshell/core/utils/` (rago-core, no QML): `SchemaAnalysis` (samples one
collection), `SchemaService` (job queue per connection), `SchemaCollector` (what to
analyze in the background), `SchemaCatalog` (store, one JSON file per database),
`FieldCompletion` and `AiQueryAssistant`. In `src/ragoshell-app/`: `SchemaAnalyzer` with
`qml/SchemaDialog.qml`, `AiAssistant`, and the autocomplete plumbing in
`ShellController.cpp` and `qml/ShellTabPage.qml`. The MCP tool `collection_schema`
(`src/ragoshell/core/mcp/`) uses `SchemaAnalysis` directly, not the catalog.

Every sampling job goes through `SchemaService`; no other code starts a sampling
thread. Service and catalog are created in `src/ragoshell-app/app/main.cpp`.

## What an analysis reads

`SchemaAnalysis::analyzeCollection` opens its own client from the connection's
settings, so it does not use or block the user's shell connection.

- Up to `kFullScanLimit` (5000) estimated documents: the whole collection.
- Larger: `sampleSize` documents, 30 % (`kRecentShare`) the newest by `_id` and the
  rest `$sample`, so fields added lately are not missed. The newest part is used
  only when `_id` is an ObjectId; no document is counted twice. Fields seen only in
  the newest part are flagged `onlyRecent`.
- Also the estimated size and the index keys (a missing `listIndexes` privilege only
  leaves the list empty). Every call has `maxTimeMs`: 30 s for service jobs, 10 s
  for the collection listing.
- Per field path (`address.city`, `grades.score`, array elements included):
  presence, BSON types, array item types, min and max (numbers and dates only) and
  frequent values: at most 50 distinct values tracked, the top 5 kept (up to 25 for
  a string field with few repeating values, `categorical`), and "upper" / "lower"
  letter case when every string has it.

Sample sizes: Schema and AI dialogs 1000 by default (the Schema dialog allows up to
100000); background jobs 200 (50 to 10000 in Preferences); the MCP tool 100 (max 1000).

## Catalog

- Location: `<app data dir>/schema/<connection uuid>/<database>.json` (`~/.ragoshell/`
  on desktop; `RAGOSHELL_CONFIG_DIR` overrides it), written with `QSaveFile`. Memory
  trimming drops the in-memory copies; the files are read again on use.
- A snapshot is stale when it is older than 7 days or the document count moved by
  more than 25 % (against a base of at least 100, so tiny collections do not churn).
  The count is read from the server only for jobs that run `onlyIfStale`.
- "Forget Schema Data" in the connection menu (`QmlBackend::forgetSchema`) cancels the
  connection's jobs, resets the collector session, deletes the connection's folder and
  refreshes the tree. Deleting a saved connection deletes its folder too.

## SchemaService

- Priorities: `Interactive` (a dialog waits), `Hint` (the user works with the
  collection), `Background`; highest first, oldest first. A higher job jumps the queue
  but does not cancel the running one.
- One job at a time per connection (connections run side by side), on a detached
  worker thread. A job for a collection already queued is merged (higher priority,
  bigger sample, values if either wanted them); a running job that covers a request
  serves its ticket. A fresh snapshot is not sampled again, and a smaller sample
  never replaces a fresh bigger one.
- A "not authorized" error stops background jobs for that database for the session
  (one warning in the Log panel, no retry loop); `Hint` and `Interactive` jobs still
  try. Other background failures are retried once, then logged.
- Disconnect drops the queue and the running job's result and forgets permission stops.
- Background jobs pause (they stay queued) while background work is not allowed
  (`BackgroundMonitor`, `BackgroundPolicy`): app suspended; on phones and tablets,
  app not in front; on a metered network when "Wait while the connection is metered"
  is on (default).

## Background collector

Mode (`schemaCollectionMode`, in the settings file `schema.collectionMode`): `Off`,
`OnUse` (default, "Databases in use") or `WholeDatabase` ("All databases").
Preferences › Schema analysis also sets "Documents per collection"
(`schemaBackgroundSampleSize`, 200), "Collections per database"
(`schemaBackgroundMaxCollections`, 50 per database and session, 1 to 1000) and the
metered-network option. A connection can override the mode (connection dialog ›
Schema analysis, stored as `schemaCollectionMode` in the connection; empty means "as
in Preferences").

Triggers (all respect the mode; `Off` disables the first three):

1. A database comes into use: its collection list loads in the tree, or a shell
   switches to it. It is listed once per session and its collections without a fresh
   snapshot are queued as `Background` jobs, smallest first (small ones are analyzed
   whole and cheaply, unknown sizes last), up to the limit.
2. A shell names a collection (`db.orders.`, `db.getCollection('orders').`, or a field
   position in it that has no snapshot): a `Hint` job, once per collection and session.
3. `WholeDatabase`: when the database list arrives, every database is put in use.
4. "Analyze Schema of All Collections" (database menu) and "Analyze the Rest" in the AI
   dialog ignore the mode and the limit and queue `Hint` jobs, which a permission stop
   does not block.

Skipped in the background: `system.*` collections (time-series buckets included),
views (their pipeline would run on every sample; the Schema dialog can still analyze
them) and the `local` and `config` databases. Jobs run `onlyIfStale`: reopening a
database costs one cheap count per collection.

UI: a dot after an analyzed collection in the tree (dim when stale, tooltip with
documents sampled and time), `◷ analyzed/total` on a database while jobs run, one Log
panel line per database batch.

## Field-name autocomplete

`FieldCompletion::analyze` scans the last 8000 characters before the caret (strings,
comments, template and regex literals are skipped) for the collection and the kind of
position. It is a pragmatic scanner, not a JS parser. Fields are offered for: keys of
a filter (also inside `$and` / `$or` / `$nor` and `$elemMatch`), of a projection, of
`sort` and `createIndex`, and of update operator bodies; keys in `$match`, `$sort` and
`$project` stages; `"$field"` references inside `aggregate()` expressions and
`$project`; and the argument of `distinct("...")`. Nothing is offered for values,
operator keys (the shell's own list stays), strings, comments, `db.getSiblingDB(...)`,
or a receiver that is not `db.<collection>` / `db.getCollection('<collection>')`.

`ShellController::requestFieldCompletion` answers synchronously from the catalog, so
there is no network call on the UI thread. Without a snapshot it offers nothing and
requests a `Hint` job; the next trigger shows the fields. The list opens after `.`,
`$`, `{`, `,` and quotes in a key position, and with Ctrl+Space. Each entry shows type
and presence (`String · 98%`), fields in under 5 % of the documents go last, at most
200 are shown. Plain identifiers are inserted as they are, dotted paths quoted
(`"address.city"`), references as `$address.city`. Only paths, types and presence are
used, so it works with sample values off. The Autocompletion preference `Off` disables it.

## AI assistant

- `AiAssistant` uses the catalog snapshot of the open collection if there is one
  (refreshed with a `Hint` job when stale); otherwise it samples 1000 documents first
  (`Interactive`, 30 s limit). This works with background analysis `Off` too.
- `AiQueryAssistant::relatedCollections` adds other analyzed collections of the database
  for `$lookup`, until `kRelatedBudget` (6000 characters): collections named in the
  question (singular or plural), collections the open one references (`customerId`,
  `customer_id`, ObjectId fields), collections that reference the open one, then the
  rest by size. Fields are cut by presence when a collection does not fit. The ranking
  is redone for every question and new snapshot, so the "Data sent" preview is exact.
  While the database is not fully analyzed the dialog offers "Analyze the Rest".
- `parseResponse` rejects a script whose `$lookup` / `$graphLookup` `from` or
  `$unionWith` names a collection that was not described.

## Privacy

- Sample values (frequent values, min and max, letter case) are written to the catalog
  and sent to the AI provider only when `aiIncludeSampleValues` is on (Preferences, off
  by default). Snapshots stored without values are sampled again when it is turned on.
  Related collections never carry values.
- Nothing reaches the AI provider before the user presses Generate, and the dialog shows
  what is sent. Autocomplete and the tree markers stay local. Background analysis can be
  turned off in Preferences or per connection; "Forget Schema Data" deletes the catalog.
- The MCP tool is separate: `ragoshell-mcp` analyzes per call (15 s limit by default),
  writes nothing to the catalog and returns values when the client passes
  `includeValues: true`, independent of `aiIncludeSampleValues`.

## Decisions

- Default mode is `OnUse`: safer on production servers and Atlas than touching every
  database on connect; `WholeDatabase` is available per connection.
- Background samples are 200 documents, enough for names, types and presence. Jobs use
  the connection's own settings (its read preference applies).
- One shared catalog, no second cache. The app bundles no language model: the AI path
  goes to the provider the user picks (Anthropic, Gemini, an OpenAI-compatible server or
  Ollama, which can run locally; see `docs/AiAssistant.md`).

Tests are `Schema*_test.cpp` (the service tests use a fake analysis function; one live
collector test skips without `localhost:27017`), `FieldCompletion_test.cpp`,
`AiQueryAssistant_test.cpp` and `McpServer_test.cpp`, all in `src/ragoshell/core`.

## Open items

- No cost measurements are recorded (time and server load of a background run, for
  example per 100 collections); the scripted checks ran without a MongoDB server.
- `docs/ManualTests.md` § 26 is mostly unticked (17 items): replica set and Atlas, a
  permission-limited user, `Off`, field completion in the real editor (no editor
  self-test exists), AI `$lookup` on real data, Forget Schema Data. `dev/mongo-test` is
  git-ignored, so no seeded `customers` / `orders` pair is in the repository.
- `SchemaCatalog::coverage` is only used by tests.
- The MCP tool description says "random `$sample`"; the analysis also reads small
  collections whole and the newest 30 % of a larger sample.
