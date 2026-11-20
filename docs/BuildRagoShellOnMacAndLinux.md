# Building RagoShell on macOS and Linux

## Prerequisites

- CMake 3.20+ and Ninja
- A C++17 compiler (Clang on macOS, GCC or Clang on Linux)
- [vcpkg](https://vcpkg.io) checkout — dependencies are declared in `vcpkg.json`
- Qt 6 (tested with 6.12.x)
- Node.js is optional (the translation check `node ci/check-i18n.mjs` only); the shell runs inside the app

## Setting up a build host

Step by step for a fresh machine. The same host serves local builds, remote
builds from Visual Studio or Qt Creator on Windows, and the packaging scripts
in `ci/` (see `docs/RemoteBuild.md`).

### macOS

1. Turn on SSH, needed only for remote builds: System Settings → General →
   Sharing → **Remote Login**.
2. Install the compiler and the build tools:

   ```bash
   xcode-select --install
   brew install cmake ninja rsync node pkg-config autoconf automake libtool
   ```

   `pkg-config` and the autotools are used by vcpkg while it builds openssl
   and the MongoDB drivers.
3. Install Qt 6.12 with the Qt Online Installer, into `~/Qt/6.12.0/macos`.
4. Clone and bootstrap vcpkg. Make a full clone, not `--depth 1`:
   `vcpkg.json` pins a `builtin-baseline` commit, and a shallow clone does not
   have it.

   ```bash
   git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
   ~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
   ```

5. Set the environment in `~/.zshenv`. Do not use `~/.zshrc`: non-interactive
   SSH sessions (remote builds, `ssh <mac-host> ./ci/build-macos.sh`) do not read it.

   ```bash
   cat >> ~/.zshenv <<'EOF'
   export VCPKG_ROOT="$HOME/vcpkg"
   export PATH="$VCPKG_ROOT:/opt/homebrew/bin:$PATH"
   export Qt6_DIR="$HOME/Qt/6.12.0/macos"
   EOF
   ```

6. Open a new terminal and check:

   ```bash
   vcpkg version
   echo $VCPKG_ROOT $Qt6_DIR
   ```

The `macos-*` presets target Apple Silicon (`arm64-osx`). On an Intel Mac
(`uname -m` prints `x86_64`) they need an `x64-osx` variant.

### Linux (Debian/Ubuntu)

1. Install the tools and desktop libraries (`openssh-server` only for remote builds):

   ```bash
   sudo apt install build-essential gdb ninja-build cmake rsync zip curl unzip tar \
       pkg-config autoconf automake libtool file patchelf desktop-file-utils appstream \
       libgl1-mesa-dev libegl1-mesa-dev libwayland-dev libxkbcommon-dev libxkbcommon-x11-0 \
       libfontconfig1-dev libsecret-1-dev libxcb-cursor0 libxcb-keysyms1 libxcb-image0 \
       libxcb-shm0 libxcb-render-util0 libxcb-xfixes0 libxcb-shape0 libxcb-icccm4 \
       libxcb-xinerama0 libxcb-randr0 libxcb-sync1 libxcb-xkb1 openssh-server nodejs npm
   ```

2. Install Qt 6.12 with the Qt Online Installer, into `~/Qt/6.12.0/gcc_64`.
3. Clone and bootstrap vcpkg (full clone, as on macOS):

   ```bash
   git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
   ~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
   ```

4. Set the environment in `~/.profile`. Ubuntu's `~/.bashrc` returns early in
   non-interactive shells, so remote builds would not see it.

   ```bash
   cat >> ~/.profile <<'EOF'
   export VCPKG_ROOT="$HOME/vcpkg"
   export PATH="$VCPKG_ROOT:$PATH"
   export Qt6_DIR="$HOME/Qt/6.12.0/gcc_64"
   EOF
   ```

### Dependencies

There is nothing to install with `vcpkg install`. The project uses vcpkg in
manifest mode: the first `cmake --preset ...` builds the packages listed in
`vcpkg.json` (mongo-cxx-driver, mongo-c-driver, openssl, libssh2). That first configure
takes 10–20 minutes; later runs reuse the vcpkg binary cache.

## Configure and build

Set the two environment variables the presets rely on (done once above for a
build host). `Qt6_DIR` is the Qt prefix, not `lib/cmake/Qt6`: the packaging
scripts run `$Qt6_DIR/bin/macdeployqt` from it.

```bash
export VCPKG_ROOT=/path/to/vcpkg
export Qt6_DIR=/path/to/Qt/6.12.0/<gcc_64|macos>   # adjust to your Qt
```

Linux x64:

```bash
cmake --preset linux-debug          # or linux-release
cmake --build --preset linux-debug
```

macOS arm64 (Apple Silicon):

```bash
cmake --preset macos-debug          # or macos-release
cmake --build --preset macos-debug
```

Build output lands in `build/linux-<config>/` or `build/macos-<config>/`; the
executable is under `src/ragoshell-app/` (the shell runtime is compiled into it).

## Packaging

Platform packaging scripts live under `ci/`:

```bash
./ci/build-linux.sh <version>   # produces dist/ragoshell-<version>-linux-x64.AppImage and .tar.gz
./ci/build-macos.sh <version>   # produces dist/ragoshell-<version>-macos-arm64.dmg and .tar.gz
```

On Linux, `./ci/build-linux.sh` builds both:
- `dist/ragoshell-<version>-linux-x64.AppImage` — self-contained single-file bundle carrying Qt 6 shared libraries, QML modules, Wayland/XCB/offscreen/TLS/imageformats/iconengines plugins, `ragoshell`, `ragoshell-mcp`, `ragoshell.desktop`, AppStream metainfo (`tr.com.ragoshell.app.metainfo.xml`), hicolor icons, and `licenses/`. Run `ragoshell-mcp` directly from the AppImage via `./ragoshell-<version>-linux-x64.AppImage --mcp --status` (or via a `ragoshell-mcp` symlink to the `.AppImage`). `appimagetool` (`1.9.1`) is pinned by release tag and verified by SHA-256 before execution (cacheable via `RAGO_LINUX_TOOLS_DIR`), invoked with `APPIMAGE_EXTRACT_AND_RUN=1` (`--appimage-extract-and-run`) so FUSE is not required in Docker/CI, and verified after packaging with `LD_LIBRARY_PATH` and `QT_PLUGIN_PATH` unset across `en`, `ar`, and `ja`.
- `dist/ragoshell-<version>-linux-x64.tar.gz` — archive containing `ragoshell`, `ragoshell-mcp`, `ragoshell.desktop`, `tr.com.ragoshell.app.metainfo.xml`, `icons/`, and `licenses/` (for system-Qt or custom-runtime setups).

On macOS, `./ci/build-macos.sh` builds both:
- `dist/ragoshell-<version>-macos-arm64.dmg` — compressed read-only (`UDZO`) drag-and-drop disk image containing `RagoShell.app`, an `/Applications` symbolic link, and `licenses/`.
- `dist/ragoshell-<version>-macos-arm64.tar.gz` — tarball containing `RagoShell.app` and `licenses/`.

### macOS Code Signing and Notarization

By default (when no signing environment variables are set), `./ci/build-macos.sh` emits a warning and creates ad-hoc signed `.app` and `.dmg` artifacts so local developer builds and external pull requests stay green. If **any** signing or notarization variable is partially set or conflicting, the script fails fast with a non-zero exit code before CMake runs (pass `--validate-signing-only` to test signing configuration alone).

To distribute outside the Mac App Store without Gatekeeper quarantine blocks, sign with a **`Developer ID Application`** certificate from the Apple Developer Program company team (**not** `Apple Distribution`, which is strictly for App Store / TestFlight uploads, and **not** `Developer ID Installer`, which is only for `.pkg` installers) and notarize with Apple:

#### Local Developer Mac (Keychain + `notarytool` Profile)

1. Install the **`Developer ID Application`** certificate and private key in your `login` Keychain (`security find-identity -v -p codesigning`).
2. Store notarization credentials once in Keychain (either with an App Store Connect API `.p8` key, or with your Apple ID + app-specific password entered at the hidden prompt):
   ```bash
   xcrun notarytool store-credentials "rago-notary" \
       --apple-id "your-apple-id@example.com" \
       --team-id "YOUR_TEAM_ID"
   ```
3. Run the packaging script:
   ```bash
   RAGO_MAC_SIGN_ID="Developer ID Application: Your Company (TEAM_ID)" \
   RAGO_MAC_NOTARY_PROFILE="rago-notary" \
   ./ci/build-macos.sh <version>
   ```

#### Headless CI / GitHub Actions (Ephemeral Keychain + App Store Connect Notary API Key)

Set the following repository secrets (without touching `login.keychain-db`; temporary `.p12`/`.p8` files and the ephemeral Keychain are wiped in `trap ... EXIT`):

- `RAGO_MAC_SIGN_ID` *(or `APPLE_DEVELOPER_ID`)* — `"Developer ID Application: Your Company (TEAM_ID)"`
- `RAGO_MAC_CERT_P12_BASE64` *(or `RAGO_MAC_CERT_P12` for a local `.p12` path)* — Base64-encoded `.p12` bundle
- `RAGO_MAC_CERT_PASSWORD` — `.p12` export password
- `RAGO_MAC_NOTARY_KEY_ID` *(or `APPLE_API_KEY_ID`)* — App Store Connect API Key ID
- `RAGO_MAC_NOTARY_ISSUER` *(or `APPLE_API_ISSUER`)* — App Store Connect Issuer UUID
- `RAGO_MAC_NOTARY_KEY_BASE64` *(or `APPLE_API_KEY` / `RAGO_MAC_NOTARY_KEY_FILE`)* — Base64-encoded `AuthKey_<KEY_ID>.p8`

When signing and notarization are enabled, `./ci/build-macos.sh`:
- Signs all nested `.dylib`/`.so` libraries, `Frameworks/*.framework`, `PlugIns`, `ragoshell-mcp`, and `RagoShell.app` inside-out with Hardened Runtime (`--options runtime --timestamp`) and `install/macosx/entitlements.plist` (`allow-jit`, `allow-unsigned-executable-memory`), then verifies with `codesign --verify --deep --strict` and runs headless `--check-qml` (`en`, `ar`, `ja`) on the signed bundle.
- Creates and signs `dist/ragoshell-<version>-macos-arm64.dmg`, submits it via `xcrun notarytool submit --wait`, verifies `"Accepted"` status (dumping `notarytool log` automatically on rejection), staples and validates tickets on both the `.dmg` and `RagoShell.app` (`xcrun stapler staple` + `validate`), and verifies Gatekeeper policy via `spctl` (`accepted source=Notarized Developer ID`) before packing the `.tar.gz`.

See `docs/MacOsCodeSigning.md` for step-by-step CSR, certificate, `.p12`/`.p8` setup, and `ci/README.md` for publishing packages as Gitea releases.

## Fonts for the UI languages

Qt falls back to installed fonts for characters the UI font lacks. On Linux
install them for Chinese, Japanese, Hindi and Arabic, or those languages show
empty boxes (Debian/Ubuntu names):

```bash
sudo apt install fonts-noto-cjk fonts-noto-core
```

`fonts-noto-cjk` covers Chinese and Japanese (the app asks for the Simplified
Chinese or the Japanese shapes by language), `fonts-noto-core` covers
Devanagari and Arabic. Windows and macOS ship suitable fonts. See
`docs/Localization.md`.

## Testing

Unit tests (the live tests need a MongoDB 4.0 on `localhost:27017` with a `samples` database and skip themselves
without one; see the README, "Test data"):

```bash
./build/<preset>/src/ragoshell-unit-tests/<config>/rago_unit_tests
```
