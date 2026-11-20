# Read-only MCP server (`ragoshell-mcp`)

`ragoshell-mcp` is an optional, local [Model Context Protocol](https://modelcontextprotocol.io)
server. AI clients such as Claude Code and Claude Desktop can use it to look at
MongoDB through your saved RagoShell connections. It is **off by default**,
exposes **only the connections you allow**, and is **read-only**.

## Quick start

In the app (desktop builds, `docs/McpSettingsUi.md`):

1. **Preferences › AI tools (MCP)**: tick "Let AI tools use the MCP server".
2. Open a connection's settings and tick **"Let AI tools read this connection
   (read-only)"** on the Connection tab. It is disabled, with the reason, for SSH
   tunnels and for a client certificate whose passphrase is asked at connect time.
3. Back in Preferences, pick your AI client under "Add to an AI tool", press
   **Copy** and paste the command or JSON block where the hint says. It
   contains only the path of `ragoshell-mcp`.

The same from the command line:

1. Show the saved connections and the server state:

   ```
   ragoshell-mcp --status
   ```

2. Enable the server and allow one or more connections (name or uuid):

   ```
   ragoshell-mcp --enable --allow "Local Docker"
   ```

3. Register it in your AI client (next section) and restart the client session.

Other commands: `--revoke <name|uuid>` stops exposing a connection, `--disable`
turns the server off and keeps the allow-list, `--help` lists everything.

The app and the command line edit the same `mcp.json` (a read-modify-write of
the file on disk under `mcp.json.lock`, so neither overwrites the other's
change). The running server
rereads it before every request: unticking a connection, `--revoke` or turning
the server off takes effect at the AI tool's next call. A connection allowed
after the client started the server appears after the client restarts it.
Deleting a connection in the app also removes it from the allow-list.

`ragoshell-mcp.exe` sits next to `ragoshell.exe` (package root, or
`build/windows/src/ragoshell-app/<Config>/` in a dev build).

## Register in Claude Code

```
claude mcp add --scope user ragoshell -- "C:\Program Files\RagoShell\ragoshell-mcp.exe"
```

Use your real install path. `--scope user` makes it available in every
project; leave it out to register for the current project only. To expose a
single allowed connection in a given project, pin it:

```
claude mcp add ragoshell-reporting -- "C:\Program Files\RagoShell\ragoshell-mcp.exe" --connection "Reporting"
```

Check it with `claude mcp list` (shows the connection status) or
`/mcp` inside a session. Tool names appear as `mcp__ragoshell__find` etc.;
they are all marked read-only.

## Register in Claude Desktop

Settings → Developer → Edit Config, then add to `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "ragoshell": {
      "command": "C:\\Program Files\\RagoShell\\ragoshell-mcp.exe",
      "args": []
    }
  }
}
```

Restart Claude Desktop.

## Tools

| Tool | What it does |
|---|---|
| `list_connections` | Allowed connections: name, uuid, address, TLS / replica set flags. Never user names, passwords, key paths or passphrases. |
| `list_databases` | Databases with size on disk. Users without the `listDatabases` privilege get the connection's default / auth database. |
| `list_collections` | Collections and views of a database. |
| `collection_schema` | Field paths, BSON types and presence from a `$sample` (default 100, max 1000) — reuses the app's Schema Analysis. Sample values and min/max only with `includeValues: true`. |
| `find` | `filter`, `projection`, `sort`, `skip`, `limit`. Filters may be JSON (Extended JSON) or Mongo Shell text (`{ _id: ObjectId('…') }`). |
| `aggregate` | Read-only pipeline; the server appends a `$limit`. |
| `count` | `countDocuments` with a filter. |
| `explain` | `executionStats` for a find or a pipeline, summarized with the app's explain parser (stages, indexes, docs/keys examined, COLLSCAN / in-memory sort warnings). |

Documents come back as relaxed Extended JSON, so ObjectId, dates, Decimal128
and 64-bit integers keep their exact values. `hasMore: true` means more
documents matched than were returned.

When more than one connection is exposed, tools take a `connection` argument
(name or uuid from `list_connections`).

## Limits and safety

Settings live in `mcp.json` next to `ragoshell.json` (`~/.ragoshell/`):

```json
{
  "enabled": true,
  "allowedConnections": ["{11111111-2222-3333-4444-555555555555}"],
  "maxDocuments": 50,
  "maxTimeMS": 15000,
  "maxResultBytes": 524288
}
```

- `maxDocuments` (default 50, hard cap 1000) bounds `find` / `aggregate` /
  `explain`; a larger `limit` from the client is cut down.
- `maxTimeMS` (default 15 s, hard cap 120 s) is sent with every query,
  aggregation, count, schema sample and explain.
- `maxResultBytes` (default 512 KB) truncates large results with a note.
- `$out` and `$merge` are rejected anywhere in a pipeline (also inside
  `$facet`, `$lookup` and `$unionWith`), as are `$where`, `$function`,
  `$accumulator` and JavaScript code values. There is no generic
  `runCommand` tool, so no write command can be sent.
- The credential collections `system.users` (password hashes) and
  `system.keys` (cluster signing keys) cannot be read, neither directly nor
  through `$lookup`, `$graphLookup` or `$unionWith`.
- A missing `mcp.json` means disabled; an unreadable one keeps the server
  disabled (also when it breaks while the server runs), and the app shows the
  error and never overwrites the file. Hand-edited limits are clamped to the
  hard caps.
- Error messages are scrubbed of the connection's password and PEM passphrase.
- The server only speaks stdio; it opens no network port.

Use a MongoDB user with the `read` role for the connections you expose — the
checks above are a second line of defense, not a replacement for it.

TLS profiles work as in the app (`buildMongoUri` / `buildMongoClientOptions`):
CA file, client PEM with a stored passphrase, CRL, allow invalid certificates
and hostnames. Not supported: SSH tunnel connections and connections that ask
for the PEM passphrase at connect time (`--allow` refuses them and
`list_connections` marks them unavailable).

`RAGOSHELL_CONFIG_DIR` points the server (and the app) at another settings
directory instead of `~/.ragoshell/` — used by the end-to-end test.

## Design

### Transport and host

| Option | For | Against |
|---|---|---|
| `ragoshell --mcp <uuid>` | One executable | The GUI exe is linked for the Windows GUI subsystem and creates a `QApplication`, QML engine and tray icon; stdio pipes from a GUI-subsystem process are fragile, and a crash in GUI code would take down the server |
| **Companion console binary `ragoshell-mcp` (chosen)** | Clean stdio; links `rago-core`, so it reuses `SettingsManager` (decrypted passwords), `buildMongoUri` / `buildMongoClientOptions` (same OpenSSL TLS stack as the app), `SchemaAnalysis` and `ExplainPlanParser` unchanged; small and testable | One more executable to ship (staged next to `ragoshell.exe`, shares its DLLs) |
| A Node.js script (the app had a Node shell helper at the time; it is gone) | Would reuse the `mongodb` 6.16 driver and a bundled `node.exe` | Must re-implement settings parsing, password decryption, URI/TLS mapping, schema analysis and explain parsing in JavaScript — two copies of security-relevant logic that drift apart |

stdio is the transport every MCP client supports, needs no port, and lets the
client own the server's lifetime. Messages are newline-delimited JSON-RPC 2.0;
protocol versions 2025-06-18, 2025-03-26 and 2024-11-05 are accepted.

### Code

- `src/ragoshell/core/mcp/McpServer.*` — JSON-RPC / MCP handling, tool
  definitions, argument parsing, limits and result formatting. Transport-free
  (`handleLine()`), so it is unit tested without pipes.
- `McpGuards.*` — read-only checks (pipelines, filters, names) and the
  secret-free connection summary.
- `McpBackend.*` — the MongoDB side behind an interface; `MongoMcpBackend`
  keeps one mongocxx client per connection.
- `McpConfig.*` — `mcp.json`. It is a separate file because the GUI rewrites
  `ragoshell.json` from memory on every save and would drop unknown keys.
  `McpConfig::modify()` is the read-modify-write the app uses.
- `McpClientSetup.*` — finds the server next to the app (inside a Linux
  AppImage: the AppImage with `--mcp`) and builds the per-client configuration
  text for Preferences.
- `src/ragoshell-app/McpSettingsBridge.*` — the QML singleton `McpSettings`
  behind the check box and the Preferences section.
- `src/ragoshell-mcp/main.cpp` — CLI and the stdio loop. File descriptor 1 is
  redirected to stderr while serving, so library output can never corrupt the
  protocol stream.

## Tests

- Unit tests (`rago_unit_tests --gtest_filter=Mcp*`): config, guards, the
  JSON-RPC layer with a fake backend, and a live test through
  `MongoMcpBackend` against `localhost:27017` (skipped when unreachable).
- Stdio end-to-end test against a local MongoDB (e.g. Docker `mongo:4.0`):

  ```
  node src/ragoshell-mcp/tests/e2e_stdio.js build/windows/src/ragoshell-app/Debug/ragoshell-mcp.exe
  ```

  It uses a temporary settings directory, never your own.
- Manual checks: `docs/ManualTests.md`, section 22.
