# RagoShell — platforms

What runs where, how each platform is built, tested and debugged, and what is still open. It replaces the
tablet plan; its history is in git. Facts were checked against the code and build files on
2026-11-02.

See also: `docs/DesignSystem.md` (touch size table, density), `docs/UiMatrix.md` (every screen on every platform
and size), `docs/MacOsWork.md` (what only a Mac can finish, with commands), `docs/TodoMac.md` (the open work as one list), `docs/Shell.md` (the shell),
`docs/BuildRagoShellOnWindows.md`, `docs/BuildRagoShellOnMacAndLinux.md`, `docs/RemoteBuild.md` (host setup),
`docs/ManualTests.md` (checklists; section 31 is the tablet one).

There is one QML code base (`src/ragoshell-app/`) and one core library (`rago-core`, `src/ragoshell/`). Platform
differences are `#if` blocks, the CMake flags `SYSTEM_IOS`, `SYSTEM_ANDROID`, `SYSTEM_MOBILE`
(`cmake/RagoShellPlatform.cmake`) and the density setting. No QML file is copied per platform.

## 1. Summary

| Platform | Runs | Built with | Tested | Signing and package | Known limits |
|---|---|---|---|---|---|
| Windows x64 | Yes, the main development platform | preset `windows` (Visual Studio 18), Qt 6.12 `msvc2022_64`, vcpkg `x64-windows` | Unit tests, `--check-qml`, layout guard, snapshots, by hand | `ci/build-windows.ps1`: zip, NSIS setup, WiX MSI; not code signed | none known |
| Linux x64 | Builds; unit tests and `--check-qml` pass (WSL) | presets `linux-debug`/`linux-release`, Qt `gcc_64`, vcpkg `x64-linux` | Not looked at on a Linux desktop (`docs/MacOsWork.md`, E) | `ci/build-linux.sh`: self-contained `.AppImage` (Qt 6, QML, Wayland/XCB/TLS plugins, `ragoshell`, `ragoshell-mcp`, `.desktop`, AppStream metainfo, icons, `licenses/`) and `.tar.gz` (expects system Qt) | Key store needs libsecret and a Secret Service, else a key file |
| macOS arm64 | Yes | presets `macos-debug`/`macos-release`, Qt `macos`, vcpkg `arm64-osx` (`vcpkg-overlays/triplets/`), min macOS 14.4, latest `macosx` SDK | Unit tests, `--check-qml` (`dev/mac/build-macos.sh`), layout guard; Command keys and Dock icon need a person | `ci/build-macos.sh`: ad-hoc signed `.app` in a tar.gz; Gatekeeper blocks it after a download | No notarization (no Developer ID certificate); minimum macOS 14.4 |
| iOS / iPadOS arm64 | Yes: universal iPhone + iPad (iPad 8th gen, iPhone 18 Pro / iPad mini / iPad Pro simulators) | preset `ios` (Xcode project, Qt 6.12 for iOS, static Qt, vcpkg `arm64-ios` in `vcpkg-overlays/triplets/`, min iOS/iPadOS 18.0, Base SDK = latest `iphoneos`); Mac only | Layout guard (360×800 and 800×360), pictures on iPhone and iPad simulators, `--shell-selftest` on the device | Personal team, installed with Xcode's Run; no store build | No `mongodb+srv://`; no GSSAPI; minimum iOS / iPadOS 18.0 |
| Android arm64 | Yes: phones and tablets (Android 10 tablet Lenovo TB-X606X, Android 17 emulator) | presets `android-arm64`/`android-arm64-release` (Windows), `android-arm64-macos`/`android-arm64-macos-release` (macOS) and `android-arm64-linux` (Linux), Qt 6.12 for Android, NDK 30, min API 28 (Android 9.0) / target API 37 (Android 17); `android-x86_64` for the emulator | Layout guard (360×800 and 800×360) and pictures through `adb`, `--shell-selftest` on the tablet | `ci/build-android.sh` and `ci/build-android.ps1`: Release `.apk` and `.aab`, optional `RAGO_ANDROID_*` upload-keystore signing (`apksigner` v2/v3 + `jarsigner`), mandatory 16 KB ELF check; production Play upload key pending | No `mongodb+srv://`; no GSSAPI; minimum Android 9.0 (API 28) |

The app, bundle and package id is `tr.com.ragoshell.app` on macOS, iOS and Android, supporting both phones and tablets
(universal iOS `UIDeviceFamily` 1 and 2; Android `smallScreens` through `xlargeScreens`). There is no Node.js in the app or in any package (the
translation check in `ci/` uses Node on the build machine). The MCP server (`ragoshell-mcp`, a desktop stdio
process) and the unit tests are built only when `NOT SYSTEM_MOBILE` (`CMakeLists.txt`), so unit tests run on
Windows, Linux and macOS only.

## 2. Design for touch and small screens

### Touch sizes and form factors

A mouse hits a 28 px button; a finger needs about 44 pt (Apple) or 48 dp (Material). Control heights come from one
table, `UiDensity::sizes(touch, tablet)` (`src/ragoshell/core/utils/UiDensity.h`, unit tested), exposed by
`ThemeBridge` as `Theme.controlHeight`, `fieldHeight`, `iconButtonSize`, `menuItemHeight`, `splitHandle`. The table
is in `docs/DesignSystem.md`. It has three columns because an 800 dp wide tablet fits a form of eight fields only if
controls are a little under the 44 pt minimum, while a phone needs the full size.

The column depends on the setting "Interface density" (Automatic, Compact, Touch; Automatic is touch on iOS and
Android, compact on a desktop) and the form factor (`Platform.formFactor`: "tablet" for a phone or tablet OS with a
shortest side of at least 600 dp, else "phone"). "Touch" on a Windows touch laptop therefore runs the tablet code.
Compact is the original desktop look, kept pixel-identical.

Layout follows the window width, not the platform: the Explorer slides over as a modal drawer below 900 dp, toolbars scroll sideways
instead of cutting button text (`HScrollRow.qml`), the Server Monitor adapts to its page width, and the minimum
window size is 480 x 360 (320 x 320 in touch density). A desktop split window, iPad Split View and Slide Over, and
Android split screen are the same case.

Hover is never the only way to reach an action; context menus open on long press. Shortcut text is shown on a phone
or tablet only while a hardware keyboard is attached (`Platform.shortcutsVisible`: Android reads the configuration,
iPadOS the GameController framework, any Ctrl/Meta/Alt/function key proves one). macOS and iPadOS show the Command
key (`Platform.macKeys`). Every F-key action has a modifier shortcut, as Mac laptops need Fn for F-keys. Text fields
ask the keyboard for no auto-capitals or suggestions, a digits pad for numbers, and "sensitive" for passwords.

The UI icons are Unicode symbols and Android 10 has no font for several (empty boxes), so mobile builds bundle DejaVu
Sans (`assets/fonts`, registered in `main.cpp`).

### Window, safe area and keyboard

On iOS and Android the system sizes the window: `main.cpp` calls `showFullScreen()`, does not save or restore
geometry, and skips the system tray. Android's activity is resizeable with Qt's `configChanges` list (rotation and
multi-window do not restart the app) and `adjustResize`; iPadOS sets `UIRequiresFullScreen` false and allows all
orientations, which Split View needs (`install/android/AndroidManifest.xml`, `install/ios/Info.plist.in`).

The on-screen keyboard was the hardest part. Qt on Android does not shrink the window reliably and reports the
keyboard rectangle in device pixels, so dialogs sat partly behind it and hid Test and Save. `VisibleArea.qml` gives
the height the keyboard leaves (divided by the pixel ratio on Android, and the smaller of that and the window so a
system that also shrinks the window is not subtracted twice). `RagoDialog` and the command palette size and center
in it, a dialog drops its title row below 380 px, and the Connection and Preferences forms scroll the focused field
into view (`FocusReveal.qml`). Checked on the Android 17 emulator and the Android 10 tablet.

The Android back key and gesture (the manifest sets `enableOnBackInvokedCallback`) close a dialog or menu first; on
the main page they do not end the app while connections are open (`Main.qml`).

### Lifecycle and connection keep-alive

A phone or tablet suspends an app soon after it leaves the screen and its sockets die. So
(`BackgroundMonitor` in `src/ragoshell-app`, `BackgroundPolicy` in `src/ragoshell/core/utils`):

- **State is saved as it changes**: the window layout 1 s after the last change and when the app leaves the
  foreground; unsaved shell scripts as drafts (`drafts.json`), offered after a crash. Nothing connects by itself.
- **Background work pauses.** The schema collector (the only background job) runs on mobile only while the app is in
  front, never while suspended, and not on a metered network while "Wait while the connection is metered" is on
  (default). A desktop app without focus keeps working.
- **Connections are re-checked and SSH tunnels re-established.** After the network returns, the app resumes from
  suspension, or the computer sleeps (a 5 s tick that came more than 30 s late), every open connection is refreshed
  after 3 s and for 20 s its errors go to the status bar, not a dialog. Direct connections recover through the driver;
  an SSH tunnel (`SshTunnelWorker`) automatically re-establishes its session on the same `127.0.0.1` loopback port with
  exponential backoff and strict `known_hosts` verification (`docs/SshTunnelReconnect.md`).

### Key store, paths, SSH

Saved passwords and AI API keys are encrypted with a key held by the operating system, not in a file beside the
settings (a copy or backup of the folder would carry both). `KeyStore` (`src/ragoshell/utils/KeyStore*.cpp`) is
written against the system APIs (the vcpkg QtKeychain port brings its own Qt, which clashes with the kit):

| Platform | Store |
|---|---|
| Windows | Credential Manager (`RagoShell/<account>`, per user, not roamed) |
| macOS, iOS | Keychain generic password item |
| Linux | Secret Service through libsecret, loaded with `QLibrary` (no build dependency) |
| Android | AES-256-GCM master key in the Android Keystore encrypts the key; IV and cipher text in `files/keystore/ragoshell-key.bin` |

Without a store (no Secret Service, or `RAGOSHELL_KEYSTORE=file`) the key file `ragoshell.key` is used and
Preferences warns. An old key file moves into the store and is deleted only after the stored key was read back
equal. A `RAGOSHELL_CONFIG_DIR` folder gets its own account so a throw-away folder never replaces the real key.

`RagoShell::appDataDir()` (`core/utils/AppPaths.cpp`) is the one data folder: `$RAGOSHELL_CONFIG_DIR`, else
`~/.ragoshell/` on a desktop (unchanged for existing users), else Qt's `AppDataLocation` on iOS and Android (the home
folder is not writable there). The log is `<appDataDir>/logs/ragoshell.log`, one rotated file at 5 MB. Android sets
`allowBackup="false"` so settings and the key store entry do not reach cloud backups.

SSH tunnels check the server key against `ssh/known_hosts` in the settings folder before any credential is sent; a
new or changed key asks the user. The app links libssh2 1.11.1 from vcpkg (the old in-tree 1.9.0 fails every
handshake with OpenSSL 3).

### Files and memory

File dialogs return URLs; `Backend.readFile`, `saveFile`, `urlToLocalFile` pass `file://` URLs as local paths and
anything else (an Android `content://` URI) as text, which `QFile` opens. `saveFile` enables
`QSaveFile::setDirectWriteFallback(true)` for Android SAF `content://` URIs and falls back to `QFile` on iOS external
security-scoped URLs (`QDarwinSecurityScopedFileEngine`). On iOS and Android, SSH private keys and TLS `CA`, `PEM` and
`CRL` files picked in `ConnectionDialog` are copied via `Backend.importCredentialFile` (`AppPaths::importCredentialFileToDir`)
into `<appDataDir>/credentials/` with `0600` owner-only permissions so `libssh2` and `mongoc`/OpenSSL receive a persistent
POSIX path inside the app container. Import and export stream in 1 MB chunks and 500-document batches (about 35 MB peak
growth for 100,000 documents, measured on Windows). The page size is capped at 1000 (`SettingsManager::kMaxBatchSize`);
big strings are cut in the tree and text view; `Backend.trimMemory()` drops the in-memory schema snapshots.

### Network permissions, DNS, authentication

- **Android permissions (`install/android/AndroidManifest.xml`, `install/android/res/xml/network_security_config.xml`).**
  Explicitly declares `INTERNET`, `ACCESS_NETWORK_STATE`, `ACCESS_WIFI_STATE`, `ACCESS_LOCAL_NETWORK` (runtime prompt on
  Android 17 / API 37+ before connecting to RFC1918, link-local, `.local` or single-label LAN hosts via
  `LocalNetworkPermission.cpp`), and `READ_EXTERNAL_STORAGE` / `WRITE_EXTERNAL_STORAGE` capped with
  `android:maxSdkVersion="28"` (overrides `Qt6Core`'s unbounded storage permission injection so Android 10+ / API 29–37
  builds use Scoped Storage / SAF exclusively; `ensureLegacyStoragePermission` handles API 28 direct external paths).
  Hardware features `android.hardware.touchscreen` and `android.hardware.wifi` are marked `required="false"` for
  Ethernet tablets and Chromebooks, and `network_security_config.xml` enforces HTTPS (`cleartextTrafficPermitted="false"`)
  with loopback/emulator exceptions (`localhost`, `127.0.0.1`, `::1`, `10.0.2.2`).
- **iOS / iPadOS permissions (`install/ios/Info.plist.in`, `install/ios/lproj/*.lproj/InfoPlist.strings`, `install/ios/PrivacyInfo.xcprivacy`).**
  `NSLocalNetworkUsageDescription` prompts on the first LAN MongoDB or SSH tunnel connection, and
  `NSPhotoLibraryUsageDescription` satisfies App Store Connect (`ITMS-90683`) because Qt's static
  `QIOSIntegrationPlugin` (`qiosfiledialog.mm`) links `Photos.framework`. Both purpose strings are localized across all
  15 supported UI languages in `<lang>.lproj/InfoPlist.strings` at the flat `.app` bundle root, unused Darwin permission
  plugins (`Camera`, `Location`, `Contacts`, `Bluetooth`, `Calendar`, `Microphone`) are excluded via
  `qt_import_plugins(ragoshell EXCLUDE_BY_TYPE permissions)`, and `NSAppTransportSecurity` sets
  `NSAllowsLocalNetworking = true` while keeping strict TLS mandatory for public HTTPS endpoints.
- **No `mongodb+srv://` on iOS and Android.** The driver's SRV lookup needs a resolver call that Android's libc
  withholds and the iOS SDK hides, so the overlay port (`vcpkg-overlays/ports/mongo-c-driver/portfile.cmake`) sets
  `ENABLE_SRV=OFF`. Atlas users paste the standard multi-host string. The connection dialog does not offer the type
  there; a saved connection that already is one keeps it, so it can still be read and edited.
- **TLS** is OpenSSL on Windows, Linux, Android and iOS (`vcpkg.json`, the overlay port) and Secure Transport on macOS.
- **No GSSAPI on iOS and Android** (no Kerberos library). `supportedAuthMechanisms()` (`MongoUriUtils.cpp`) hides it
  there; a saved mechanism the build cannot offer stays selectable so an edit does not change it silently. Offered
  everywhere: SCRAM-SHA-1, SCRAM-SHA-256, MONGODB-X509, PLAIN.
- **VPN-only servers** are the most common reason a tablet cannot connect; the user runs the VPN client.

### The shell

The shell runs inside the app on QJSEngine on every platform (`docs/Shell.md`): iOS cannot start a child
process and Android 10+ cannot run files from app data, which ruled out Node.js. iOS allows no JIT, so scripts run
interpreted there and CPU-heavy ones are slower; a runaway loop still ends at the time limit and Stop works.
`--shell-selftest=<host:port>` runs a fixed script set against a server on any platform and prints
`SHELLSELFTEST PASS|FAIL` lines; the exit code is the number of failed checks.

### Packaging notes and OS version bounds

- **Android (`install/android/`, `CMakeLists.txt`, `src/ragoshell-app/CMakeLists.txt`, `vcpkg-overlays/triplets/`):**
  - **Minimum SDK: API 28 (Android 9.0 Pie)** (`ANDROID_PLATFORM=android-28`, `RAGO_ANDROID_MIN_SDK=28`, `QT_ANDROID_MIN_SDK_VERSION=28`, `VCPKG_CMAKE_SYSTEM_VERSION=28`). Set by Qt 6.12's Android toolchain floor and Android Bionic `getrandom()` (added in API 28 and required by OpenSSL 3 and `mongo-c-driver`); Android Keystore `AES/GCM/NoPadding` (API 23+), `resizeableActivity` (API 24+), and adaptive icons (API 26+) all fit below API 28.
  - **Target & Compile SDK: API 37 (Android 17)** (`RAGO_ANDROID_TARGET_SDK=37`, `QT_ANDROID_TARGET_SDK_VERSION=37`; `compileSdk` auto-detected by Qt from the latest installed `ANDROID_SDK_ROOT/platforms/android-37*`, no `android:maxSdkVersion` cap per Google Play policy). Satisfies Google Play's requirement to target the latest Android release; Android 17's `ACCESS_LOCAL_NETWORK` permission is handled at runtime.
  - `versionCode` = major x 1000000 + minor x 10000 + patch x 100 (`src/ragoshell-app/CMakeLists.txt`). Google Play requires every native library to be 16 KB page aligned (`-Wl,-z,max-page-size=16384` in `CMakeLists.txt`, `src/ragoshell-app/CMakeLists.txt` and `vcpkg-overlays/triplets/`); `ci/build-android.sh` and `ci/build-android.ps1` enforce `RAGO_ANDROID_STRICT_ALIGN=1` across the build tree and every `.so` inside the produced `.apk` and `.aab`, and run `zipalign -P 16` before `apksigner` when signing is configured (`docs/AndroidCodeSigning.md`). A `--` inside an XML comment breaks androiddeployqt.
- **iOS and iPadOS (`install/ios/`, `CMakeLists.txt`, `src/ragoshell-app/CMakeLists.txt`, `vcpkg-overlays/triplets/`):**
  - **Minimum OS: iOS / iPadOS 18.0** (`CMAKE_OSX_DEPLOYMENT_TARGET=18.0`, `XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET=18.0`, `MinimumOSVersion=18.0`, `VCPKG_OSX_DEPLOYMENT_TARGET=18.0`). Set by Qt 6.12's hard floor (`QT_SUPPORTED_MIN_IOS_VERSION=18`, prebuilt `QtCore.framework` `minos 18.0`); `mongo-cxx-driver 4.0.0`, `mongo-c-driver 1.30.7`, OpenSSL 3, `libssh2`, `GameController` (iOS 14+), Keychain, `UILaunchScreen` (iOS 14+), and dark/tinted app icons (iOS 18+) all work from 18.0 upward.
  - **Target / Max SDK: `SDKROOT = iphoneos` (latest installed Xcode iOS SDK, no upper OS cap)** — meets Apple App Store Connect's requirement to build against the latest iOS/iPadOS SDK while supporting all releases from 18.0 through the newest iOS/iPadOS.
  - Universal iPhone and iPad (`TARGETED_DEVICE_FAMILY "1,2"`, `UIDeviceFamily` 1 and 2), own `UILaunchScreen` (Qt's storyboard is off), local network text, file sharing on, `PrivacyInfo.xcprivacy` (no tracking; unlinked optional `ProductInteraction` usage metrics for the release check in `docs/UpdateAndTelemetry.md`; the required-reason APIs QSettings and Qt use). The asset catalog must be in Copy Bundle Resources (the `RESOURCE` property) or no icon is built. Static Qt shows a missing QML plugin only at run time: run `--check-qml` on a device build.
- Icons of every platform come from one artwork: `dev/icons/make-icons.sh` (ImageMagick 7) writes the `.ico`, `.icns`,
  Linux hicolor set, Android mipmaps and Play icon, iOS catalog (light, dark, tinted) and the in-app icon.

## 3. Building

Common: a vcpkg checkout (`VCPKG_ROOT` on Mac and Linux; Windows uses Visual Studio's), Qt 6.12 (`Qt6_DIR` on Mac
and Linux), CMake 3.25+. Dependencies come from `vcpkg.json`; the first build of a triplet takes about 20 minutes.

```powershell
# Windows
cmake --preset windows
cmake --build --preset windows-debug
build\windows\src\ragoshell-unit-tests\Debug\rago_unit_tests.exe
build\windows\src\ragoshell\Debug\ragoshell.exe --check-qml
.\ci\build-windows.ps1 -Version 2.1.0          # zip, NSIS setup, WiX MSI in dist\
```

```bash
# Linux (macOS: Qt6_DIR=~/Qt/6.12.0/macos, preset macos-release, ci/build-macos.sh)
export VCPKG_ROOT=~/vcpkg Qt6_DIR=~/Qt/6.12.0/gcc_64
cmake --preset linux-release && cmake --build --preset linux-release
ci/build-linux.sh 2.1.0                          # runs the translation check (needs Node), writes dist/

# From the Windows PC to a macOS build host (ssh host "<mac-host>"):
RAGO_MAC_HOST=<mac-host> dev/mac/sync.sh          # working tree to ~/build-ragoshell
ssh <mac-host> '~/build-ragoshell/dev/mac/build-macos.sh'   # build, unit tests, --check-qml
```

**Android** (Windows, macOS or Linux host). Needs Qt 6.12 for Android (`android_arm64_v8a`, and `android_x86_64`
for the emulator), the Android SDK with build-tools 37 and NDK 30.0.16248370 (`%LOCALAPPDATA%\Android\Sdk` on
Windows, `~/Library/Android/sdk` on macOS), and JDK 17 or 21 for Gradle.

```powershell
# Windows (debug APK or Release APK + AAB)
cmake --preset android-arm64
cmake --build --preset android-arm64-debug --target apk     # about 150 MB debug APK
adb install -r build\android-arm64\src\ragoshell\android-build\build\outputs\apk\debug\android-build-debug.apk
.\ci\build-android.ps1 -Version 2.1.0                       # Release .apk + .aab in dist\ (16 KB check + optional RAGO_ANDROID_* signing)
# emulator: cmake --preset android-x86_64; cmake --build --preset android-x86_64-debug --target apk
```

```bash
# macOS or Linux (Release .apk + .aab in dist/, 16 KB check + optional RAGO_ANDROID_* signing)
./ci/build-android.sh 2.1.0
```

**iPadOS** (on the Mac only). Needs Xcode, Qt 6.12 for iOS (`ios_device`, `ios_simulator_arm64`) and the variables
`VCPKG_ROOT`, `Qt6_DIR` (host Qt) and `Qt6_DIR_IOS` that the `ios` preset reads (the scripts set them).

```bash
ssh <mac-host> '~/build-ragoshell/dev/mac/build-ios.sh ios-ck9 device'      # unsigned compile for the iPad
ssh <mac-host> '~/build-ragoshell/dev/mac/build-ios.sh ios-sim simulator'   # iOS simulator
```

Then open `build/<folder>/RagoShell.xcodeproj` on the Mac screen, pick the iPad (Developer Mode on, trusted) and press
Run: that signs with the personal team and installs (a free team's app expires after 7 days). Why this way:
`codesign` over ssh fails with `errSecInternalComponent` (the key needs the desktop session), so only Run in Xcode
signs; Xcode keeps the folder of a project it has open, so each build gets a new folder name; and the bundle stages
no resources (a nested `Resources` folder in the flat iOS bundle makes codesign say "code object is not signed").

## 4. Testing

| What | How |
|---|---|
| Unit tests (desktop only) | `rago_unit_tests`; tests that need a server use `dev/mongo-test` and skip without one on `localhost:27017` |
| QML loads | `ragoshell --check-qml` (exit 0) |
| Layout of every screen | `dev/ui-shots/geometry.sh` (desktop build at a size, density, language; lists items outside the window and cut button text); `dev/ui-shots/shots.sh` for Android pictures; results in `docs/UiMatrix.md`; read `dev/ui-shots/README.md` first (the test connection) |
| A picture of the desktop build | `ragoshell --window-size 800x1280 --density Touch --lang ar --open-view prefs --snapshot out.png 5000 --exit-after-snapshot` |
| Test server | `dev/mongo-test`: `./seed.sh up`, `./seed.sh seed` (MongoDB 4.0 in Docker). Emulator host `10.0.2.2:27017`; a real device uses `adb reverse tcp:27017 tcp:27017` and host `localhost`, or the PC's address with an inbound firewall rule for 27017 |
| By a person | `docs/ManualTests.md` (section 31 is mostly unticked), `docs/MacOsWork.md` |

`--density Touch` and `--window-size WxH` stand in for a tablet on a desktop. They catch layout failures; touch
itself (long press, keyboard, selection handles) needs a device.

## 5. Debugging on a device

Start options that cannot be typed on a tablet go through the launcher:

```bash
adb shell "am start -n tr.com.ragoshell.app/org.qtproject.qt.android.bindings.QtActivity -e applicationArguments '--lang ja'"
xcrun devicectl device process launch --device <UDID> --terminate-existing --console tr.com.ragoshell.app --lang ja   # on the Mac
```

| Platform | Settings folder and log (`logs/ragoshell.log`) | Live log |
|---|---|---|
| Windows | `%USERPROFILE%\.ragoshell\` | stderr of a console run; Help, Open Log Folder |
| Linux, macOS | `~/.ragoshell/` | stderr |
| Android | `files/` of the app: `adb shell run-as tr.com.ragoshell.app cat files/logs/ragoshell.log` (debug builds) | `adb logcat --pid=$(adb shell pidof tr.com.ragoshell.app)` |
| iPadOS | `Library/Application Support/RagoShell/RagoShell/` in the container; copy with `xcrun devicectl device copy from --domain-type appDataContainer --domain-identifier tr.com.ragoshell.app` | `--console` on `process launch` |

`RAGOSHELL_CONFIG_DIR` moves the settings folder (desktop); `RAGOSHELL_KEYSTORE=file` forces the key file. A test
connection goes into a device's app with `adb push` plus `run-as ... cp` (Git Bash: `MSYS_NO_PATHCONV=1`) or
`devicectl device copy to` (`dev/ui-shots/README.md`).

Things that look like app bugs and are not:

- **A blank white window on Android** (a 9 KB screenshot, an empty log) is the tablet's screen being off or locked:
  the launched app gets no surface. Check `adb shell dumpsys power | grep "Display Power"`, send `KEYCODE_WAKEUP`
  and launch within seconds, or enable "Stay awake" while charging.
- **Tearing and striped buttons on the emulator** come from its software GPU (`-gpu swiftshader_indirect`).
- **LAN connections time out** on Android 17 until the local network permission is granted, and on an iPad while
  the Local Network question is open. A PC address that times out while `localhost` over `adb reverse` works is
  usually the Windows firewall.
- **An old icon on the iPad**: iOS caches it per bundle id and version; delete the app or restart the iPad.

## 6. Open items

Only what is not done, checked against the code.

**Android release.** `ci/build-android.sh`, `ci/build-android.ps1`, `ci/generate-releases-json.mjs`,
`ci/generate-sbom.mjs`, and `ci/gitea-release.*` build, optionally sign (`RAGO_ANDROID_*`,
`docs/AndroidCodeSigning.md`), verify (`apksigner` v2/v3, `jarsigner -verify`), and 16 KB check both the Release `.apk`
and `.aab`. Still open: generating the production Google Play upload keystore outside the repository, configuring CI
secrets, installing and testing the signed Release APK on a physical Android tablet, completing the Google Play Console
listing, Data Safety submission (no developer telemetry collected; the optional AI assistant sends prompts and schema
summaries directly to the endpoint the user chooses, and with a local model the data stays on that machine), and closed testing.

**iOS / iPadOS release.** The App Store needs a decision on publishing a GPLv3 app (the store terms add restrictions the GPL
does not allow); nothing is decided, and running on one's own iPhone or iPad from Xcode does not need it. A store build, an
archive upload with no privacy-manifest warning, and a test on an IPv6-only (NAT64) network, which App Review uses.

**UI on mobile.**
- Touch editing in the code editor (selection handles, pinch zoom) and the keyboard hints of the Scintilla item
  are untried on a device. The Android permission dialog uses "nearby devices" wording and the app does not explain
  first.

**Platform integration.**
- OS memory warnings: on iOS, `PlatformInfo_ios` observes `UIApplicationDidReceiveMemoryWarningNotification` and Darwin low-memory notifications to invoke `Backend.trimMemory()` on the GUI thread (tested on iPad simulator 2026-10-30); Android `onTrimMemory` hook is pending.
- "Share log": only Help, Open Log Folder exists (a folder URL through `QDesktopServices`); there is no share sheet.
- SSH keys and TLS CA, PEM and CRL files picked on mobile are copied into `<appDataDir>/credentials/` (`0600`) via
  `Backend.importCredentialFile` (unit tested); live SSH tunnels and TLS with picked files still need a manual pass on a
  physical device.
- The AI assistant over HTTPS on Android is unverified: Android gives apps no OpenSSL for Qt Network and no
  `libssl`/`libcrypto` is packaged (no `QT_ANDROID_EXTRA_LIBS`).
- Key store on devices: app reinstall and migration of an old `ragoshell.key` are untested on Android; the Keychain
  code is not exercised beyond starting the app (`docs/MacOsWork.md`, A6).
- The metered-network pause, the resume and wake re-checks, and SSH tunnel re-establishment (`SshTunnelWorker`) are unit
  tested, but not yet run on a real metered network, sleeping machine or backgrounded tablet (`docs/ManualTests.md`,
  sections 29 and 31).

**Not looked at** (`docs/UiMatrix.md`, `docs/MacOsWork.md`): a Linux desktop by eye, macOS by hand (Command keys, Dock
icon), iPad Split View, Stage Manager and rotation, Android split window, a real phone, hardware keyboards on the
tablets, screen readers. macOS notarization needs a Developer ID Application certificate (`docs/MacOsWork.md`, A5).
