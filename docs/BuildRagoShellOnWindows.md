Building RagoShell (Windows)
============================

A. Prerequisites
----------------

- Visual Studio 2022+ (or VS 18) with C++ workload and Windows SDK.
- Qt 6.12.x msvc_64 — set `CMAKE_PREFIX_PATH` via the CMake preset, or
  install to `C:\Qt\6.12.0\msvc2022_64`.
- vcpkg (manifest mode is used; `vcpkg.json` pins
  `mongo-cxx-driver 4.0.0` + `mongo-c-driver/libbson 1.30.7` + OpenSSL + libssh2).
  Presets point at the VS-bundled vcpkg; override `VCPKG_ROOT` in
  CMakePresets.json if yours lives elsewhere.
- Node.js 20.x (mise-managed via `.mise.toml`) — optional: only the translation check
  (`node ci/check-i18n.mjs`) and the MCP end-to-end test use it. The shell needs no Node.js: it runs inside
  the app (docs/Shell.md).

B. Configure & build
--------------------

    cmake --preset windows          # Visual Studio generator
    cmake --build build/windows --config Debug --target ragoshell ragoshell-mcp

Presets: `windows` (VS) and `windows-ninja` (requires a vcvars
environment). The app (`ragoshell.exe`) and the MCP server
(`ragoshell-mcp.exe`) land in `build/windows/src/ragoshell-app/<Config>/`;
windeployqt runs post-build, so they start from there.

C. Runtime layout (packaged)
----------------------------

    ragoshell.exe    (the app; Qt Quick)
    ragoshell-mcp.exe   (read-only MCP server, see docs/McpServer.md)
    Qt6*.dll, qml/, plugins  (windeployqt)
    mongocxx/bsoncxx/mongoc/bson DLLs, OpenSSL 3 DLLs
    licenses/

D. Tests
--------

    build\windows\src\ragoshell-unit-tests\Debug\rago_unit_tests.exe

(Qt's `bin` directory must be on PATH.)

QML checks (no window needed):

    build\windows\src\ragoshell\Debug\ragoshell.exe --check-qml
    cmake --build build/windows --config Debug --target ragoshell_qmllint

E. Packages
-----------

`ci\build-windows.ps1` builds Release, runs the unit tests and
`--check-qml`, and writes the ZIP, NSIS setup.exe and WiX MSI to `dist\`
(see `ci/README.md`).
