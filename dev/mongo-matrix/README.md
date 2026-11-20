# RagoShell live MongoDB compatibility matrix (`dev/mongo-matrix/`)

Runs the real app (`--matrix-selftest`, `src/ragoshell-app/MatrixSelfTest*.cpp`) against real MongoDB servers in
Docker: **every connection type on every MongoDB release line**, one version at a time. The self-test drives
`QmlBackend`, `ShellController`, `ServerMonitorController`, `SshTunnelWorker`, `MongoWorker`, `MongoClient` and
`CollectionTransfer`, so it checks what a user does in the UI, not a driver call in isolation.

Everything needed to repeat the run lives in this folder; nothing is installed on the host apart from Docker.

## Versions

`4.0, 4.2, 4.4, 5.0, 6.0, 7.0, 8.0, 8.2, 8.3, 9.0` — every release line with a Docker Official Image. When a new
line ships, add it to `ALL_VERSIONS` in `run-matrix.sh`; the compose file needs no change (`MONGO_IMAGE`).
4.0 knows only the old `--ssl*` option names; `mongod-run.sh` translates them, so all services share one definition.

## Suites (each runs on every version)

| Suite | Server (host port) | What is checked |
|---|---|---|
| `version` | `mongo-std` `--auth` + `allowTLS` (37040) | DEFAULT/SCRAM-SHA-256, SCRAM-SHA-1, custom `authSource`, restricted single-DB user + manually visible DBs, TLS with CA, X.509 (`MONGODB-X509`, `$external` users), user create/update/drop, collections (time series ≥5.0, clustered ≥6.0), indexes (wildcard ≥4.2, hidden ≥4.4), stored functions, shell CRUD, NDJSON export/import, server monitor, version detection |
| `noauth` | `mongo-noauth` (37039) | connection without credentials, shell CRUD, drop database |
| `mtls` | `mongo-mtls` `requireTLS` (37090) | refused without a client certificate; `client.pem` + SCRAM-SHA-256; passphrase-protected `client-enc.pem` + X.509 |
| `sharded` | config RS + shard RS + `mongos` (37217) | `MONGOS` badge and monitor role, TLS + SCRAM, X.509, user management and `listShards` through mongos |
| `ssh` | `ssh-bastion` (32222) | host key trust on first use, SSH password, Ed25519 key, encrypted Ed25519 key, RSA key + TLS, SSH + strict mTLS + X.509, SSH + mongos |
| `rs` | 3-member `rs0` (37117–37119) | set-name discovery from a secondary seed, `RS · rs0` badge, `secondaryPreferred` + TLS, `localhost` ↔ `127.0.0.1`, restricted user (`isMaster` fallback), shell per member |

Every port binds to `127.0.0.1`; data lives in `tmpfs`; certificates, SSH keys and the cluster keyfile are generated
into `.fixtures/` by `generate-fixtures.sh` (git-ignored, recreated per run).

## Running it

Needs Docker (Compose v2.24+), `openssl`, `ssh-keygen`, bash, and a RagoShell build.

```bash
# Linux / macOS / WSL: everything (10 versions x 6 suites)
dev/mongo-matrix/run-matrix.sh

# a few versions or suites
dev/mongo-matrix/run-matrix.sh --versions 4.0,8.0,9.0 --suites version,ssh

# a specific build
dev/mongo-matrix/run-matrix.sh --exe build/linux-release/src/ragoshell-app/ragoshell
```

On Windows run it from Git Bash; it picks up `dist/ragoshell-*-windows-x64/ragoshell.exe` or
`build/windows/src/ragoshell-app/Release/ragoshell.exe`, converts the paths it hands to Docker and the app, and uses
the `windows` Qt platform plugin (a `windeployqt` bundle has no `offscreen` plugin; no window is shown).

The result table is printed at the end and saved to `.results/matrix-<UTC time>.md`, with one log per suite in
`.results/logs/`. The script exits non-zero when any suite fails. `--keep-up` leaves the last containers running
for debugging; `docker compose -p rago-test-matrix -f dev/mongo-matrix/docker-compose.yml --profile standalone
--profile rs --profile sharded --profile ssh down -v` removes them.

A full run (60 suites) takes about 8 minutes on a 16-core Windows machine with Docker Desktop once the images are
pulled (about 20 minutes the first time); at most 7 `mongod`/`mongos` processes run at once.

## Starting one setup by hand

```bash
dev/mongo-matrix/generate-fixtures.sh dev/mongo-matrix/.fixtures
export MONGO_IMAGE=mongo:9.0
docker compose -p rago-test-matrix -f dev/mongo-matrix/docker-compose.yml --profile standalone up -d
dev/mongo-matrix/bootstrap-service.sh standalone mongo-std 27017
# then connect RagoShell to 127.0.0.1:37040 (users: see bootstrap-service.sh seed_users_and_data)
```
