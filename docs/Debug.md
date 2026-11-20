# Debugging RagoShell

## Verbose logging

RagoShell writes diagnostic messages to the output log panel, to stderr (when
launched from a terminal) and to a log file. Help → Open Log Folder opens the
folder; the file is `ragoshell.log` in `logs/` under the settings folder
(`~/.ragoshell/logs/` on desktop, or `$RAGOSHELL_CONFIG_DIR/logs/`), with one
older file, `ragoshell.log.1`, kept when it reaches 5 MB. It records `LOG_MSG`
lines, worker log events and every Qt message (`qDebug`, `qWarning`, QML
errors), one line each with time and level. Debug-level worker events and
`debugLog` output are recorded only when debug mode is on.

Qt logging works as usual: `QT_LOGGING_RULES` filters categories, and
`QT_MESSAGE_PATTERN` changes the terminal format (not the file's). On Windows a
command-line run (`--check-qml`, the self-tests, `--snapshot`) prints to
stderr; a normal double-click launch sends Qt messages to the debugger only.

## Debug builds

```bash
# Windows
cmake --preset windows
cmake --build --preset windows-debug
build\windows\src\ragoshell\Debug\ragoshell.exe

# Linux / macOS
cmake --preset linux-debug        # or macos-debug
cmake --build --preset linux-debug
```

Use Visual Studio, `gdb`, or `lldb` against the produced executable. Qt debug
helpers (`qInstallMessageHandler`, `QT_LOGGING_RULES`) work as usual.

## Script drafts

Unsaved script text of shell tabs is written to `drafts.json` in the settings
folder (see "Verbose logging" for where) while a tab has unsaved changes, and
removed when the tab closes or the script is saved. A leftover file means the
last session ended without closing those tabs. Dev helpers:
`--drafts-selftest` (model checks, exit code = failures), `--open-view recover`
(the recovery dialog) and `--recover-drafts [--close-recovered]` (does what
"Open" does for every draft and reports each tab on stderr).

## Script engine / shell backend

The shell runs inside the app on QJSEngine (`src/ragoshell/core/engine/`: `ScriptEngine` ->
`JsShellBackend` -> `shell-runtime.js` + `ShellHost` -> `mongocxx`; docs/Shell.md). When debugging
shell issues:

- `ragoshell --shell-selftest=host:port` runs a fixed set of scripts through `ScriptEngine` against a
  server and prints `SHELLSELFTEST PASS|FAIL` lines (on a tablet: `adb logcat`, on an iPad: `xcrun devicectl
  device process launch --console ...`); no UI is needed.
- `src/ragoshell/core/engine/js/shell-runtime.js` is embedded at build time; edit it and rebuild
  (CMake regenerates the byte array).
- After a timeout, a Stop or a lost connection `ScriptEngine` drops the connection and the next script
  reconnects; look for the `failedScope` flag when a previously working tab stops responding.

## Live integration tests

`ScriptEngineLiveTest` cases in `rago_unit_tests` exercise the full
`ScriptEngine -> JsShellBackend -> MongoDB` chain against a MongoDB 4.0 on `localhost:27017` with a `samples`
database. They skip automatically when no server answers. `ShellCorpus_test.cpp` checks 107 scripts and 52 completion prefixes against the results
recorded in `src/ragoshell-unit-tests/shell-corpus/*.json`.

## Local test data

Any `mongo:4.0` container with sample data on `localhost:27017` will do (the README, "Test data"). The author's docker
setup is `dev/mongo-test/`, which is git-ignored and not part of the repository. The screen checks use the same
server through a test connection: `dev/ui-shots/README.md`.
