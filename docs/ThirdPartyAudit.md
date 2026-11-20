# RagoShell — In-Tree Third-Party Audit & Architecture Decision Record (E7)

- **Status:** Accepted
- **Date:** 2026-11-02
- **Roadmap Item:** `docs/Roadmap.md` §2 E7 & §11 E7 (`QScintilla/googletest kararı`)
- **Components Covered:** `QScintilla 2.8.4` (embedding `Scintilla 3.3.6`), `googletest 1.8.1` *(with inventory cross-reference to `QJson 0.8.1`)*

---

## Executive Summary

Most C/C++ dependencies in **RagoShell** (`mongo-cxx-driver`, `mongo-c-driver`, `libbson`, `openssl`, `libssh2`, `zlib`, `utf8proc`) are resolved from a commit-pinned `vcpkg.json` manifest with SHA-512 port verification and SPDX 2.3 SBOM generation (`ci/generate-sbom.mjs`). Two historical libraries imported in `src/third-party/` predated the Qt 6 migration and warranted an explicit security, maintenance, and architectural review under Roadmap item **E7**:

1. **`QScintilla 2.8.4` (`src/third-party/qscintilla-2.8.4/`, embedding `Scintilla 3.3.6`):**
   - **Decision (`ADR-E7-01`):** **Keep the in-tree patched `2.8.4` build** for the short and medium term under explicit compensating controls and defined replacement triggers.
   - **Why:** RagoShell does **not** use QScintilla's desktop `QsciScintilla` widget UI. Instead, `ScintillaQuickItem` (`QQuickPaintedItem`) subclasses Scintilla's internal `ScintillaBase` directly (`ScintillaQuickBackend`) and relies on five sets of custom Qt 6.12, logical-pixel HiDPI, headless-window, bounded HarfBuzz UTF-8 shaping, CJK IME byte-offset, and macOS/mobile adaptations inside `sources/Qt4Qt5/` and `src/ragoshell-app/ScintillaQuick*`. Furthermore, 96 of 97 upstream language lexers are excluded at compile time (leaving only `LexCPP.cpp`), dynamic lexer loading (`SCI_LOADLEXERLIBRARY`) is never called, zero network/filesystem/HTML capabilities exist, and five layered truncation/lexing ceilings bound untrusted MongoDB BSON-to-JSON payloads. Upgrading to upstream QScintilla `2.14.x` (desktop `QWidget`-centric, Scintilla 5 / Lexilla split) would break `ScintillaQuickBackend` and custom rendering/IME patches while delivering zero security reduction.

2. **`googletest 1.8.1` (`src/third-party/googletest-1.8.1/`):**
   - **Decision (`ADR-E7-02`):** **Retain in-tree `1.8.1` strictly as a desktop test-only dependency** (`rago_unit_tests`) with `INSTALL_GTEST=OFF` and `BUILD_GMOCK=OFF` enforced in root `CMakeLists.txt`, and execute a **single-step migration to `vcpkg.json` (`gtest`)** when a C++20/23 standard bump or newer compiler toolchain breaks `1.8.1`.
   - **Why:** `googletest` is never compiled on mobile (`NOT SYSTEM_MOBILE`), is never linked into `rago-core`, `ragoshell`, or `ragoshell-mcp`, and is completely absent from all user installation packages (`.exe`, `.msi`, `.zip`, `.dmg`, `.tar.gz`, `.AppImage`, `.apk`, `.aab`, and iOS bundles). It carries zero production runtime risk.

---

## 1. Ground-Truth Inventory

### 1.1 Overview of `src/third-party/`

| Component | Version | Source Directory | License (SPDX) | Linked Target(s) | Shipped in User Packages? | SBOM Relationship |
|---|---|---|---|---|---|---|
| **QScintilla** *(+ Scintilla core)* | `2.8.4` *(Scintilla `3.3.6`)* | `src/third-party/qscintilla-2.8.4` | `GPL-3.0-only AND LicenseRef-Scintilla` | `ragoshell` (`PRIVATE` static) | **Yes** — statically linked into `ragoshell` GUI binary only | `DEPENDS_ON` |
| **googletest** | `1.8.1` | `src/third-party/googletest-1.8.1` | `BSD-3-Clause` | `rago_unit_tests` (`PRIVATE` static, desktop only) | **No** — excluded from all shipped executables and installers | `TEST_DEPENDENCY_OF` |
| *QJson (cross-ref)* | *`0.8.1`* | *`src/third-party/qjson-0.8.1`* | *`LGPL-2.1-only`* | *`rago-core` (`PUBLIC` static)* | *Yes — statically linked (covered by LGPLv3 §4 notice in `THIRD-PARTY-NOTICES.txt`)* | *`DEPENDS_ON`* |

---

### 1.2 `QScintilla 2.8.4` (Embedding `Scintilla 3.3.6`)

- **Exact Version & Origin:** Riverbank Computing QScintilla `2.8.4` (released 11 September 2014; `src/third-party/qscintilla-2.8.4/sources/NEWS:1`), built on Scintilla `3.3.6` (`sources/NEWS:32`). Configured via `QSCINTILLA_VERSION` in root `CMakeLists.txt:99-100,106`.
- **License & Compliance:** Dual-licensed upstream; used in RagoShell under **GNU GPL v3.0** (`sources/LICENSE.GPL3`) alongside the permissive Scintilla copyright notice (`sources/OPENSOURCE-NOTICE.TXT`), matching RagoShell's own GPLv3 license (`LICENSE`). Both notice files are installed into every platform's `licenses/third-party/` bundle by `cmake/RagoShellLicenses.cmake:59-60` and verified fail-fast by `ci/generate-sbom.mjs:62-63,296` and `THIRD-PARTY-NOTICES.txt:146-157`.
- **Target Linkage Boundary:**
  - Built as static library `qscintilla` (`src/third-party/qscintilla-2.8.4/CMakeLists.txt:213-240`), linking `Qt6::Widgets`, `Qt6::PrintSupport`, and `Qt6::Core5Compat`.
  - Linked **only** (`PRIVATE qscintilla`) into the `ragoshell` GUI application (`src/ragoshell-app/CMakeLists.txt:234`).
  - **Not** linked into `rago-core` (`src/ragoshell/CMakeLists.txt`), the headless `ragoshell-mcp` server (`src/ragoshell-mcp/CMakeLists.txt`), or `rago_unit_tests` (`src/ragoshell-unit-tests/CMakeLists.txt`).
- **Stripped Compilation Surface:**
  - In `src/third-party/qscintilla-2.8.4/CMakeLists.txt:69-211`, **96 out of 97** upstream Scintilla language lexers (`LexHTML.cpp`, `LexXML.cpp`, `LexSQL.cpp`, `LexRuby.cpp`, `LexCaml.cpp`, `LexLua.cpp`, `LexPerl.cpp`, `LexPython.cpp`, `LexYAML.cpp`, etc.) and **35** `qscilexer*.cpp` wrapper classes are commented out and never compiled.
  - Likewise, in `src/third-party/qscintilla-2.8.4/sources/src/Catalogue.cpp:57-187`, **106 out of 107** `LINK_LEXER(...)` registrations are commented out; **only `LINK_LEXER(lmCPP)` (`sources/lexers/LexCPP.cpp`, `SCLEX_CPP`)** is compiled and registered in the binary.
- **In-Tree Local Patches (Qt 6.12, macOS, HiDPI, Headless QML & DoS Hardening):**
  1. **Qt 6 CMake & Core API Port** (`CMakeLists.txt:13,236-240`, `InputMethod.cpp:114`, `PlatQt.cpp:41-42,137,150,414,438,675,682,691`, `qsciapis.cpp:600`, `qscicommandset.cpp:899,938`, `qscilexer.cpp:384,461,470,580,618,625`, `qscimacro.cpp:195,217,220`, `qsciprinter.cpp:104`, `qsciscintillabase.cpp:604`):
     - Migrated `qt5_wrap_cpp` to `qt6_wrap_cpp` and Qt5 targets (`Qt5::Widgets`, `Qt5::PrintSupport`, `Qt5::MacExtras`) to `Qt6::Widgets`, `Qt6::PrintSupport`, and `Qt6::Core5Compat`.
     - Replaced removed Qt 5 symbols (`QDesktopWidget` → `QGuiApplication::screenAt`, `QPainter::drawRoundRect` → `drawRoundedRect`, `QFontMetricsF::width` → `horizontalAdvance`, `Qt::ImMicroFocus` → `Qt::ImCursorRectangle`, `Qt::MidButton` → `Qt::MiddleButton`, `QString::sprintf` → `QString::asprintf`, `QPrinter::numCopies` → `copyCount`, `qLowerBound` → `std::lower_bound`).
  2. **macOS SDK & Qt 6.12 Removals** (`MacPasteboardMime.cpp:30`, `PlatQt.cpp:121-123`, `qsciscintillabase.cpp:87,122`):
     - Guarded out Qt 5-only `QMacPasteboardMime` rectangular-selection pasteboard registration and `QFont::ForceIntegerMetrics` under `QT_VERSION >= 0x060000`.
  3. **Headless Qt Quick (`WindowID wid == 0`) Null-Pointer Guards** (`PlatQt.cpp:296,480,488,500,508,517,524`):
     - Added null-checks on `PWindow(wid)` across `SurfaceImpl::InitPixMap` and `Window::HasFocus`, `Show`, `SetFont`, `SetCursor`, `SetTitle`, and `GetMonitorRect` so running `ScintillaBase` without a visible `QWidget` parent never dereferences a null widget pointer.
  4. **Logical-Pixel Font Sizing & Fractional HiDPI Scaling** (`PlatQt.cpp:109-148,292-304`):
     - Replaced `QFont::setPointSizeF` with `QFont::setPixelSize(qMax(6, qRound(fp.size)))` and `QFont::PreferVerticalHinting`, added `SC_EFF_QUALITY_LCD_OPTIMIZED`, and switched `InitPixMap` to `devicePixelRatioF()` with `qCeil` dimensions so Scintilla glyph heights match QML `font.pixelSize` identically across 100%–300% desktop and mobile HiDPI displays without double-scaling.
  5. **Bounded UTF-8 HarfBuzz Shaping Chunks** (`PlatQt.cpp:305-435`):
     - Rewrote `SurfaceImpl::MeasureWidths` to segment lines at UTF-8 code-point boundaries (`(byte & 0xC0) != 0x80`) into chunks of at most `kMaxChunkBytes = 1024` bytes before invoking `QTextLayout`, eliminating $O(N^2)$ HarfBuzz shaping stalls on multi-megabyte single-line JSON or Base64 strings.
- **`ScintillaQuick` QML Integration Surface:**
  - `RagoCodeEditor.qml` (`src/ragoshell-app/qml/RagoCodeEditor.qml`) wraps `ScintillaQuickItem` (`QML_NAMED_ELEMENT(ScintillaEditor)` in `src/ragoshell-app/ScintillaQuick.h:35`), which owns `ScintillaQuickBackend : public ScintillaBase` (`src/ragoshell-app/ScintillaQuickBackend.h:59`).
  - Uses a hidden off-screen `QWidget` (`Qt::WA_DontShowOnScreen`) solely as `wMain` metric anchor, disables Scintilla's internal `QPixmap` line buffer (`SCI_SETBUFFEREDDRAW, 0`), and paints directly into a native-DPR opaque `QImage::Format_RGB32` backing store (`ScintillaQuick.cpp:248-302`) to preserve ClearType/LCD subpixel antialiasing inside Qt Quick's scene graph.
  - Marshals `NotifyParent` and repaint callbacks across Qt Quick's render thread (`QSGRenderThread`) via `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` (`ScintillaQuickBackend.h:117-148`).
  - Implements custom UTF-8 byte-offset CJK Input Method (`QInputMethodEvent`) inline preedit composition in `ScintillaQuickInput.cpp:283-494` (bypassing QScintilla 2.8.4's character-indexed `InputMethod.cpp`) and mobile touch long-press context menus (`RagoCodeEditor.qml:232-242`).
- **SBOM & Vulnerability Scanner Coordinates (`ci/generate-sbom.mjs:868-880`):**
  - SPDX ID: `SPDXRef-Package-embedded-qscintilla`
  - Version: `2.8.4` (dynamically extracted from root `CMakeLists.txt` `QSCINTILLA_VERSION` and verified against `src/third-party/qscintilla-2.8.4/sources/LICENSE.GPL3`)
  - Package URL (`purl`): `pkg:generic/qscintilla@2.8.4`
  - CPE 2.3 (`cpe23Type`): `cpe:2.3:a:riverbankcomputing:qscintilla:2.8.4:*:*:*:*:*:*:*`

---

### 1.3 `googletest 1.8.1`

- **Exact Version & Origin:** Google Test `1.8.1` (`release-1.8.1`, released 31 August 2018), vendored at `src/third-party/googletest-1.8.1/`. Not listed in `vcpkg.json` (`vcpkg.json:1-37`).
  - *Metadata note:* Upstream `google/googletest@release-1.8.1` shipped with `set(GOOGLETEST_VERSION 1.9.0)` in `src/third-party/googletest-1.8.1/CMakeLists.txt:8`; root `CMakeLists.txt:101-103` explicitly pins `set(GOOGLE_TEST_VERSION 1.8.1)` so SBOM and build metadata record the true `1.8.1` release version.
- **License & Compliance:** `BSD-3-Clause` (`src/third-party/googletest-1.8.1/LICENSE`), copied to `licenses/third-party/googletest-LICENSE.txt` (`cmake/RagoShellLicenses.cmake:62`) and documented in `THIRD-PARTY-NOTICES.txt:382-390`.
- **In-Tree Local Patch:**
  - `src/third-party/googletest-1.8.1/googletest/cmake/internal_utils.cmake:99-103`: Removed `-Werror` from `cxx_base_flags` under `CMAKE_COMPILER_IS_GNUCXX` because GCC 13+ emits `-Wmaybe-uninitialized` on 2018-era `gtest-death-test.cc`.
- **Build Scope & Zero-Distribution Proof:**
  - **Mobile exclusion:** Root `CMakeLists.txt:108-112` wraps `add_subdirectory(${GOOGLE_TEST_DIR})` inside `if(NOT SYSTEM_MOBILE)`, so `googletest` is never configured or compiled for Android or iOS.
  - **Install & mock exclusion:** Root `CMakeLists.txt:109-110` forces `INSTALL_GTEST=OFF` and `BUILD_GMOCK=OFF` before `add_subdirectory(${GOOGLE_TEST_DIR})`. Consequently, `cmake --install` emits **zero** `include/gtest/`, `include/gmock/`, `libgtest*.a`, `libgmock*.a`, `lib/cmake/GTest/`, or `lib/pkgconfig/*.pc` files.
  - **Executable linkage:** Linked **exclusively** (`gtest`, `gtest_main`) into `rago_unit_tests` (`src/ragoshell-unit-tests/CMakeLists.txt:68-72`), which has no `install(...)` rule. Neither `rago-core`, `ragoshell`, nor `ragoshell-mcp` links or includes any `googletest` symbol.
  - **Verified absence across all user packages:**
    - Windows `.exe` / `.msi` / `.zip` (`ci/build-windows.ps1`): stages only `ragoshell.exe`, `ragoshell-mcp.exe`, runtime Qt/vcpkg DLLs, and `--component licenses`.
    - macOS `.dmg` (`ci/build-macos.sh`): stages `RagoShell.app` (containing `RagoShell` and `ragoshell-mcp`) and `--component licenses`.
    - Linux `.tar.gz` & `.AppImage` (`ci/build-linux.sh:334,348,375`): `cmake --install` installs only `ragoshell`, `ragoshell-mcp`, desktop/icon/metainfo files, and `licenses/` (with `INSTALL_GTEST=OFF` preventing static archives or headers from entering `AppDir/usr`).
    - Android `.apk` / `.aab` (`ci/build-android.sh`) & iOS bundles: `SYSTEM_MOBILE` is `TRUE`; `googletest` is not even added to the CMake build graph.
- **SBOM & Vulnerability Scanner Coordinates (`ci/generate-sbom.mjs:908-919`):**
  - SPDX ID: `SPDXRef-Package-embedded-googletest`
  - Version: `1.8.1`
  - Package URL (`purl`): `pkg:github/google/googletest@release-1.8.1`
  - CPE 2.3 (`cpe23Type`): `cpe:2.3:a:google:googletest:1.8.1:*:*:*:*:*:*:*`
  - Relationship: `TEST_DEPENDENCY_OF` (distinguishing build/test verification tooling from shipped runtime dependencies).

---

## 2. Threat, Attack Surface & CVE Assessment

### 2.1 `QScintilla 2.8.4` / `Scintilla 3.3.6`

#### Known CVE & OSV Status
- **Riverbank QScintilla (`cpe:2.3:a:riverbankcomputing:qscintilla:2.8.4`):** Zero published NVD CVEs or OSV advisories. Continuous automated scanning runs in CI on every push/PR via **OSV-Scanner v2.7.0** against the generated SPDX 2.3 SBOM (`.github/workflows/ci.yml:80-101`).
- **Embedded Scintilla `3.3.6` Core:** Historical advisories in the broader Scintilla ecosystem fall into two categories, **neither of which is reachable in RagoShell**:
  1. *Language-specific lexer buffer bugs* (e.g., historical `CVE-2007-2666` in Scintilla ≤ 1.73 `LexRuby.cxx` or `CVE-2019-16294` in `SciLexer` Caml `.ml` handling): `LexRuby.cpp`, `LexCaml.cpp`, and 94 other lexers are **excluded from compilation** (`src/third-party/qscintilla-2.8.4/CMakeLists.txt:69-167`, `Catalogue.cpp:57-187`). Only `LexCPP.cpp` (`SCLEX_CPP`) is compiled.
  2. *External lexer plugin / DLL loading (`ExternalLexer.cpp`, `SCI_LOADLEXERLIBRARY = 4007`)*: `RagoShell` never calls `SCI_LOADLEXERLIBRARY` or `LoadLexerLibrary` anywhere in the codebase; `ScintillaQuick.cpp:365` hardcodes `send(SCI_SETLEXER, _plainText ? SCLEX_NULL : SCLEX_CPP)`.

#### Capability & Trust-Boundary Analysis
- **No Network, Process, or Filesystem I/O:** `qscintilla` opens zero sockets, spawns zero subprocesses, and performs zero direct file reads or writes. All script/JSON file reading and writing happens in Qt (`QFile`) prior to passing UTF-8 `QString` buffers to `ScintillaQuickItem::setText()`.
- **No HTML, XML, Rich-Text, or Binary Parsers:** Scintilla operates strictly on flat in-memory UTF-8 byte buffers (`SplitVector<char>` in `CellBuffer.cpp`) and style run arrays; no DOM, CSS, image decoder, or binary object parser exists in the compiled target.
- **No Untrusted Regex Evaluation:** `ScintillaQuickItem::findText` (`src/ragoshell-app/ScintillaQuick.cpp:630-668`) passes only `SCFIND_MATCHCASE` or `0` — never `SCFIND_REGEXP` or `SCFIND_POSIX` — keeping Scintilla's legacy `RESearch.cpp` regex engine completely unexposed.

#### Untrusted MongoDB Document Payloads & Memory/DoS Bounds
The primary untrusted input reaching `ScintillaQuick` is **Extended JSON (EJSON) text** serialized by `bsoncxx` from BSON documents returned by a remote MongoDB deployment (e.g., deeply nested documents, huge arrays, or multi-megabyte single-line strings). RagoShell enforces **five layered resource ceilings** before and inside Scintilla so hostile or pathological documents cannot exhaust memory or freeze the GUI thread:

| Layer | Location | Constant / Mechanism | Protection Provided |
|---|---|---|---|
| **1. Query Batch Ceiling** | `src/ragoshell/core/settings/SettingsManager.h:159` | `kMaxBatchSize = 1000` *(default `_batchSize = 50`)* | Bounds the number of BSON documents fetched into a single result tab page. |
| **2. Multi-Document JSON Truncation** | `src/ragoshell-app/ShellControllerResults.cpp:764-795` | `kViewerFieldPreviewBytes = 8192` (8 KiB/field), `kMaxTotalViewerChars = 2 * 1024 * 1024` (2 MiB total), lazy `_textResultDirty` | Truncates oversized string/binary fields and caps total EJSON viewer text at 2 MiB; skips serialization completely while Tree or Table view is active. |
| **3. Single-Document View Cap** | `src/ragoshell-app/ShellControllerResults.cpp:79` | `kViewFieldPreviewBytes = 16384` (16 KiB/field) | Bounds individual field expansion in read-only document inspection dialogs. |
| **4. Editor Large-Document Mode** | `src/ragoshell-app/ScintillaQuick.cpp:162-190,338,399` | `kLargeDocThresholdBytes = 128 * 1024` (128 KiB), `kInitialLexViewportBytes = 32 * 1024` (32 KiB) | Above 128 KiB, disables `SCI_SETSCROLLWIDTHTRACKING`, brace matching, and code folding (`fold = 0`), and restricts synchronous `SCI_COLOURISE` lexing to the initial 32 KiB viewport. |
| **5. Bounded UTF-8 Text Shaping** | `src/third-party/qscintilla-2.8.4/sources/Qt4Qt5/PlatQt.cpp:319` | `kMaxChunkBytes = 1024` (UTF-8 code-point aligned) | Prevents $O(N^2)$ HarfBuzz / `QTextLayout` stalls when rendering long unwrapped single-line payloads. |

---

### 2.2 `googletest 1.8.1`

- **Production Attack Surface:** **Zero.** `googletest 1.8.1` is neither linked into any production executable (`ragoshell`, `ragoshell-mcp`) nor included in any user installer or archive. No CVE or OSV advisory applies to `1.8.1`.
- **Toolchain & Maintenance Friction:**
  1. *Compiler diagnostics on 2018 C++11 idioms:* GCC 13+ already required patching `internal_utils.cmake` to drop `-Werror` due to `-Wmaybe-uninitialized` in `gtest-death-test.cc`. Newer Clang (19+) and MSVC toolsets under C++20/C++23 increasingly warn on legacy iterator/tuple traits and `gtest-linked_ptr.h`.
  2. *Legacy CMake minimum policy:* `src/third-party/googletest-1.8.1/CMakeLists.txt:1` declares `cmake_minimum_required(VERSION 2.6.4)`, which currently relies on `CMAKE_POLICY_VERSION_MINIMUM: "3.5"` in `CMakePresets.json:14` under CMake 3.30+/4.x.

---

## 3. Options Analysis & Architecture Decision Record (ADR)

### 3.1 `ADR-E7-01` — `QScintilla` Strategy

#### Options Considered

| Option | Summary | Pros | Cons / Risks |
|---|---|---|---|
| **Option A1 (Selected): Keep in-tree patched `QScintilla 2.8.4`** | Retain `src/third-party/qscintilla-2.8.4` with stripped single-lexer build, custom `PlatQt`/`ScintillaQuick` patches, SBOM/OSV tracking, and payload caps. | Zero regression risk to `ScintillaQuickBackend` (`ScintillaBase`), HiDPI RGB32 ClearType rendering, CJK UTF-8 IME, macOS Qt 6.12, iOS/Android touch, and easy `#ifndef Q_OS_WASM` gating of `PrintSupport` for Phase 8.1. | Retains 2014 Scintilla `3.3.6` core and keeps `Qt6::Widgets` + `Qt6::Core5Compat` linked into `ragoshell`. |
| **Option A2: Upgrade in-tree to upstream `QScintilla 2.14.1`** | Vendor latest GPLv3 QScintilla 2.14.x (based on Scintilla 5 / Lexilla) and re-apply local patches. | Newer upstream Scintilla/Qt6 widget baseline; drops `Qt6::Core5Compat`. | **High breakage for zero security gain:** QScintilla 2.12+ restructured internal headers around Scintilla 5 / Lexilla and targets desktop `QWidget` (`QsciScintilla`). Re-porting `ScintillaQuickBackend : public ScintillaBase`, pixel-size `PlatQt`, `kMaxChunkBytes` UTF-8 shaping, and CJK IME carries high multi-platform regression risk without reducing any active attack surface. |
| **Option A3: Replace with native Qt Quick editor (`TextEdit` + `QSyntaxHighlighter`) or standalone Scintilla 5 / Lexilla** | Remove QScintilla completely; either build a pure QML code editor or wrap minimal widget-free Scintilla 5 + `Lexilla` (`LexCPP`). | Drops `Qt6::Widgets`, `Qt6::PrintSupport`, and `Qt6::Core5Compat` from `ragoshell` (smaller mobile and WebAssembly binaries). | Multi-week UI engineering effort (code folding margins, multi-caret/column selection, calltips, autocomplete popups, 128 KiB+ virtualized viewport performance) outside Phase 0–3 scope. |

#### Decision (`ADR-E7-01`)
**Adopt Option A1:** Keep the in-tree patched `QScintilla 2.8.4` for the short and medium term.

#### Mandatory Compensating Controls
1. **SBOM & Continuous Vulnerability Scanning:** Every CI run and release generates deterministic SPDX 2.3 SBOM entries for `qscintilla@2.8.4` (`cpe:2.3:a:riverbankcomputing:qscintilla:2.8.4:*:*:*:*:*:*:*`) and scans them with `OSV-Scanner v2.7.0` (`.github/workflows/ci.yml:80-101`).
2. **Minimal Compiled Surface:** Keep all lexers except `LexCPP.cpp` excluded in `src/third-party/qscintilla-2.8.4/CMakeLists.txt` and `Catalogue.cpp`. Never invoke `SCI_LOADLEXERLIBRARY` or pass untrusted patterns with `SCFIND_REGEXP`.
3. **Strict Resource Bounds:** Preserve the five truncation, batch-size, viewport-colourise (`32 KiB`), large-document (`128 KiB`), and HarfBuzz chunking (`1024 B`) ceilings documented in §2.1.
4. **Platform Binary Hardening:** Ship release binaries with macOS Hardened Runtime (`ci/build-macos.sh`), Windows MSVC `/DYNAMICBASE` + `/NXCOMPAT`, Linux PIE/RELRO (`ci/build-linux.sh`), and Android NDK 16 KB ELF alignment (`ci/build-android.sh`).

#### Concrete Upgrade / Replacement Triggers
Re-open `ADR-E7-01` and execute **Option A3** (preferred long-term architecture: widget-free Qt Quick editor or standalone widgetless Scintilla 5 + Lexilla) when **any** of the following triggers occurs:
1. **Qt 6.x or WebAssembly Target Breakage:** A future Qt 6 release removes `Qt6::Core5Compat` APIs used by `qscintilla`, or Phase 8.1 (`docs/WebAssembly.md`) demonstrates unacceptable WASM bundle overhead from linking `Qt6::Widgets`.
2. **Sanitizer / Fuzzing / Advisory Finding:** AddressSanitizer (`-fsanitize=address`), UndefinedBehaviorSanitizer (`-fsanitize=undefined`), or a new Scintilla security advisory reveals a memory-safety defect in `Scintilla 3.3.6` core buffer handling (`CellBuffer`, `Document`, `Editor`) or `LexCPP.cpp`.
3. **Editor Feature Ceiling:** Product requirements demand native tree-sitter / MongoDB aggregation AST highlighting or mobile native text-selection handles that `QQuickPaintedItem` + `ScintillaBase` cannot provide cleanly.

---

### 3.2 `ADR-E7-02` — `googletest` Strategy

#### Options Considered

| Option | Summary | Pros | Cons / Risks |
|---|---|---|---|
| **Option B1 (Selected for current baseline): Retain in-tree `googletest 1.8.1` (`INSTALL_GTEST=OFF`, `BUILD_GMOCK=OFF`)** | Keep `src/third-party/googletest-1.8.1` strictly gated to `NOT SYSTEM_MOBILE` and `rago_unit_tests`, with install rules and unused `gmock` disabled. | Zero production footprint; zero network/vcpkg build overhead on mobile presets; all 298 unit tests compile and pass today across Windows, macOS, and Linux. | Requires `CMAKE_POLICY_VERSION_MINIMUM=3.5` (also needed by `qjson-0.8.1`) and will eventually hit C++20/23 compiler deprecations. |
| **Option B2 (Planned on trigger): Migrate to `vcpkg.json` `gtest` port** | Remove `src/third-party/googletest-1.8.1/` and consume modern `GTest::gtest` / `GTest::gtest_main` from `vcpkg.json` (`"platform": "!android & !ios"`). | Upstream C++20/23 support, modern CMake `3.16+` config targets, automatic vcpkg SHA-512 & SPDX tracking. | Touches desktop CI vcpkg binary caches across Windows, macOS, and Linux simultaneously; unnecessary churn until C++ standard or CMake policy changes. |

#### Decision (`ADR-E7-02`)
**Adopt Option B1 now** (with `INSTALL_GTEST=OFF` and `BUILD_GMOCK=OFF` enforced in root `CMakeLists.txt:109-110` so `cmake --install` never emits test archives or headers), and execute **Option B2** as a single mechanical commit as soon as any of the following occurs:
- RagoShell raises `CMAKE_CXX_STANDARD` from `17` (`cmake/RagoShellCMakeDefaults.cmake:42`) to `20` or `23`;
- A newer Clang, AppleClang, GCC, or MSVC release in CI rejects `googletest 1.8.1` headers; or
- `qjson-0.8.1` CMake minimum version is modernized, allowing `CMAKE_POLICY_VERSION_MINIMUM=3.5` to be dropped from `CMakePresets.json`.

#### Single-Step `vcpkg` Migration Checklist (Ready to Execute on Trigger)
1. **`vcpkg.json`:** Add `{ "name": "gtest", "platform": "!android & !ios" }` under `"dependencies"` (and optionally pin exact `version` in `"overrides"` alongside the repository `builtin-baseline`) so Android and iOS builds skip compiling `gtest`.
2. **Root `CMakeLists.txt`:** Remove `GOOGLE_TEST_VERSION` / `GOOGLE_TEST_DIR` / `add_subdirectory(${GOOGLE_TEST_DIR})`, add `find_package(GTest CONFIG REQUIRED)` inside `if(NOT SYSTEM_MOBILE)`, and delete `src/third-party/googletest-1.8.1/`.
3. **`src/ragoshell-unit-tests/CMakeLists.txt`:** Replace `gtest` and `gtest_main` in `target_link_libraries(rago_unit_tests PRIVATE ...)` with `GTest::gtest` and `GTest::gtest_main`.
4. **Licenses & SBOM (`cmake/RagoShellLicenses.cmake`, `THIRD-PARTY-NOTICES.txt`, `ci/generate-sbom.mjs`, `ci/test-sbom-and-licenses.mjs`):** Remove `googletest-1.8.1` from `EMBEDDED_THIRD_PARTY_LICENSE_MAP` and `embeddedSpecs`, and register `gtest` in `VCPKG_PORT_CATALOG` with `relationship: 'TEST_DEPENDENCY_OF'` so vcpkg's installed `share/gtest/copyright` and `vcpkg.spdx.json` are verified automatically.
