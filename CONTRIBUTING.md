# Contributing to RagoShell

Thank you for your interest in contributing to **RagoShell** — a cross-platform, shell-centric MongoDB management tool built with C++17, Qt 6 Quick (QML), and the official MongoDB C/C++ drivers.

Before opening an issue or pull request, please review:

- [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) — respectful, constructive technical collaboration standards.
- [`SECURITY.md`](SECURITY.md) — **never** report security vulnerabilities in public issues or pull requests.
- [`README.md`](README.md) and [`docs/Platforms.md`](docs/Platforms.md) — architecture overview, supported platforms, and current open items.

---

## Repository Topology

- **Official repository & CI:** `https://github.com/ragipunal/ragoshell` (default branch: `main`)

Key directories:

| Path | Purpose |
|---|---|
| `src/ragoshell/` | `rago-core` — GUI-free C++17 core library (`mongocxx`/`libbson`, `QJSEngine` shell runtime in `core/engine/`, `KeyStore`, `UpdateService`, SSH tunnel) |
| `src/ragoshell-app/` | Qt 6 Quick (QML) desktop & mobile UI, QScintilla shell editor bridge, and 15-language `.ts` translation catalogs (`i18n/`) |
| `src/ragoshell-mcp/` | `ragoshell-mcp` — optional desktop stdio Model Context Protocol server (read-only, explicit connection allowlist; [`docs/McpServer.md`](docs/McpServer.md)) |
| `src/ragoshell-unit-tests/` | `rago_unit_tests` — desktop GoogleTest suite |
| `cmake/` | CMake modules, including the single-source brand definition (`cmake/RagoShellDefaults.cmake`) and license bundler (`cmake/RagoShellLicenses.cmake`) |
| `ci/` | Cross-platform build/packaging scripts, i18n checker (`check-i18n.mjs`), SPDX 2.3 SBOM generator (`generate-sbom.mjs`), and release manifest tools |
| `docs/` | Architecture, design system, platform, localization, shell, and signing documentation |

---

## Build Prerequisites & CMake Presets

### Prerequisites

- **CMake 3.25+** (`CMakePresets.json` v6; top-level `CMakeLists.txt` floor is 3.20) and **Ninja** (or Visual Studio 2022 / 2026 on Windows)
- **C++17 compiler:** MSVC on Windows, GCC or Clang on Linux, Apple Clang (Xcode Command Line Tools) on macOS
- **[vcpkg](https://vcpkg.io):** Manifest dependencies (`mongo-cxx-driver`, `mongo-c-driver`, `openssl`, `libssh2`) are declared in [`vcpkg.json`](vcpkg.json). Set `VCPKG_ROOT` on Linux, macOS, and Android (Windows presets locate Visual Studio's bundled `vcpkg` or `$env:VCPKG_ROOT`).
- **Qt 6 (tested with Qt 6.12.x):** Community / Open Source Edition with `Core`, `Gui`, `Widgets`, `PrintSupport`, `Network`, `Xml`, `Core5Compat`, `Qml`, `Quick`, `QuickControls2`, and `LinguistTools`. Set `Qt6_DIR` on Linux and macOS (`Qt6_DIR_IOS` / `Qt6_DIR_ANDROID` when cross-compiling mobile targets).
- **Node.js (20/22+, build host only):** Used only for `node ci/check-i18n.mjs`, SBOM/release scripts, and MCP/server tests — Node.js is never bundled into RagoShell packages.
- **Python 3 (optional):** Used for `ci/fill-english-plurals.py` and `ci/pseudo-translate.py`.

For step-by-step host setup, see [`docs/BuildRagoShellOnWindows.md`](docs/BuildRagoShellOnWindows.md), [`docs/BuildRagoShellOnMacAndLinux.md`](docs/BuildRagoShellOnMacAndLinux.md), [`docs/Platforms.md`](docs/Platforms.md), and [`ci/README.md`](ci/README.md).

### CMake Presets (`CMakePresets.json`)

| Target Platform | Configure Preset(s) | Build Preset(s) | Packaging / CI Script |
|---|---|---|---|
| **Windows 10/11 (x64)** | `windows`, `windows-ninja` | `windows-debug`, `windows-release` | `.\ci\build-windows.ps1` |
| **macOS 14.4+ (arm64)** | `macos-debug`, `macos-release` | `macos-debug`, `macos-release` | `./ci/build-macos.sh` |
| **Linux (x64)** | `linux-debug`, `linux-release` | `linux-debug`, `linux-release` | `./ci/build-linux.sh` |
| **iOS / iPadOS 18.0+ (arm64)** | `ios` (macOS + Xcode) | `ios-debug`, `ios-release` | `dev/mac/build-ios.sh` |
| **Android 9.0+ / API 28–37 (arm64 & emulator)** | `android-arm64`, `android-arm64-release` (Win), `android-arm64-macos`, `android-arm64-macos-release` (macOS), `android-arm64-linux` (Linux), `android-x86_64` (emulator) | `android-arm64-debug`, `android-arm64-release`, `android-arm64-macos-debug`, `android-arm64-macos-release`, `android-arm64-linux`, `android-x86_64-debug` | `./ci/build-android.sh`, `.\ci\build-android.ps1` |

Quick desktop build example:

```bash
# Linux or macOS (after exporting VCPKG_ROOT and Qt6_DIR)
cmake --preset macos-debug          # or linux-debug
cmake --build --preset macos-debug

# Windows (PowerShell / Developer Command Prompt)
cmake --preset windows
cmake --build --preset windows-debug
```

### Live MongoDB Tests (Optional)

Unit tests run offline out of the box; live integration tests (`ScriptEngineLiveTest`, etc.) connect to `localhost:27017` (`samples` database) and **skip cleanly** when no server is running. Because RagoShell supports **MongoDB 4.0+**, do not rely on 4.2+ server features (such as `$$NOW` or `$set` in aggregation pipelines) in shared tests without explicit version guards.

---

## Rules of the House

These rules come directly from [`docs/DesignSystem.md`](docs/DesignSystem.md), [`docs/Shell.md`](docs/Shell.md), and [`docs/Brand.md`](docs/Brand.md). Following them prevents regressions across desktop, tablet, phone, and RTL layouts:

1. **Design doc before multi-phase or security-sensitive changes:** If a change spans multiple steps or touches authentication, credentials (`KeyStore`), cryptography, update signing, network endpoints, or sandboxing, write or update `docs/<Topic>.md` (covering architecture, threats/edge cases, and test plan) **before** writing code.
2. **Qt Quick (QML) layout discipline (`docs/DesignSystem.md`):**
   - **Never hardcode pixel sizes in screens:** read spacing, font sizes, and control heights (`controlHeight`, `fieldHeight`, `iconButtonSize`, `menuItemHeight`) from `Theme` / `UiDensity`.
   - **Responsive breakpoints use page width, not window width:** forms stack into a **single column below 520 dp**.
   - **Overflow guards:** a non-fill item in `RowLayout`/`ColumnLayout` does not shrink on its own; wrap toolbars in `HScrollRow`; set `Layout.minimumWidth: 0` on every child of a `StackLayout`.
   - **Keyboard shortcuts:** format all shortcut labels exclusively through `Platform` (`keyLabel`, `withShortcut`, `macKeys`, `RagoMenuItem.keys`) so macOS/iPadOS render `⌘` and touch devices without a hardware keyboard hide shortcut hints cleanly.
   - **One shared QML tree:** never duplicate QML files per OS. Use `#if` blocks in C++ and CMake platform flags (`SYSTEM_IOS`, `SYSTEM_ANDROID`, `SYSTEM_MOBILE`).
3. **Embedded JavaScript shell (`docs/Shell.md`):**
   - The shell executes on Qt's `QJSEngine` (**ES2016-ish**), not V8/Node.js. In `src/ragoshell/core/engine/js/` and built-in shell scripts, **do not** use object spread (`{...obj}`), `async`/`await`, `BigInt`, or bare `catch {}` (`?.` and `??` work).
4. **Secrets, `KeyStore`, and network privacy:**
   - Passwords, SSH passphrases, AI provider API keys, and tokens must **always** go through `KeyStore` — never store secrets in `ragoshell.json` or plain text files, and never commit keys, tokens, certificates, or `.env` files.
   - Outbound network services belong in `rago-core` (`src/ragoshell/core/`) as unit-testable C++ classes exercised against a local fake server in `rago_unit_tests`; QML only binds to C++ state. Nothing phones home without an explicit user action or documented preference (`docs/UpdateAndTelemetry.md`).
   - Read environment variables via `productEnv("X")` (`RAGOSHELL_X`) and config filenames via `appFileName()`.
5. **Single-source brand rule (`docs/Brand.md`):**
   - Product name, IDs, bundle ID, and URLs are defined **once** in [`cmake/RagoShellDefaults.cmake`](cmake/RagoShellDefaults.cmake) (`RAGO_PRODUCT_NAME`, `RAGO_PRODUCT_ID`, `RAGO_BUNDLE_ID`, `RAGO_DOMAIN`, `RAGO_PRODUCT_URL`, `RAGO_DONATE_URL`) and exposed through generated `BrandInfo.h` and QML `Brand`.
   - Never hardcode the product name inside C++/QML code or translatable strings (use `qsTr("Welcome to %1").arg(Brand.productName)`). In documentation and comments, always write **RagoShell** (exact CamelCase with a capital `S` as a single word — never split into two words, lowercase the `S`, write in all-caps, or abbreviate; see [`docs/Brand.md`](docs/Brand.md)). Only historical attribution to the upstream `Robomongo` 1.4.4 codebase (`COPYRIGHT`) uses the legacy upstream name.
6. **Formatting, line endings & binary assets:**
   - Commit source files with **LF** line endings (except Windows `.bat` scripts per [`.gitattributes`](.gitattributes)).
   - Match the naming, indentation, and idiom of surrounding code; write comments in plain English explaining *why*, not *what*.
   - Do not commit screenshots or binary test dumps to the repository.

---

## Localization (i18n) Workflow

RagoShell ships in **15 languages** (`en`, `hi`, `zh_CN`, `es`, `pt_BR`, `ru`, `de`, `fr`, `ja`, `ar` RTL, `tr`, `az`, `kk`, `ky`, `uz`). Full rules live in [`docs/Localization.md`](docs/Localization.md) and [`src/ragoshell-app/i18n/GLOSSARY.md`](src/ragoshell-app/i18n/GLOSSARY.md).

- Wrap user-visible UI strings in `qsTr()` (QML) or `tr()` / `QCoreApplication::translate()` (C++).
- Never build sentences by concatenating strings onto `qsTr()`: use numbered placeholders (`%1`, `%2`) with `.arg()`, `%n` for pluralized counts, and `Language.isolate()` around hostnames, paths, database/collection names, or identifiers so Arabic (`RTL`) keeps bidirectional order intact.
- Never call locale-sensitive case conversion (`toLocaleUpperCase()`) on identifiers, operators, or keywords — use `QString::toUpper()` or JS `toUpperCase()` (in Turkish/Azerbaijani locales, `"index"` otherwise becomes `"İNDEX"`).
- Prefer keeping existing source wording when practical: modifying a source string marks it unfinished across all 15 `.ts` files.

Whenever you add or modify any `qsTr()` / `tr()` string:

```bash
# 1. Refresh all .ts catalogs from QML and C++ targets
cmake --build --preset <your-preset> --target update_translations

# 2. Populate English plural forms if any %n string changed
python3 ci/fill-english-plurals.py

# 3. Update translations in src/ragoshell-app/i18n/ragoshell_<lang>.ts (following GLOSSARY.md)

# 4. Run the mandatory i18n consistency & placeholder check
node ci/check-i18n.mjs
```

---

## Mandatory Pre-PR Gates

Every pull request must pass all applicable verification gates locally before submission (and in GitHub Actions CI once opened):

1. **Clean build:** The touched CMake preset builds with zero errors.
2. **Unit tests (`rago_unit_tests`):**
   ```bash
   ./build/<preset>/src/ragoshell-unit-tests/rago_unit_tests
   ```
3. **QML load & runtime check (`--check-qml`):**
   ```bash
   ./build/<preset>/src/ragoshell-app/ragoshell --check-qml
   # If you touched UI layouts, fonts, or translations, also verify RTL and CJK:
   ./build/<preset>/src/ragoshell-app/ragoshell --check-qml --lang ar
   ./build/<preset>/src/ragoshell-app/ragoshell --check-qml --lang ja
   ```
4. **Translation & placeholder gate (`check-i18n.mjs`):**
   ```bash
   node ci/check-i18n.mjs
   ```
   *(CI also fails if `update_translations` produces uncommitted diffs in `src/ragoshell-app/i18n/*.ts`.)*

Keep each pull request focused on a single logical change, update affected files under `docs/`, and fill out [`.github/PULL_REQUEST_TEMPLATE.md`](.github/PULL_REQUEST_TEMPLATE.md).

---

## Licensing & Contributor Agreement (GNU GPLv3)

RagoShell is free software distributed under the **GNU General Public License version 3 (`GPLv3`)** — see [`LICENSE`](LICENSE), [`LICENSE.LGPLv3`](LICENSE.LGPLv3), [`COPYRIGHT`](COPYRIGHT), and [`THIRD-PARTY-NOTICES.txt`](THIRD-PARTY-NOTICES.txt).

By submitting a pull request, patch, translation, or documentation contribution to this repository, you certify and agree that:

1. **Originality & right to contribute:** The contribution was created in whole or in part by you and you have the legal right to submit it (including any employer permission if applicable), or it is derived exclusively from open-source work whose license is fully compatible with **GNU GPLv3** and carries intact attribution notices.
2. **GPLv3 licensing:** Your contribution is provided under the terms of the **GNU General Public License v3.0** (`LICENSE`) to be distributed as part of RagoShell.
3. **No closed-source or incompatible third-party code:** Do **not** include proprietary code, NDA-covered material, unattributed snippets, or third-party dependencies under GPLv3-incompatible licenses. Adding any new third-party library or font requires prior maintainer discussion, GPLv3 compatibility verification, SPDX 2.3 SBOM registration (`ci/generate-sbom.mjs`), and complete notice packaging in `THIRD-PARTY-NOTICES.txt` and `cmake/RagoShellLicenses.cmake`.

---

*MongoDB is a registered trademark of MongoDB, Inc. RagoShell is not affiliated with or endorsed by MongoDB, Inc.*
