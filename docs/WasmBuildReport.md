# WASM Build Research — Go/No-Go for `ragoshell-web` (roadmap 8.1, WebAssembly.md Faz 1)

Written: 2026-11-03 · answers the three questions of Phase 8.1 before code:
can Qt WASM compile this app, can mongo-c-driver/OpenSSL be Emscripten-built,
and what does Mod A (in-browser demo) need to work.

## 1. Qt WASM port — feasible

- Qt 6.12 `wasm_singlethread`/`wasm_multithread` are supported targets via the
  Emscripten SDK + `qt-cmake -DCMAKE_TOOLCHAIN_FILE=…/qt.toolchain.cmake`.
- This app's QML (Qt Quick Controls Basic, QJSEngine shell, ScintillaQuick)
  compiles for WASM in principle; `QScintilla` has known WASM builds upstream.
- Size: the QML runtime + app ≈ 15–25 MB `.wasm` before Brotli; acceptable for
  a `demo` page, marginal for a daily-driver UI (loading time, mobile Safari
  memory caps ~1.5 GB RAM — fine for the demo scope).
- **Pitfalls**: no threads on `wasm_singlethread` (our MongoWorker runs on a
  `QThread` — needs the multithread build + `-pthread` + COOP/COEP headers, or
  a single-thread transport shim); `QSettings`/files live in memory unless
  IDBFS sync is wired (Faz 2); no raw sockets (browser) — the reason for
  `IMongoTransport`.

## 2. mongo-c-driver / OpenSSL under Emscripten — no-go (direct)

- libmongoc needs real TCP/TLS sockets; Emscripten has none (browser model).
  Building it for WASM is wasted work — it cannot open a socket.
- Conclusion: do **not** port libmongoc. Revised (2026-11-03, after reading the
  dependency graph): bsoncxx/mongocxx headers are woven through ~60 files —
  domain objects, events, settings, BSON utils — so "compile without mongocxx"
  is the wrong split. bsoncxx itself is header+lib C++ that Emscripten can
  build; what must be replaced is the *socket path*, i.e. the client factory
  inside `MongoClient`/`MongoWorker` (`getConnection`, `buildClientOptions`).
- The seam therefore sits at the client factory: commands cross it already
  serialized (BSON→EJSON), and `IMongoTransport::Result` mirrors the command
  triplet. BSON handling stays as-is on both sides of the seam.
- Mod A demo therefore uses `InMemoryDemoTransport` (canned dataset
  `catalog.products/customers/orders`, answers find/aggregate locally).
- Real connections need Mod B (self-hosted WS gateway, Phase 8.4) or Mod C
  (`127.0.0.1` agent, Phase 8.3) — transport carries EJSON commands over
  WebSocket; TLS termination stays in the browser↔gateway hop.

## 3. `IMongoTransport` — the right seam

`MongoClient`/`MongoWorker`'s *connection factory* is the split point, not the
whole worker API: the ~25 event handlers and BSON plumbing are shared, only
"how bytes reach a server" differs. `IMongoTransport` (core/transport/) takes
EJSON in/out — `NativeMongoTransport` wraps today's mongocxx path unchanged;
`InMemoryDemoTransport` (done, 7 tests) serves Mod A; `WebSocketGatewayTransport`
comes with Mod B/C. The refactor extracts `MongoWorker`'s mongocxx calls into
`NativeMongoTransport`; handlers then talk EJSON through the interface.

## 4. Go/No-Go

**GO** for Mod A: WASM build of the shell UI + demo dataset is achievable with
a `SYSTEM_WASM` flag, Emscripten presets, the transport seam, and
`wasm_multithread` + COOP/COEP on the demo host. Mod A has no server costs —
static files on `dl.`/site.

**Defer**: Mod B/C require their own threat review (prompts 8.3–8.5); direct
libmongoc-on-Emscripten is a confirmed no-go — do not attempt it.

## 5. Threat list for Faz 1 code

- Bundle leaks: the demo must embed **no** real credentials/URIs — dataset is
  synthetic; `InMemoryDemoTransport` returns fixed documents.
- CSP on the demo host: `script-src 'self' 'wasm-unsafe-eval'`, COOP/COEP for
  threads; no third-party calls.
- Filesystem writes go to memory only until Faz 2 IDBFS — settings must not
  pretend to persist.

## 6. Empirical build — DONE (2026-11-03)

The pipeline is no longer theory; the app links for `wasm32-emscripten`:

- **SDK**: Qt 6.12 pins Emscripten **5.0.5** (`QtPublicWasmToolchainHelpers.cmake`
  hard-fails on 6.x) — `emsdk install 5.0.5 && emsdk activate 5.0.5`.
- **Deps**: vcpkg `wasm32-emscripten` community triplet builds the full stack —
  argon2, OpenSSL, libssh2, libbson, mongo-c-driver, mongo-cxx-driver,
  utf8proc, zlib. The repo overlay portfile for mongo-c-driver gained
  `VCPKG_CMAKE_SYSTEM_NAME STREQUAL "Emscripten"` → `ENABLE_SRV=OFF`
  (no `res_nsearch` in the browser, same reasoning as Android/iOS).
- **Preset**: `wasm-single` now chainloads Emscripten.cmake through the vcpkg
  toolchain (same pattern as android/ios) and pins `CMAKE_SIZEOF_VOID_P=4`
  (wasm try_compile cannot probe it).
- **Qt wasm helpers**: `qt.toolchain.cmake` normally includes
  `QtPublicWasmToolchainHelpers.cmake`; under the vcpkg chainload the
  top-level CMakeLists includes it directly when
  `CMAKE_SYSTEM_NAME STREQUAL "Emscripten"`.
- **Output** (`build/wasm-single/src/ragoshell-app/`): `ragoshell.html` 3.0 KB,
  `ragoshell.js` 245 KB, `ragoshell.wasm` 52 MB, `ragoshell.wasm.br` **15 MB**
  (post-build `brotli -9` custom command in `src/ragoshell-app/CMakeLists.txt`).
- **Demo seed**: under `Q_OS_WASM`, `main.cpp` seeds one connection
  `RagoShell Demo (in-browser)` (`mongodb://demo/`, db `catalog`,
  `transportKind = "memory"`) when no connections exist; `MongoWorker` turns
  `transportKind` into `InMemoryDemoTransport` via `_ownedTransport`
  (docs/TransportSeam.md §4). Native paths are untouched — `hasTransport()`
  is false for every existing connection.
- **Licences**: `LicensesDialog.qml` gained a `Source Code` pill linking
  `Brand.sourceRepoUrl` (GPL §6 source offer on the web, WebAssembly.md §1.4).
- **Remaining for 8.2+**: singlethread vs multithread preset choice,
  COOP/COEP headers on the demo host, IDBFS persistence, QML smoke in a real
  browser (unit tests are native-only).
