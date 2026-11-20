# Building for macOS and Linux from a Windows PC

The macOS build always runs on a Mac: Apple's clang and the macOS SDK only run
there, and neither Qt nor Visual Studio can cross-compile to macOS from Windows.
What a Windows PC can do is drive the Mac and Linux build hosts over SSH. Set
up each host first as described in `docs/BuildRagoShellOnMacAndLinux.md`.

There are two jobs:

- **Develop and debug**: Visual Studio or Qt Creator builds on the remote host.
- **Release packages (dist)**: the `ci/build-*.sh` scripts run on each host,
  triggered over SSH from the PC.

## Visual Studio (remote CMake)

Needs the **Linux and embedded development with C++** workload (Visual Studio
Installer). Visual Studio copies the sources to the host over SSH, runs CMake and
Ninja there, and debugs with gdb on Linux and lldb on macOS.

1. Tools → Options → Cross Platform → Connection Manager → **Add**: host name,
   user, and a password or an SSH key. Add the Mac and the Linux host.
2. In the toolbar pick the remote host as the target system, then the
   `macos-*` or `linux-*` preset.
3. The sources are copied to `~/.vs/<project folder>` on the host. It is a plain copy
   and not a git checkout, so `ci/build-*.sh` cannot run there (the translation
   check calls `git diff`). Use a separate clone for packaging; see below.

Visual Studio decides which presets belong to which target from the `hostOS`
vendor setting. `CMakePresets.json` does not set it yet, so every preset shows
for every target. To filter them, add to `unix-base` (with `["Linux"]` or
`["macOS"]` per platform preset) and give `base` `["Windows"]`:

```json
"vendor": {
  "microsoft.com/VisualStudioSettings/CMake/1.0": { "hostOS": ["macOS"] },
  "microsoft.com/VisualStudioRemoteSettings/CMake/1.0": {
    "sourceDir": "$env{HOME}/.vs/$ms{projectDirName}"
  }
}
```

The remote build reads `VCPKG_ROOT` and `Qt6_DIR` from the host's login
environment, which is why the host setup puts them in `~/.zshenv` (macOS) or
`~/.profile` (Linux).

## Qt Creator

- **On the host itself**: the simplest setup. Qt Creator reads
  `CMakePresets.json`; open the project and pick `macos-release` or
  `linux-release`. From the PC, reach the Mac with Screen Sharing or VNC.
- **From Windows to Linux**: Qt Creator's Remote Linux device can open and
  build a project on a Linux host over SSH. The feature is still experimental.
- **From Windows to a Docker container**: a Docker device (Docker Desktop) can
  stand in for a separate Linux machine.
- macOS is not a supported remote build device; use Qt Creator on the Mac.

Like Visual Studio, Qt Creator only builds. `macdeployqt` and the archive are done by the `ci/` scripts.

## Release packages from the PC

Keep a git clone of the repository on each host, for example `~/src/ragoshell`
(not the Visual Studio copy in `~/.vs`). Then, from the PC, for a version
`1.4.5`:

```powershell
# macOS
ssh <mac-host> "cd ~/src/ragoshell && git pull --ff-only && ./ci/build-macos.sh 1.4.5"
scp "<mac-host>:~/src/ragoshell/dist/ragoshell-1.4.5-macos-arm64.tar.gz" dist/

# Linux
ssh <linux-host> "cd ~/src/ragoshell && git pull --ff-only && ./ci/build-linux.sh 1.4.5"
scp "<linux-host>:~/src/ragoshell/dist/ragoshell-1.4.5-linux-x64.tar.gz" dist/

# Windows, locally
.\ci\build-windows.ps1 1.4.5
```

`<mac-host>` and `<linux-host>` are host aliases from `~/.ssh/config` on the PC:

```
Host <mac-host>
    HostName <mac address>
    User <user>
Host <linux-host>
    HostName <linux address>
    User <user>
```

The scripts find `VCPKG_ROOT` and `Qt6_DIR` through the login environment set up
on each host. The first run on a host builds the vcpkg packages and takes longer.

For a distributed macOS build, sign and notarize on the Mac before uploading
(the commands are in the header of `ci/build-macos.sh`).

## Day to day with the Mac

For the debug loop (not the release packages) the scripts of `dev/mac/` do the sync and the builds: `dev/mac/sync.sh`
sends the working tree (uncommitted edits and new files included) to `~/build-ragoshell` on the Mac, `dev/mac/build-macos.sh`
and `dev/mac/build-ios.sh` build there. Platform build and signing details live in `docs/Platforms.md`.
