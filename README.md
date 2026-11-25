# RagoShell

RagoShell is an open-source, cross-platform MongoDB client for desktop and mobile.

## Highlights

- **Qt 6** (tested with 6.12.x) — migrated from Qt 5.12.
- **Qt Quick (QML) user interface** — the old Qt Widgets UI was rewritten in QML (`src/ragoshell-app/`) on top of a GUI-free core library (`rago-core`); the shell editor still renders through QScintilla.
- **Official MongoDB drivers** — `mongo-cxx-driver` / `libbson` via [vcpkg](https://vcpkg.io); the old embedded `mongo` shell codebase was removed.
- **Embedded JavaScript shell** — the shell runs inside the application on Qt's JavaScript engine (`src/ragoshell/core/engine/`: the runtime `js/shell-runtime.js`, the MongoDB host `ShellHost` on `mongocxx` and the BSON converter), the same on Windows, Linux, macOS, Android, iOS and iPadOS. There is no Node.js process and no Node.js in the packages; see `docs/Shell.md`.
- **CMake + CMakePresets** — replaces the old qmake/PowerShell build system.
- **Lighter startup and memory profile** — the QtWebEngine-based welcome tab was replaced with a lightweight network implementation, and the output log is bounded.
- **Optional read-only MCP server** — `ragoshell-mcp` lets AI clients (Claude Code, Claude Desktop) inspect MongoDB through saved connections you explicitly allow; off by default. See `docs/McpServer.md`.
- **Cross-platform CI & signed releases** — per-platform build/packaging scripts under `ci/` producing distributable installers and archives, SPDX 2.3 SBOMs, and Minisign-verified release manifests.

MongoDB 4.0 compatibility is preserved and tested against MongoDB 4.0.x.

## Supported platforms

| Platform | Architecture | Status |
|---|---|---|
| Windows 10/11 | x64 | Full support — built and tested; `ci/build-windows.ps1` builds NSIS `-setup.exe`, WiX `.msi`, and portable `.zip` packages, with optional `signtool` / Azure Trusted Signing support ready (`docs/WindowsCodeSigning.md`) |
| macOS 14.4+ | arm64 (Apple Silicon) | Full support (`min` macOS 14.4, latest `macosx` SDK) — `ci/build-macos.sh` builds `.dmg` and `.tar.gz` packages (ad-hoc signed by default; optional Developer ID + notarization workflow ready, `docs/MacOsCodeSigning.md`) |
| Linux | x64 | Full support — Wayland and X11; `ci/build-linux.sh` builds a self-contained `.AppImage` and a system-Qt `.tar.gz` (`docs/Platforms.md`) |
| iOS / iPadOS 18.0+ | arm64 | Runs on iPhone and iPad (universal `UIDeviceFamily` 1 & 2, `min` 18.0, latest `iphoneos` SDK) — touch layout, embedded shell, Keychain; no `mongodb+srv://` or GSSAPI; installed from Xcode, App Store decision open (`docs/Platforms.md`, `docs/MacOsWork.md`) |
| Android 9.0+ phones and tablets | arm64 (`x86_64` emulator) | Runs on phones, tablets, and emulator (`min` API 28 / Android 9.0, target API 37 / Android 17) — touch layout, embedded shell, Android Keystore; `ci/build-android.sh` and `ci/build-android.ps1` build 16 KB page-aligned Release `.apk` and `.aab` packages with optional upload-keystore signing (`docs/AndroidCodeSigning.md`, Play listing open); no `mongodb+srv://` or GSSAPI (`docs/Platforms.md`) |

## Download

Prebuilt packages and checksums are published on [GitHub Releases](https://github.com/ragipunal/ragoshell/releases) and [ragoshell.com.tr/download](https://ragoshell.com.tr/download/).

## Build

Prerequisites:

- CMake 3.20+ and Ninja
- A C++17 compiler (MSVC on Windows, GCC/Clang elsewhere)
- [vcpkg](https://vcpkg.io) — dependencies are declared in `vcpkg.json`
- Qt 6.12 with Qt Quick and Quick Controls 2
- Node.js is optional: only the translation check (`node ci/check-i18n.mjs`) and the MCP end-to-end test use it

```bash
# Windows (uses presets that locate Qt under C:\Qt)
cmake --preset windows
cmake --build --preset windows-debug      # or windows-release

# Linux / macOS (set VCPKG_ROOT and Qt6_DIR first)
cmake --preset linux-debug                # or macos-debug
cmake --build --preset linux-debug
```

The app is `ragoshell` (`build/<preset>/src/ragoshell-app/`); the MCP server `ragoshell-mcp` and the unit tests (`rago_unit_tests`) build alongside it.

See `docs/BuildRagoShellOnWindows.md`, `docs/BuildRagoShellOnMacAndLinux.md` and `ci/README.md` for details, and `docs/RemoteBuild.md` for building the macOS and Linux packages from a Windows PC.

### More documents

`docs/DesignSystem.md` (how the interface is built and its rules), `docs/UiMatrix.md` (how every screen was checked), `docs/Platforms.md` (what runs where), `docs/Shell.md` (the embedded shell), `docs/MacOsWork.md` (what needs a Mac), `docs/TodoMac.md` (the open work, for a session on the Mac), `docs/McpServer.md`, `docs/ManualTests.md`, `docs/Brand.md` (how the name RagoShell is written and used).

### Local CI / packaging

```powershell
# Windows / Android (Windows host)
.\ci\build-windows.ps1 -Version 2.1.0
.\ci\build-android.ps1 -Version 2.1.0
```

```bash
# Linux / macOS / Android (Linux or macOS host)
./ci/build-linux.sh 2.1.0
./ci/build-macos.sh 2.1.0
./ci/build-android.sh 2.1.0
```

Packages land in `dist/` (git-ignored). Release manifest generation, SPDX 2.3 SBOM creation, Minisign signing, and automated publishing are documented in `ci/README.md` and `docs/ReleasePipeline.md`.

### Test data

The live unit tests and the screen checks expect a MongoDB 4.0 server on `localhost:27017` with a `samples` database; the tests skip themselves when no server answers. The author keeps a docker setup for it in `dev/mongo-test/`, which is git-ignored and therefore not part of the repository; any `mongo:4.0` container with sample data does the same job. How the screens are checked against it: `dev/ui-shots/README.md`.

## Translating RagoShell

The UI is available in 15 languages (English, हिन्दी, 简体中文, Español, Português (Brasil), Русский, Deutsch, Français, 日本語, العربية, Türkçe, Azərbaycan dili, Қазақ тілі, Кыргызча, Oʻzbekcha); Arabic is laid out right to left. Pick one in Preferences, or start with `--lang <code>` (for example `ragoshell --lang ja`). How the translations are made and checked is in `docs/Localization.md`.

The translations are machine-drafted from the glossary and checked mechanically; they still need a review by native speakers, and corrections are welcome.

```bash
# refresh the .ts files after changing UI text (strings in QML: qsTr, in C++: tr)
cmake --build build/windows --config Release --target update_translations
# translate with Qt Linguist (src/ragoshell-app/i18n/ragoshell_<lang>.ts), follow i18n/GLOSSARY.md, then:
node ci/check-i18n.mjs
# see the result without waiting for a translator: a pseudo-locale shows untranslatable text
python ci/pseudo-translate.py src/ragoshell-app/i18n/ragoshell_de.ts pseudo.ts
lrelease pseudo.ts -qm pseudo.qm && ragoshell --qm-file pseudo.qm
```

Adding a language takes one row in `src/ragoshell/core/utils/LanguageCatalog.cpp`, its code in `RAGO_UI_LANGUAGES` in `src/ragoshell-app/CMakeLists.txt` (CMake then creates the empty `.ts` file), and a glossary column.

## History and attribution

RagoShell is based on the open-source Robomongo 1.4.4 codebase, whose development ended in 2021. Attribution for the original work lives in `COPYRIGHT`.

## License

Copyright of the original codebase belongs to its original authors (2014-2021); RagoShell modifications are copyright of the RagoShell contributors.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License version 3 as published by the Free Software Foundation. This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See `LICENSE`, `LICENSE.LGPLv3`, `THIRD-PARTY-NOTICES.txt`, and <https://www.gnu.org/licenses/> for details.

RagoShell uses the Qt 6 Toolkit (Community / Open Source Edition) under the GNU LGPLv3 / GPLv3, QScintilla under the GNU GPLv3, and official MongoDB C/C++ drivers and OpenSSL 3 under the Apache License 2.0. The mobile builds also bundle the DejaVu Sans font (`assets/fonts/DEJAVU-LICENSE`). Full copyright and license notices for all third-party components are listed in `THIRD-PARTY-NOTICES.txt` and inside the application under **Help → Open-Source Licenses**.
