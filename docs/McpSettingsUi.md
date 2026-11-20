# MCP settings in the app

Until now the read-only MCP server (`docs/McpServer.md`) was switched on and given connections only from the
command line (`ragoshell-mcp --enable --allow …`). A user who registered it in an AI client without that step saw
"Connection closed" and nothing else. This plan moves the same settings into the app. `mcp.json` stays the only
store and the command line keeps working.

## 1. What the user gets

- **Connection dialog, Connection tab**: a check box "Let AI tools read this connection (MCP, read-only)".
  It is disabled, with the reason, for an SSH tunnel and for a client certificate whose passphrase is asked at
  connect time (the server cannot serve those, `McpGuards::unsupportedReason`). A line under it says when the
  server itself is off.
- **Preferences, new section "AI TOOLS (MCP)"**: turn the server on or off, the document cap and the time limit,
  how many connections are exposed, and a ready configuration for Claude Code, Claude Desktop, Cursor,
  Antigravity and VS Code (Copilot), with a Copy button. The configuration holds only the path of
  `ragoshell-mcp` (on a Linux AppImage the AppImage with `--mcp`), never a secret.
- Desktop only. The phone, tablet and browser builds ship no `ragoshell-mcp`, so both controls are hidden there.

## 2. Phases

1. **Core** (`src/ragoshell/core/mcp/`, unit tested)
   - `McpConfig::modify(path, fn)`: read the file, change it, write it back atomically. It refuses to write when
     the existing file cannot be read or parsed.
   - `McpClientSetup`: finds the server binary next to the app and builds the configuration text per client
     (JSON escaping of Windows paths, shell quoting for the `claude mcp add` line).
   - `McpServer::applyConfig()`: applies a changed `mcp.json` to a running server (see threat 3).
2. **Server** (`src/ragoshell-mcp/main.cpp`): before each request, reload `mcp.json` when its time stamp or
   size changed. A file that no longer loads disables the server.
3. **App bridge**: `McpSettingsBridge`, the QML singleton `McpSettings`, over phase 1.
   `ConnectionListModel::create()` returns the new uuid, and `remove()` takes the uuid off the allow-list.
4. **QML**: the check box in `ConnectionDialog.qml`, `PreferencesMcpSection.qml`, translations for all
   15 catalogs.
5. **Docs and checks**: `docs/McpServer.md`, `docs/ManualTests.md` §22, gates, security review of the diff.

## 3. Threats and edge cases

| # | Case | Handling |
|---|---|---|
| 1 | `mcp.json` is unreadable or hand-broken | The app never overwrites it. The controls show the error and stay read-only. The server stays disabled (as today). |
| 2 | The command line and the app write at the same time | Every change is a read-modify-write of the file on disk, never of a copy held in memory. `QSaveFile` replaces the file atomically. |
| 3 | The user unticks a connection or turns the server off while an AI client has it running | The server reloads `mcp.json` before each request: a revoked connection and a disabled server take effect at the next tool call, not at the next client restart. A newly allowed connection needs a restart of the client's server; the text in the dialog says so. |
| 4 | A connection is deleted | Its uuid is removed from the allow-list, so a later connection can never inherit it. A clone gets a new uuid and is not exposed. |
| 5 | An allowed connection is later switched to an SSH tunnel | Already handled by the server (`unavailable` in `list_connections`, refused in tools). The dialog shows the reason and disables the box. |
| 6 | Saving Preferences when nothing MCP-related changed | `mcp.json` is not written. A broken file then cannot make the whole Preferences save fail. |
| 7 | The configuration text leaks something | It contains the server name, the binary path and `--mcp` at most. No connection names, users or passwords. |
| 8 | Turning the server on exposes data | The server switch and the allow-list stay separate. Turning it on with no connection ticked exposes nothing. |
| 9 | Unsafe limits set from the UI | The spin boxes stop at the hard caps, and `McpConfig::normalized()` clamps whatever reaches the file. |

Nothing here opens a port or changes what the server can do; the read-only guards are unchanged.

## 4. Security review (2026-11-04)

A review of the diff found no high-severity issue. These findings were fixed before the change was finished:

| Finding | Fix |
|---|---|
| A failed write (read-only folder, file in use) went unnoticed after the dialog closed | `McpSettings.writeFailed` opens a message ("AI tools may keep their access"), and Preferences shows the error until a write succeeds |
| An untouched check box, or Preferences left open, could undo a `--revoke` / `--disable` made meanwhile | The dialog writes only when the box changed (or the connection became unservable); Preferences writes only the fields the user changed (`updateServer`) |
| The command line and the app could lose each other's update | `McpConfig::modify` holds `mcp.json.lock` across the read-modify-write, and `ragoshell-mcp --enable/--disable/--allow/--revoke` go through it. On Windows the save is retried briefly, in case it collides with the server reading the file |
| A connection deleted by sync stayed on the allow-list | Every write and every connection change prunes uuids of connections that no longer exist (`McpConfig::keepOnly`), and the app does it again at start |
| A Windows folder name with `$`, `` ` `` or `%` would be expanded by a shell when pasting the `claude mcp add` line | No command line is offered for such a path (`McpClientSetup::commandLineSafe`), only the JSON blocks |

One residual: a server that is already running keeps the connection list it read at start. A connection deleted by sync while the server runs stays readable until the AI tool restarts the server, or until the app starts and prunes the allow-list; revoking it in the app takes effect at once.
