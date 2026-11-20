# SSH Tunnel Automatic Re-establishment (M9)

Design, root-cause analysis, and security threat model for roadmap item **M9** — re-establishing broken SSH tunnels automatically while preserving local loopback isolation, strict `known_hosts` verification, and transparent session continuity for open MongoDB workers and JavaScript shell tabs.

---

## 1. Root-Cause Analysis

In the existing architecture, an SSH-tunneled MongoDB connection breaks permanently whenever the underlying SSH transport drops (Wi-Fi switch, laptop sleep/wake, tablet suspension, NAT idle timeout, or `sshd` restart):

1. **Unowned fire-and-forget worker thread (`App.cpp`, `SshTunnelWorker.cpp`).**
   `App::openServerInternal` allocated `new SshTunnelWorker(settingsCopy)` on a `QThread` wired to `deleteLater()` without storing any pointer on `App` or `MongoServer`. Once `rbm_ssh_open_tunnel` entered its blocking C `select()` loop, the `QThread`'s event loop was blocked and `SshTunnelWorker` could neither receive control events nor be stopped/restarted from outside. When `rbm_ssh_open_tunnel` exited, `SshTunnelWorker` called `stopAndDelete()`, destroying itself while `MongoServer` remained open in the Explorer.
2. **Premature listener suicide when client channels drop to zero (`ssh.c`).**
   `handle_ssh_connections` and `handle_client_connections` cleared `localsocket` from `masterset` whenever `connection->channelssize == 0`, causing `rbm_open_tunnel` to call `rbm_ssh_session_close(connection->publicsession)` and exit. Whenever `libmongoc` recycled an idle/broken pooled socket or `ConnectionTest` / a shell closed its temporary socket, the tunnel closed `127.0.0.1:<localPort>` and terminated.
3. **No port preservation in `socket_listen` (`ssh.c`).**
   `socket_listen` ignored `*port` on input (`sin.sin_port = htons(0)`) while `SshTunnelConfigCreator` initialized `_localport = 27040`. Meanwhile, `App::continueOpenServer` rewrote `MongoServer`'s `ConnectionSettings` to `127.0.0.1:<localPort>`, and every downstream consumer (`MongoWorker`, open `JsShellBackend` shell tabs, `ServerMonitorController`, `SchemaCollector`, `CollectionTransfer`) bound its `mongocxx::client` URI to `127.0.0.1:<localPort>`.
4. **Immediate single-shot failure without backoff (`ssh.c`).**
   - When an idle SSH session died and a local client connected to `127.0.0.1:<localPort>`, `libssh2_channel_direct_tcpip_ex` failed with `RBM_CHANNEL_CREATION_ERROR` and `rbm_ssh_open_tunnel` returned `RBM_ERROR` immediately without attempting to reconnect SSH.
   - When `libssh2_channel_read` failed during an active outage, `rbm_ssh_open_tunnel` called `rbm_ssh_setup` once with zero delay; because the network was still down, setup failed immediately, `SshTunnelWorker` closed `localsocket`, emitted a fatal `ConnectionFailedEvent`, and deleted itself.
5. **Indefinite blocking in `select()` and `connect()` (`ssh.c`).**
   `rbm_open_tunnel` passed `NULL` timeout to `select()` with no wakeup descriptor, and `socket_connect` used a blocking TCP `connect()`, preventing deterministic `stop()` / `join()` when tearing down or restarting a tunnel.

---

## 2. Step-by-Step Implementation Plan

### Step 1 — Hardened C SSH Transport (`src/ragoshell/ssh/ssh.h`, `private.h`, `ssh.c`)

- **Error classification (`enum rbm_ssh_error_code`)**: expose `RBM_SSH_ERR_NONE`, `RBM_SSH_ERR_NETWORK`, `RBM_SSH_ERR_HOSTKEY`, `RBM_SSH_ERR_AUTH`, `RBM_SSH_ERR_LOCAL_BIND`, and `RBM_SSH_ERR_STOPPED` on `rbm_ssh_session` so the C++ supervisor distinguishes transient network drops from fatal security/authentication errors and local port collisions.
- **Loopback-only listener with preferred port (`socket_listen`)**:
  - Reject any `ip` other than `"127.0.0.1"` and bind exclusively to `htonl(INADDR_LOOPBACK)`.
  - Bind `htons(*port)` when `*port` is in `1..65535`, or `htons(0)` (ephemeral) when `*port == 0`; write the assigned port back via `getsockname`.
  - Set `SO_REUSEADDR` on POSIX (`#if !defined(_WIN32) && !defined(WIN32)`) so rebinding `127.0.0.1:<localPort>` after closing the previous listener succeeds even if former local client sockets are in `TIME_WAIT`. Never set `SO_REUSEPORT` on any platform, and never set `SO_REUSEADDR` on Windows (where Winsock `SO_REUSEADDR` allows active port hijacking).
- **Interruptible I/O and clean shutdown (`rbm_ssh_session_stop`)**:
  - Add an atomic `stop_requested` flag on `rbm_session` plus a non-blocking POSIX wakeup pipe (`wakefds[2]`) monitored in `select()` (with bounded `timeval` fallback on both POSIX and Windows).
  - Make `socket_connect` non-blocking with bounded connect timeout (10 s) and `stop_requested` / wakeup-pipe polling, and set `libssh2_session_set_timeout(session, 10000)` during handshake and authentication so `stop()` and `join()` never hang on blackholed routes.
- **Persistent listener across zero-channel idle periods**:
  - Keep `localsocket` in `masterset` when `channelssize` drops to `0` so local clients can open new channels at any time while the `MongoServer` remains open.
  - Distinguish remote target refusal (`LIBSSH2_ERROR_CHANNEL_FAILURE`: SSH transport still healthy, close only the failed client socket) from dead SSH transport (`SOCKET_DISCONNECT`, `SOCKET_SEND`, `SOCKET_RECV`, `TIMEOUT`, `PROTO`: return `RBM_ERROR` with `RBM_SSH_ERR_NETWORK` so the C++ supervisor reconnects).
  - Make `rbm_ssh_session_close` the sole owner of freeing `rbm_ssh_session` (removing the asymmetric `rbm_ssh_session_close` inside `rbm_open_tunnel` and fixing socket/`addrinfo`/`LIBSSH2_SESSION` leaks on error paths).

### Step 2 — Supervisor, Backoff & Injectable Runner (`src/ragoshell/core/mongodb/SshTunnelWorker.*`)

- **Testable session interface (`ISshTunnelSession` / `SshTunnelSessionFactory`)**:
  - Extract tunnel session setup, forwarding, and stop signaling behind `ISshTunnelSession`, with `Libssh2TunnelSession` as the default production implementation and deterministic fake/loopback runners for `rago_unit_tests`.
- **Owned `std::thread` with two-level locking**:
  - Replace the self-deleting `QThread` with a `std::thread` owned directly by `SshTunnelWorker`.
  - `_lifecycleMutex` (acquired only by external callers: `startInitialConnect`, `restart`, `onNetworkResumed`, `stop`, `~SshTunnelWorker`) serializes thread stop + `join()` + start.
  - `_stateMutex` + `_stateCv` (leaf lock shared with the worker thread) guards state flags, active session pointer, bound local port, attempt counter, and interruptible backoff waits. The worker thread never acquires `_lifecycleMutex`, making deadlock impossible.
- **Same-port priority & port-busy fallback**:
  - Initial connect requests `localPort = 0` and records `_boundLocalPort`.
  - Every reconnect / `restart()` first stops and `join()`s any prior session/thread (closing the old listener socket completely), then calls `setup(_boundLocalPort, /*interactiveHostKey=*/false)`.
  - If `127.0.0.1:<_boundLocalPort>` was taken by an unrelated local process (`SshTunnelErrorKind::LocalBind`), the worker logs a warning, retries once with `localPort = 0`, updates `_boundLocalPort`, and emits `tunnelReconnected(newPort, /*portChanged=*/true)`.
- **Controlled exponential backoff & storm prevention**:
  - Default `SshReconnectPolicy`: `initialDelayMs = 500` (fast enough to recover inside `libmongoc`'s 5 s `serverSelectionTimeoutMS` when the server is reachable), `multiplier = 2.0`, `maxDelayMs = 30000` (30 s ceiling), `maxAttempts = 8`.
  - `onNetworkResumed()` coalesces duplicate wake/reachability bursts within a `resumeCooldownMs` window (`2000 ms`): if a fresh connect or handshake is already in flight it does not tear it down; if sleeping in backoff it wakes `_stateCv` immediately with reset attempts; if `Running` after a real network/sleep transition (outside the cooldown window) or `Exhausted`, it performs a clean `restart()` so half-open blackholed SSH sockets are replaced immediately.
  - Explicit `stop()` sets `_userStopped = true` permanently; neither internal backoff nor external `restart()` / `onNetworkResumed()` can ever start a session after `stop()`.

### Step 3 — Ownership & App/Shell Integration (`App`, `MongoServer`, `MongoWorker`, `QmlBackend`)

- **Explicit tunnel ownership**:
  - `App` holds pending `std::unique_ptr<SshTunnelWorker>` instances by `serverHandle` during initial setup and transfers ownership to `MongoServer` via `MongoServer::attachSshTunnel(...)` once `EstablishSshConnectionResponse` succeeds.
  - `MongoServer::~MongoServer()` calls `_sshTunnel->stop()`, deterministically stopping and joining the tunnel thread when the user disconnects, when `ConnectionTest` closes its temporary server, or on application exit.
- **Transparent continuity for `MongoWorker` and `JsShellBackend`**:
  - `JsShellBackend` does not open its own SSH tunnel; each shell tab (`ConnectionSecondary` `MongoServer`) connects via `mongocxx::client` to `127.0.0.1:<localPort>`. When the tunnel re-binds the same `127.0.0.1:<localPort>`, `MongoWorker`, all open `JsShellBackend` tabs, `ServerMonitorController`, and `SchemaCollector` continue working transparently without URI changes or losing JS engine state.
  - In the rare `LocalBind` fallback where `_boundLocalPort` changes, `MongoServer` updates `_connSettings` and `MongoWorker::updateLocalServerPort(newPort)`, and `App` propagates the new port to any open `MongoShell` secondary `MongoServer` instances bound to that primary server.
- **Resume path (`BackgroundMonitor` → `QmlBackend::recheckConnections`)**:
  - `QmlBackend::recheckConnections()` invokes `server->recheckAfterNetworkChange()`, which coordinates `SshTunnelWorker::onNetworkResumed()` and triggers `loadDatabases()` once the tunnel is ready, keeping errors inside the 20 s status-bar quiet window (`autoRefreshing(uuid)`).

### Step 4 — Unit Tests & Documentation Cleanup

- Add `src/ragoshell/core/mongodb/SshTunnelWorker_test.cpp` to `rago_unit_tests` covering:
  1. Unexpected tunnel drop + automatic recovery and explicit `restart()` joining the old thread cleanly and rebinding the **exact same `127.0.0.1:<localPort>`** (verified both via injectable fake session runner and real loopback TCP listener socket).
  2. Concurrent/repeated `restart()` calls across multiple threads without races, double-starts, or port leaks, plus `onNetworkResumed()` burst coalescing.
  3. Explicit `stop()` / disconnect preventing any further background retries or subsequent `restart()` calls.
  4. Exponential backoff delay progression with ceiling, attempt-limit exhaustion (`maxAttempts`), and immediate halt on `HostKey` mismatch and `Auth` rejection.
  5. Loopback-only enforcement (`0.0.0.0` rejected in `socket_listen`) and fallback when the previous local port is held by another socket.
- Update `docs/Platforms.md` (§2 lifecycle note and §6 open items), delete completed `M9` from `docs/TodoMac.md`, and tick `M9` in `docs/Roadmap.md` §11.

---

## 3. Security Threat Model

| # | Threat | Mitigation & Enforced Invariant |
|---|---|---|
| 1 | **MITM / Host-Key Change During Auto-Reconnect (`known_hosts` bypass)** | `verify_hostkey` in `ssh.c` runs immediately after `libssh2_session_handshake` and **before** `libssh2_userauth_*` on every initial and reconnected session. During unattended reconnects (`interactiveHostKey == false`), only an exact `LIBSSH2_KNOWNHOST_CHECK_MATCH` against `<appDataDir>/ssh/known_hosts` is accepted; `RBM_HOSTKEY_CHANGED` and `RBM_HOSTKEY_NEW` are rejected immediately (`return 0`, `RBM_SSH_ERR_HOSTKEY`), disconnecting before any password or public-key signature is sent and permanently halting automatic retries until the user reconnects interactively. |
| 2 | **Local Listener Exposure or Port Hijacking (`0.0.0.0` / `SO_REUSEPORT`)** | `socket_listen` rejects any non-`127.0.0.1` address and binds strictly to `htonl(INADDR_LOOPBACK)`. `SO_REUSEPORT` is **never** used on any platform, and `SO_REUSEADDR` is **never** enabled on Windows. A new listener socket is opened **only after** the previous `rbm_ssh_session` has closed its listener FD and its worker thread has been `join()`ed. |
| 3 | **Thread Races, Double-Start, Use-After-Free, or Shutdown Deadlock** | `SshTunnelWorker` owns its `std::thread` directly (no `deleteLater()` race). `_lifecycleMutex` serializes all external `start`/`restart`/`stop`/destructor calls; the worker thread only ever takes leaf `_stateMutex`, so `_workerThread.join()` under `_lifecycleMutex` cannot deadlock. Non-blocking `socket_connect` + `libssh2_session_set_timeout` + `wakefds` pipe ensure `stop()` completes promptly even on blackholed networks. |
| 4 | **Credential / Passphrase Leakage & Account Lockout** | Reconnect, error, and debug logs record only SSH host, port, local loopback port, attempt index, and transport status — never `userPassword`, `passphrase`, or `askedPassword`. `~SshTunnelConfigCreator()` wipes in-memory secret copies on teardown. Explicit server authentication rejections (`RBM_SSH_ERR_AUTH`) halt automatic retries immediately so rotated credentials cannot trigger `fail2ban` or PAM lockouts. |
