# Shell Corpus Seeder & Recorder

Self-contained tooling for seeding the deterministic `samples` dataset and verifying or re-recording `src/ragoshell-unit-tests/shell-corpus/*.json` (`docs/ShellCorpusRecorder.md`).

## Safety & Isolation

- `--ephemeral-docker` binds exclusively to `127.0.0.1:<random>` (OS-assigned ephemeral port, never `0.0.0.0` and never port `27017`), stores data in container `--tmpfs /data/db`, and removes the container on exit.
- `dev/mongo-test/` is never read, modified, or referenced.
- `--mongo <host[:port]>` requires a loopback host unless `--allow-remote` is passed, and never drops/reseeds `samples` unless `--seed` is explicitly passed.

## Prerequisites

Build `rago_unit_tests` first:

```bash
cmake --build --preset macos-release --target rago_unit_tests
```

## Verify the Checked-In Golden Corpus

Start an ephemeral `mongo:7` container, seed `samples`, verify all 107 corpus scripts + 64 completion prefixes + offline comparator tests, and tear down:

```bash
./dev/shell-corpus/record-corpus.sh --ephemeral-docker --verify-only
```

Verify against a specific server image (for example `mongo:4.0` or `mongo:8.0`):

```bash
./dev/shell-corpus/record-corpus.sh --ephemeral-docker mongo:8.0 --verify-only
```

Verify against an already-running local test server (re-seeding `samples` explicitly):

```bash
./dev/shell-corpus/record-corpus.sh --mongo 127.0.0.1:27018 --seed --verify-only
```

## Record / Refresh the Golden Corpus

Record into a temporary directory first to inspect diffs cleanly:

```bash
./dev/shell-corpus/record-corpus.sh --ephemeral-docker --out /tmp/rago-corpus-preview
diff -ru src/ragoshell-unit-tests/shell-corpus /tmp/rago-corpus-preview
```

Overwrite `src/ragoshell-unit-tests/shell-corpus/*.json` in place (records canonicalized ordinal `$oid` placeholders and zeroed non-query paging fields, then runs a clean verification pass):

```bash
./dev/shell-corpus/record-corpus.sh --ephemeral-docker
```

## Seeding Standalone with `mongosh` or `mongo`

`dev/shell-corpus/seed-samples.js` is plain ES5 JavaScript compatible with MongoDB 4.0+ (`mongo`, `mongosh`, and RagoShell's `ScriptEngine`):

```bash
mongosh "mongodb://127.0.0.1:27018/samples" dev/shell-corpus/seed-samples.js
```
