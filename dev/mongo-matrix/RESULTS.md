# Matrix results

Latest full run, kept here because `.results/` is not committed. Add a new section on top after each release-relevant
run (new MongoDB line, connection code change, driver or OpenSSL/libssh2 update).

## 2026-11-04 — 60/60 suites, 678 checks passed

- App: RagoShell 2.1.0, Windows x64 release bundle (`dist/ragoshell-2.1.0-windows-x64`), commit `5ceb433` + matrix changes
- Host: Windows 11, Docker Desktop 29.9.2 (WSL 2), Git Bash
- Images: `mongo:4.0` (4.0.28) … `mongo:9.0` (9.0.4), pulled 2026-11-04

| MongoDB | version | noauth | mtls | sharded | ssh | rs |
|---|---|---|---|---|---|---|
| 4.0 | ok | ok | ok | ok | ok | ok |
| 4.2 | ok | ok | ok | ok | ok | ok |
| 4.4 | ok | ok | ok | ok | ok | ok |
| 5.0 | ok | ok | ok | ok | ok | ok |
| 6.0 | ok | ok | ok | ok | ok | ok |
| 7.0 | ok | ok | ok | ok | ok | ok |
| 8.0 | ok | ok | ok | ok | ok | ok |
| 8.2 | ok | ok | ok | ok | ok | ok |
| 8.3 | ok | ok | ok | ok | ok | ok |
| 9.0 | ok | ok | ok | ok | ok | ok |

Fixed on the way (test harness only, no app change was needed):
- MongoDB 4.0 strict mTLS bootstrap: the 4.0 `mongo` shell (OpenSSL 1.0.2) does not match IP SANs; it now connects
  as `localhost`. The server certificate also lists the current service names (`mongo-std`, `mongo-noauth`).
- Replica set: the suite opened shells on secondaries before they had replicated the seeded users (seen once on
  4.2); the bootstrap now waits for a write acknowledged by all three members.
- The topologies (mTLS, replica set, sharded, SSH) ran only on MongoDB 7 before; they now run on every version.

Not covered: `mongodb+srv://` (needs DNS SRV records), LDAP / Kerberos (Enterprise only), MongoDB Atlas, and the
Linux/macOS builds (same connection code; run `run-matrix.sh --exe <binary>` there).
