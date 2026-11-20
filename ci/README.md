# Local & Automated CI

Each platform has a self-contained build, test, and packaging script under `ci/`.
All scripts read the product identity (`ragoshell` / `RagoShell`) from
`cmake/RagoShellDefaults.cmake` and drop release artifacts into `dist/` under a
uniform naming scheme.

## Build & Packaging Scripts

| Platform | Script | CMake Preset(s) | Output (`dist/`) |
|---|---|---|---|
| Windows x64 | `ci/build-windows.ps1` | `windows` | `ragoshell-<ver>-windows-x64.zip`, `ragoshell-<ver>-windows-x64-setup.exe`, `ragoshell-<ver>-windows-x64.msi` |
| Linux x64 | `ci/build-linux.sh` | `linux-release` | `ragoshell-<ver>-linux-x64.AppImage`, `ragoshell-<ver>-linux-x64.tar.gz` |
| macOS arm64 | `ci/build-macos.sh` | `macos-release` | `ragoshell-<ver>-macos-arm64.dmg`, `ragoshell-<ver>-macos-arm64.tar.gz` |
| Android arm64 | `ci/build-android.sh`, `ci/build-android.ps1` | `android-arm64-macos-release` (macOS), `android-arm64-linux` (Linux), `android-arm64-release` (Windows) | `ragoshell-<ver>-android-arm64.apk`, `ragoshell-<ver>-android-arm64.aab` |

When `<ver>` is omitted (`.\ci\build-windows.ps1`, `./ci/build-linux.sh`,
`./ci/build-macos.sh`, `./ci/build-android.sh`, `.\ci\build-android.ps1`), every script defaults to
`PROJECT_VERSION` from the top-level `CMakeLists.txt` (e.g. `2.1.0`). Pass an
explicit version or tag (e.g. `2.1.0` or `v2.1.0`) to override it.

Desktop archives and installers include the GUI binary (`ragoshell` /
`RagoShell.app`), the read-only stdio MCP server (`ragoshell-mcp`, placed inside
`RagoShell.app/Contents/MacOS/` on macOS, inside `AppDir/usr/bin/` and reachable
via `--mcp` or a `ragoshell-mcp` symlink in the Linux `.AppImage`, and next to
`ragoshell` on Windows and in the Linux `.tar.gz`), and the full `licenses/`
bundle installed by `cmake/RagoShellLicenses.cmake` (`cmake --install <build>
--component licenses`, placing root notices `LICENSE`, `LICENSE.LGPLv3`,
`COPYRIGHT`, `THIRD-PARTY-NOTICES.txt`, embedded notices under
`licenses/third-party/`, and `vcpkg_installed/<triplet>/share/<port>/copyright`
notices as `licenses/third-party/<port>-COPYRIGHT` inside
`RagoShell.app/Contents/Resources/licenses` on macOS and `licenses/` on Windows
and Linux). The macOS `.dmg` (`UDZO` compressed read-only image) additionally
provides an `/Applications` drag-and-drop symbolic link alongside
`RagoShell.app` and `licenses/`. On Linux, `ci/build-linux.sh` produces both a
self-contained `.AppImage` (bundling Qt 6 libraries, QML modules,
Wayland/XCB/offscreen/TLS/imageformats/iconengines plugins, `ragoshell`,
`ragoshell-mcp`, `ragoshell.desktop`, `tr.com.ragoshell.app.metainfo.xml`,
`icons/hicolor/`, and `licenses/`) and a portable `.tar.gz` archive (for
system-Qt or custom-runtime installations).

## Host Requirements & CMake Presets

Check available presets on the current host with `cmake --list-presets` and
`cmake --list-presets=build`.

- **Common (all hosts):**
  - CMake 3.25+, Git, and a C++17 toolchain.
  - Full `vcpkg` clone (`VCPKG_ROOT` on Linux, macOS, and Android; Windows uses
    `$env:VCPKG_ROOT`, `$env:VCPKG_INSTALLATION_ROOT`, or the Visual Studio
    bundled `vcpkg`). Manifest dependencies (`mongo-cxx-driver`, `mongo-c-driver`,
    `openssl`, `libssh2`) are built automatically on first configure.
  - Qt 6 (tested with 6.12.x; `Core`, `Gui`, `Widgets`, `PrintSupport`,
    `Network`, `Xml`, `Core5Compat`, `Qml`, `Quick`, `QuickControls2`,
    `LinguistTools`).
  - Node.js on `PATH` for `ci/check-i18n.mjs`, `ci/generate-sbom.mjs`, and
    `ci/generate-releases-json.mjs` only (nothing of Node.js is bundled into the
    application or packages).
- **Windows (`windows`, `windows-ninja`):**
  - Visual Studio 2022 (17.x) or 2026 (18.x) (`vswhere` auto-detected), `Qt6_DIR`
    (falls back to `C:\Qt\6.12.0\msvc2022_64`), and `windeployqt`.
  - Optional installer tools: NSIS (`makensis.exe`, 3.08+ recommended for
    compile-time `Uninstall.exe` signing) for `-setup.exe` and WiX v5 CLI
    (`wix.exe` with `WixToolset.UI.wixext`) for `.msi` (skipped with a warning
    when absent; portable `.zip` is always built).
  - Optional Authenticode code signing (`signtool.exe` from Windows 10/11 SDK):
    set one credential mode — PFX (`RAGO_WIN_SIGN_PFX` or
    `RAGO_WIN_SIGN_PFX_BASE64` + `RAGO_WIN_SIGN_PASSWORD`), Windows Certificate
    Store (`RAGO_WIN_SIGN_SHA1` or `RAGO_WIN_SIGN_SUBJECT`), or Azure Trusted
    Signing (`RAGO_WIN_SIGN_DLIB` + `RAGO_WIN_SIGN_METADATA` /
    `RAGO_WIN_SIGN_METADATA_JSON`) — plus optional `RAGO_WIN_SIGN_TIMESTAMP_URL`
    (RFC 3161 SHA-256). Without any `RAGO_WIN_SIGN_*` variables, signing is
    skipped with a warning; when any `RAGO_WIN_SIGN_*` variable is set, missing
    parameters, missing `signtool.exe`, or a failed signature/verification stops
    the build immediately. See `docs/WindowsCodeSigning.md`.
- **Linux (`linux-debug`, `linux-release`):**
  - Ninja, GCC or Clang, `pkg-config`, `autoconf`, `automake`, `libtool`, `curl`,
    `file`, `patchelf`, Qt 6 `gcc_64` (`Qt6_DIR`), and Mesa/Wayland/X11/XCB/xkb/fontconfig
    packages (`docs/BuildRagoShellOnMacAndLinux.md`). When `desktop-file-validate`
    (`desktop-file-utils`) and `appstreamcli` (`appstream`) are installed,
    `ci/build-linux.sh` also validates `ragoshell.desktop` and
    `tr.com.ragoshell.app.metainfo.xml` (`appstreamcli validate --no-net`; pass
    `--validate-packaging-only` to run metadata, version, and SHA-256 guard checks
    alone).
  - External AppImage tooling (`appimagetool` `1.9.1`) is pinned by release tag and
    verified against its hardcoded SHA-256 digest before execution, cached in
    `RAGO_LINUX_TOOLS_DIR` when set, and invoked with `APPIMAGE_EXTRACT_AND_RUN=1`
    (`--appimage-extract-and-run`) so `/dev/fuse` is never required in containers or CI.
  - Install `fonts-noto-cjk` and `fonts-noto-core` for CJK, Devanagari, and Arabic
    rendering.
- **macOS (`macos-debug`, `macos-release`, `ios`):**
  - Apple Silicon (`arm64`, minimum deployment target macOS 14.4), Xcode Command
    Line Tools, Ninja, `pkg-config`, `autoconf`, `automake`, `libtool`, and Qt 6
    `macos` (`Qt6_DIR`, providing `$Qt6_DIR/bin/macdeployqt`).
  - Optional Developer ID code signing and Apple notarization:
    - Local Keychain mode: `RAGO_MAC_SIGN_ID` (`Developer ID Application: ...`, or
      alias `APPLE_DEVELOPER_ID`) plus optional `RAGO_MAC_NOTARY_PROFILE` (stored
      via `xcrun notarytool store-credentials`).
    - Headless CI / ephemeral Keychain mode: `RAGO_MAC_SIGN_ID` +
      `RAGO_MAC_CERT_P12` or `RAGO_MAC_CERT_P12_BASE64` +
      `RAGO_MAC_CERT_PASSWORD`, plus App Store Connect Notary API Key triple
      (`RAGO_MAC_NOTARY_KEY_ID`, `RAGO_MAC_NOTARY_ISSUER`, and
      `RAGO_MAC_NOTARY_KEY_FILE` or `RAGO_MAC_NOTARY_KEY_BASE64` /
      `APPLE_API_KEY`).
    - Without any `RAGO_MAC_*` / `APPLE_*` variables, the `.app` and `.dmg` stay
      ad-hoc signed with a warning; when any variable is set, missing, partial, or
      conflicting settings fail fast before CMake runs (`--validate-signing-only`
      validates signing configuration alone). See `docs/MacOsCodeSigning.md`.
  - iOS / iPadOS builds (`ios` / `ios-debug` / `ios-release`) require full Xcode
    and `Qt6_DIR_IOS` (`docs/Platforms.md`).
- **Android (`android-arm64-macos-release`, `android-arm64-macos`, `android-arm64-linux`, `android-arm64`, `android-arm64-release`, `android-x86_64`):**
  - Ninja, JDK 17 / 21, host Qt (`Qt6_DIR`), Android Qt (`Qt6_DIR_ANDROID`,
    auto-detected from `$Qt6_DIR/../android_arm64_v8a` when unset),
    `ANDROID_SDK_ROOT` (`build-tools` providing `zipalign` and `apksigner`), and
    `ANDROID_NDK_ROOT` (auto-detected from the newest `$ANDROID_SDK_ROOT/ndk/*`
    when unset; NDK r27+ / r30 recommended).
  - `ci/build-android.sh` runs on macOS (`android-arm64-macos-release`) and Linux
    (`android-arm64-linux`) hosts; `ci/build-android.ps1` runs on Windows
    (`android-arm64-release`). Both produce `dist/ragoshell-<ver>-android-arm64.apk`
    and `dist/ragoshell-<ver>-android-arm64.aab`.
  - Optional Google Play upload-keystore signing: set `RAGO_ANDROID_KEYSTORE` or
    `RAGO_ANDROID_KEYSTORE_BASE64` (`ANDROID_KEYSTORE_BASE64`),
    `RAGO_ANDROID_KEYSTORE_PASS` (`ANDROID_KEYSTORE_PASSWORD`),
    `RAGO_ANDROID_KEY_ALIAS` (`ANDROID_KEY_ALIAS`), and optional
    `RAGO_ANDROID_KEY_PASS` (`ANDROID_KEY_PASSWORD`). Without any
    `RAGO_ANDROID_KEYSTORE*` variables, unsigned/debug-signed packages are emitted
    with a `warn:` line; when any variable is set, missing, conflicting, or invalid
    credentials fail fast before CMake runs (`--validate-signing-only` /
    `-ValidateSigningOnly` validates credentials alone). See
    `docs/AndroidCodeSigning.md`.

## Test Gates, Fail-Fast Exit Codes & Isolation

Every script enforces fail-fast execution (`set -euo pipefail` in bash;
`$ErrorActionPreference = 'Stop'` plus explicit `$LASTEXITCODE` checks in
PowerShell). **A failure at any build, test, translation, deployment, or QML
verification step immediately terminates the script with a non-zero exit code
before any `dist/` archive or installer is produced.**

1. **Unit tests (`Windows`, `Linux`, `macOS`):** Runs `rago_unit_tests`. Live
   MongoDB tests automatically skip when no server listens on `localhost:27017`.
2. **Translation freshness & consistency (`Windows`, `Linux`, `macOS`, plus `Android` when `node` is on `PATH`):**
   - Desktop scripts build `--target update_translations` and fail via
     `git diff --exit-code -- src/ragoshell-app/i18n` if any `.ts` catalog is
     stale (refresh and commit `src/ragoshell-app/i18n/*.ts`).
   - Runs `node ci/check-i18n.mjs`: fails if text is concatenated onto `qsTr()`,
     if `%1`/`%n` placeholders diverge between source and translation, or if
     plural forms are malformed; prints the completeness table across all 15
     languages.
3. **Deployment, `licenses/` bundle, signing & bundle verification:**
   - Installs the `licenses/` component via `cmake --install <build> --component
     licenses` (`cmake/RagoShellLicenses.cmake`) and fail-fast verifies both
     `ragoshell` and `ragoshell-mcp` executables plus every root notice,
     embedded `third-party/` notice, and `third-party/<port>-COPYRIGHT` file via
     `node ci/generate-sbom.mjs --verify-licenses <dir> --require-vcpkg-copyrights`
     (non-empty regular files, zero symlinks, and GPLv3 §6 / LGPLv3 §4
     declarations in `THIRD-PARTY-NOTICES.txt`; `--validate-packaging-only` and
     `--validate-signing-only` run `--verify-source-licenses`).
   - Runs `windeployqt` (Windows), `macdeployqt` + `codesign --verify --deep
     --strict` (macOS), or `qmlimportscanner` + recursive `ldd` bundling +
     `patchelf` relative `$ORIGIN` RPATH rewriting + `appimagetool` (Linux); any
     deployment, unresolved library, non-relocatable RPATH, or signature error
     stops the build.
   - On Windows, when `RAGO_WIN_SIGN_*` is configured, signs `ragoshell.exe` and
     `ragoshell-mcp.exe` in `stage` prior to `--check-qml` and archiving, signs
     `Uninstall.exe` at NSIS compile time (`!uninstfinalize`), signs the output
     `-setup.exe` and `.msi` installers, and verifies every signed file with
     `signtool verify /pa /v`.
   - On macOS, when `RAGO_MAC_SIGN_ID` is configured, signs all nested
     `.dylib`/`.so` libraries, `Frameworks/*.framework`, `PlugIns`,
     `ragoshell-mcp`, and `RagoShell.app` inside-out with Hardened Runtime
     (`--options runtime --timestamp`) and `install/macosx/entitlements.plist`
     prior to `--check-qml`, builds and signs the `UDZO` `.dmg`, and (when notary
     credentials are set) runs `xcrun notarytool submit --wait`, `xcrun stapler
     staple` + `validate`, and Gatekeeper `spctl` checks on both `.app` and
     `.dmg`.
4. **Headless QML load check (`Windows`, `Linux`, `macOS`):**
   - Runs the **staged** application binary with `--check-qml` across three
     locales: default English, `--lang ar` (right-to-left layout), and
     `--lang ja` (CJK font fallback), using `QT_QPA_PLATFORM=offscreen` on Linux
     and macOS. On Linux, `ci/build-linux.sh` additionally tests the packed
     `.AppImage` with `LD_LIBRARY_PATH`, `QT_PLUGIN_PATH`, `QML2_IMPORT_PATH`,
     and `Qt6_DIR` **unset** (`--appimage-extract-and-run --check-qml` across
     `en`, `ar`, `ja`, verifying via `LD_DEBUG=libs` that zero libraries are
     loaded from the host `Qt6_DIR`, plus `--mcp --status` and `ragoshell-mcp`
     symlink invocation). Any QML load error or warning exits with code `1` and
     aborts packaging.
5. **Android APK, AAB, signing & 16 KB ELF page alignment (`ci/build-android.sh`, `ci/build-android.ps1`):**
   - Fails with exit code `1` if either the Release `.apk` or `.aab` is missing or
     empty.
   - When `RAGO_ANDROID_KEYSTORE*` is configured, runs `zipalign -P 16 -f -v 4`
     (or `-p` on Build-Tools < 35) + `apksigner sign` (v2 + v3) + `apksigner
     verify` on the `.apk`, and `jarsigner` (SHA-256) + `jarsigner -verify
     -verbose -certs` (`jar verified.`) on the `.aab`, passing passwords strictly
     via `env:` and zero-wiping any temporary decoded keystore on exit.
   - Enforces **`RAGO_ANDROID_STRICT_ALIGN=1` by default**: inspects every `.so`
     ELF `LOAD` segment via `llvm-readelf -lW` / `readelf -lW` across the build
     tree **and** all `lib/arm64-v8a/*.so` / `base/lib/arm64-v8a/*.so` entries
     extracted from the produced `.apk` and `.aab`, failing with exit code `1` if
     any `.so` reports `p_align < 0x4000` (16 KB) required by Android 15+ / Google
     Play (`--check-align-only` / `-CheckAlignOnly` runs this gate standalone).

### Host Isolation

`ci/build-linux.sh`, `ci/build-macos.sh`, `ci/build-windows.ps1`,
`ci/build-android.sh`, and `ci/build-android.ps1` create an
isolated temporary directory per run, set `RAGOSHELL_CONFIG_DIR` (and
`XDG_CONFIG_HOME` on Linux) plus `RAGOSHELL_KEYSTORE=file` on desktop runs, reset the temporary
config directory before `--check-qml`, zero-wipe any temporary decoded `.pfx`
material on Windows, temporary `.p12`/`.p8` + ephemeral Keychain files on
macOS (restoring the previous Keychain search list), and temporary decoded
Android `.keystore` files on Android builds (`0700` directory, `0600` file), and remove the temporary
directory on exit (`trap ... EXIT` / `finally`). Local CI runs never read,
overwrite, or pollute the developer's `~/.ragoshell/` settings, macOS
`login.keychain-db`, Linux Secret Service keyring, or Windows Credential
Manager.

## Automated CI Workflows

- **GitHub Actions (`.github/workflows/ci.yml`):** Runs on pushes, tags, and pull
  requests on the public GitHub mirror (`i18n` — including
  `node ci/test-sbom-and-licenses.mjs`, deterministic SPDX 2.3 SBOM generation,
  and SHA-256-verified Google OSV-Scanner `v2.7.0` vulnerability scanning across
  `vcpkg.json` and the generated SBOM — plus `linux`, `macos`, `windows`,
  `android`, and — on `v*` tags or manual `workflow_dispatch` with
  `publish_release: true` — `release` jobs calling the scripts above and below,
  caching SHA-256-verified Linux AppImage tooling, uploading both `.AppImage`
  and `.tar.gz` Linux artifacts, `.dmg` + `.tar.gz` macOS artifacts, and `.apk`
  + `.aab` Android artifacts, and
  producing `dist/<product>-<version>-sbom.spdx.json` alongside Minisign-signed
  `releases.json` + `SHA256SUMS.txt` metadata). See
  `docs/WindowsCodeSigning.md`, `docs/MacOsCodeSigning.md`,
  `docs/AndroidCodeSigning.md`, and `docs/ReleasePipeline.md`.

## Release Manifest, SPDX 2.3 SBOM, Minisign Signing & Multi-Target Publishing

Full design, schema, threat model, key lifecycle, and atomic upload order are
specified in `docs/ReleasePipeline.md`.

- `node ci/generate-sbom.mjs [--version <ver>] [--dist dist]` — zero-dependency
  deterministic SPDX 2.3 JSON SBOM generator and license verifier. Reads product
  identity strictly from `cmake/RagoShellDefaults.cmake` (never reading
  `ci/.env`), merges `vcpkg.json` (`builtin-baseline` + `overrides`),
  `vcpkg_installed/<triplet>/share/<port>/vcpkg.spdx.json` when present, Qt 6
  modules, embedded `src/third-party/` components, and SHA-256 digests of built
  `dist/` packages into `dist/ragoshell-<ver>-sbom.spdx.json` with `purl`,
  `cpe23Type`, and SPDX license expressions. Also provides `--verify-only`,
  `--verify-source-licenses`, and `--verify-licenses <dir>
  [--require-vcpkg-copyrights]`.
- `node ci/generate-releases-json.mjs [--version <ver>] [--dist dist]` — scans
  `dist/` release artifacts (`.exe`, `.msi`, `.zip`, `.dmg`, `.tar.gz`,
  `.AppImage`, `.apk`, `.aab`) plus `ragoshell-<ver>-sbom.spdx.json` when present,
  validates strict SemVer, `https://` allowlisted hosts
  (`dl.<domain>`, `play.google.com`, `apps.apple.com`), non-empty regular files,
  and `CHANGELOG` entries, and writes `dist/SHA256SUMS.txt` (including the SBOM
  digest) and `dist/releases.json` (`manifest.sbom`, 100% compatible with
  `ragoshell-site`, `services/release-pulse-api/server.mjs`, and
  `UpdateService`). Pass `--verify-only` to re-verify an existing
  `dist/releases.json`, `dist/SHA256SUMS.txt`, and `.spdx.json` against disk
  binaries.
- `./ci/sign-release.sh [--version <ver>] [--dist dist]` — signs `releases.json`
  (`releases.json.minisig`) and `SHA256SUMS.txt` (`SHA256SUMS.txt.minisig`) with
  `minisign` (Ed25519 + BLAKE2b-512), binding
  `release:<version>\ttimestamp:<UTC ISO-8601>\tfile:<basename>` inside the
  signed `trusted comment`, and immediately verifies both signatures with
  `minisign -Vm` and `ci/lib/minisign.mjs` (`RAGO_MINISIGN_SECRET_KEY` /
  `RAGO_MINISIGN_SECRET_KEY_FILE`, optional `RAGO_MINISIGN_PASSWORD`, and
  `RAGO_MINISIGN_PUBKEY` or `cmake/RagoShellDefaults.cmake`).
- `./ci/publish-release.sh [--dry-run] [--targets r2,github,gitea,pulse,site] [version]`
  — verifies artifacts, SPDX 2.3 SBOM, and Minisign signatures before touching
  the network, uploads versioned packages, `.spdx.json`
  (`Content-Type: application/spdx+json; charset=utf-8`), and metadata to
  Cloudflare R2 (`releases/<version>/*`,
  `Cache-Control: public, max-age=31536000, immutable`), publishes assets, SBOM,
  and `.minisig` signatures to GitHub Releases and Gitea Releases, atomically
  updates root `releases.json.minisig` + `releases.json`
  (`Cache-Control: public, max-age=300`) last, and triggers optional
  `release-pulse-api` (`PULSE_API_ADMIN_URL` + `PULSE_ADMIN_TOKEN`) or static
  site (`RAGO_SITE_RELEASES_SSH=deploy@host`, streaming `releases.json(.minisig)`
  to `ragoshell-infra`'s `deploy-receive.sh releases` forced command) hooks when
  included in `--targets`. Never reads `ci/.env`.
- `node ci/test-sbom-and-licenses.mjs` — zero-dependency automated test suite
  covering CMake `licenses/` bundle rules (`cmake/RagoShellLicenses.cmake`),
  `THIRD-PARTY-NOTICES.txt` GPLv3 §6 / LGPLv3 §4 and brand cleanliness, SPDX 2.3
  JSON determinism and schema validation, negative/tamper guards, and release
  pipeline SBOM integration.
- `node ci/test-release-pipeline.mjs` — zero-dependency automated test suite
  covering manifest generation, cross-repo `ragoshell-site` and
  `release-pulse-api` compatibility, negative security tests, Ed25519 /
  `trusted comment` tamper/replay guards, `--dry-run` R2 ordering, and (when
  `minisign` is installed) end-to-end CLI keygen, signing, and verification.

## Translation Helper Scripts

- `node ci/check-i18n.mjs` — placeholder, plural, and `qsTr()` concatenation check.
- `python3 ci/fill-english-plurals.py` — populates `%n` singular/plural forms in
  `src/ragoshell-app/i18n/ragoshell_en.ts` after `update_translations`.
- `python3 ci/ts-exchange.py` — exports `.ts` catalogs to chunked JSON and
  imports/validates translated JSON (`export`, `import`, `check`, `forms`).
- `python3 ci/pseudo-translate.py <in.ts> <out.ts>` — generates an accented,
  expanded `[...]` pseudo-locale `.ts` for testing untranslated strings and UI
  clipping (`lrelease out.ts -qm out.qm && ragoshell --qm-file out.qm`).

## Publishing to Gitea Releases

```bash
./ci/gitea-release.sh [--dry-run] [--stable|--prerelease] v2.1.0   # bash
.\ci\gitea-release.ps1 [-DryRun] [-Stable|-Prerelease] v2.1.0      # PowerShell
```

When invoked directly (outside `ci/publish-release.sh`, which sets
`RAGO_NO_DOTENV=1`), the token is resolved in this order — nothing is ever
committed:

1. `GITEA_TOKEN` environment variable
2. `ci/.env` file (git-ignored, skipped when `RAGO_NO_DOTENV=1`): `GITEA_TOKEN=xxxx`
3. Interactive prompt (hidden input) if neither is set

What it does: creates or updates the `v<version>` release on `GITEA_HOST` (set via
environment variable or `ci/.env`) and idempotently uploads every `dist/ragoshell-*`
package and `.spdx.json` SBOM together with `SHA256SUMS.txt(.minisig)` and
`releases.json(.minisig)` when present. Pre-release status defaults automatically
(`true` when the version contains `-`, e.g. `-rc.1`; `false` for stable SemVer)
and can be overridden with `--stable` / `--prerelease`.

Equivalent manual flow (no script): create the tag and release in the Gitea web
UI (`/releases/new`), then drag-and-drop the artifacts from `dist/`.
