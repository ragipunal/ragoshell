# RagoShell QML — Manual Test Checklist

Changes that unit tests and `--check-qml` cannot cover. Tick an item once it
has been verified on a real machine; add the date and any notes.

Binary (Debug, from the repo root):

```
./build/windows/src/ragoshell-app/Debug/ragoshell.exe
```

Settings file: `%USERPROFILE%\.ragoshell\ragoshell.json`.
Log panel: `Ctrl+L` — attach its lines when reporting a failure.

## 1. `mongodb+srv` (Atlas / DNS seedlist)

Prerequisite: an Atlas cluster (free M0 is enough) with a database user and
your IP in Network Access. Copy the URI from Connect → Drivers, e.g.
`mongodb+srv://user:pass@cluster0.xxxxx.mongodb.net/?retryWrites=true&w=majority`.

- [ ] **URI import** — New Connection → paste URI into "From URI" → From URI:
  - Type is **DNS Seedlist (mongodb+srv)**
  - SRV Host shows `cluster0.xxxxx.mongodb.net`, port field is hidden
  - Authentication tab has user/password; TLS / SSL tab has TLS enabled
- [ ] **Test Connection** succeeds
- [ ] **Connect** — after saving, Explorer lists the databases
- [ ] **Shell** — open a shell on a collection, run `db.getCollectionNames()`
      and a `find()`; results appear (the shell connects over SRV on desktop)
- [ ] **Persistence** — restart the app; the connection is still SRV, and
      `ragoshell.json` contains `"isSrv": true` for it
- [ ] **Validation** — choose DNS Seedlist manually and enter `localhost` or
      `host:27017`; Save shows the "SRV host must be a hostname…" error
- [ ] **SSH tab** — with DNS Seedlist selected, SSH fields are disabled

## 2. AltGr input in the editor (Turkish Q keyboard, Windows)

Open a shell tab and type into the editor:

- [ ] AltGr + Q → `@`
- [ ] AltGr + 7 / 8 / 9 / 0 → `{ [ ] }`
- [ ] AltGr + `*` (key right of 0) → `\`
- [ ] AltGr + `-` → `|`
- [ ] AltGr + Ü → `~`
- [ ] AltGr + 3 / 4 → `# $`

Shortcuts must still work and must not insert text:

- [ ] Ctrl+F opens the find bar
- [ ] Ctrl+Space opens autocomplete
- [ ] Ctrl+Shift+C toggles line comment
- [ ] Ctrl+Z / Ctrl+Y undo / redo
- [ ] F5 executes the script

## 3. Tabs (open, close, switch)

Tabs come from a C++ model (`Backend.tabs`) in the order they were opened;
`ragoshell --tabs-selftest` checks the model itself (20 checks).

- [ ] Run a long query (e.g. a large `find()`), close the tab while it is
      busy — no crash, other tabs keep working
- [ ] Close the left / right tabs via the tab context menu with several
      shells open — no crash
- [x] Real keystrokes (2026-10-25): Ctrl+T, Ctrl+Shift+T (duplicate),
      Ctrl+Shift+M (monitor) open tabs that become current; Ctrl+W / Ctrl+F4
      close the current tab and its right neighbour (else the left one)
      becomes current; Ctrl+Tab / Ctrl+Shift+Tab wrap; Alt+N jumps; Ctrl+W on
      Welcome + Ctrl+Shift+W reopens it first; context menu › Close Other Tabs
      keeps only that tab
- [ ] Each tab keeps its colour while others open and close; a busy (⟳) or
      modified (●) shell shows it on its tab
- [ ] Closing a modified shell (single, others, to the right, window) asks
      first and lists the modified tabs
- [ ] Ctrl+S, Ctrl+O, Ctrl+N and F11 work (they were listed twice next to
      their StandardKey before and never fired on Windows)

## 4. Shell editor modified marker (Scintilla save points)

- [ ] Open a shell tab: no modified marker on the tab
- [ ] Type a character → marker appears; Ctrl+Z back to the original →
      marker disappears
- [ ] Ctrl+S → marker clears; typing again sets it
- [ ] Explorer actions that prefill a script (e.g. View Documents,
      getIndexes) open with no marker
- [ ] Close a modified tab → "unsaved changes" confirmation appears

## 5. Lazy dialogs (created on first open)

Each dialog opens correctly the first time and on reopen, with its fields
reset / prefilled as before:

- [ ] New Connection / Edit Connection, Preferences, About, Shortcuts
- [ ] Explorer: Insert Document, Create Collection (standard / view /
      time-series / clustered), Add / Edit Index, Add / Edit User,
      Add / Edit Function, Import / Export
- [ ] Server Monitor: inspect an operation (document viewer)

## 6. Server Monitor auto-refresh pausing

- [ ] Open a monitor tab: charts update every ~2 s
- [ ] Switch to another tab for 30 s, come back: no request spike in the
      Log panel while hidden; on return it refreshes immediately and rates
      are sensible (no huge first value)
- [ ] Minimize the window (or hide to tray) and restore: same behaviour
- [ ] Close a monitor tab / quit the app while the server is unreachable:
      closes within ~15 s, no hang

## 7. Result views

- [ ] Run a query, re-run it several times quickly, switch Tree (F2) /
      Table (F3) / Text (F4) between runs — no crash, no stale rows
- [ ] Load more / paging still works in Tree and Table

## 8. Top bar and command palette

There is no menu bar any more (docs/DesignSystem.md, section 6): the top bar has the search field, New
Connection, Monitor, a theme button and a ⋮ menu; every former menu command is in the command
palette (Ctrl+K), grouped under the old menu names.

- [ ] The top bar and the ⋮ menu follow the theme (Light and Dark); the theme button switches
      System / Light / Dark
- [ ] Each palette command does the same as its shortcut; commands needing a shell tab are dimmed
      with a reason on the Welcome tab, New Shell Tab / Server Monitor are dimmed with no connection
- [ ] Pressing a shortcut runs the action once (not twice)
- [x] macOS: Cmd+Q, Cmd+, and Cmd+K work with no menu bar (2026-10-30)

## 9. Preferences additions

- [ ] **Default result view** = Table → new shell tabs open in Table; a
      restart keeps the choice
- [ ] **Shell timeout** changed while a shell tab is open → a query in that
      tab honours the new timeout without reconnecting
- [ ] Preferences dialog fits a small window (e.g. 1280×720) and scrolls

## 10. Connection: read preference and X.509

- [ ] Replica set with Read Preference = Secondary Preferred: queries
      succeed; `db.getMongo().getReadPrefMode()` (or server logs) show the
      mode
- [ ] X.509: TLS on + PEM key file, Mechanism = MONGODB-X509, password and
      Auth DB fields are disabled, hint is shown; Test Connection and a
      shell query succeed
- [ ] From URI with `authMechanism=MONGODB-X509&authSource=$external`
      imports correctly

## 11. Startup script (Preferences)

The old "Load ~/.mongorc.js" switch is gone; Preferences has a "Startup script" box instead.

- [ ] Put `function hello() { return "rc ok" }` into the Startup script box, save, open a connection,
      run `hello()` in a new shell tab → `rc ok`; the next query still knows `hello`
- [ ] Empty box → `hello()` is not defined
- [ ] A syntax error in the box logs "startup script not loaded" as a warning and queries still run
- [ ] An existing settings file with "Load ~/.mongorc.js" switched on and a `~/.mongorc.js` file:
      the first start copies the file into the Startup script box (once)
- [ ] Unit tests `ScriptEngineLiveTest.startup_script_*` pass (need the dev MongoDB)

## 12. Window geometry

- [ ] Move/resize the window, quit, start again → same size and position
- [ ] Quit while maximized → starts maximized; un-maximize returns to the
      previous normal size
- [ ] Quit on a second monitor, disconnect it, start → window opens
      centered on an available screen
- [ ] Drag the explorer / work-area splitter and the log panel splitter
      (Ctrl+L shows the log): sizes, log visibility and explorer visibility
      (Ctrl+B) are the same after a restart (registry
      `HKCU\Software\RagoShell\RagoShell\QmlLayout`; `--snapshot` runs
      neither read nor write it)
- [x] Save / restore round trip (2026-10-25): log shown, window closed →
      `logVisible=true` and both split states saved; next start without the
      log view restores it and saves it again
- [ ] Log panel ⧉ opens the log in its own window (move it to a second
      monitor); ⇲ in that window docks it back; Ctrl+L and the toolbar Log
      button show / hide whichever is active; ✕ closes it
- [x] Detached log (2026-10-25): closing the main window also ends the app
      while the log window is open; `logDetached` and the window geometry
      are saved and the next start opens the separate log window again

## 13. Editor horizontal scrolling

- [ ] Type a line longer than the editor → horizontal scrollbar appears;
      dragging it scrolls; shortening the line hides it
- [ ] Shift + mouse wheel and touchpad sideways swipe scroll horizontally
- [ ] Caret moving past the right edge (End key) scrolls the view

## 14. Script save

- [ ] Ctrl+S on a new tab → Save As dialog → file is written
- [ ] Ctrl+S again overwrites; the tab's modified marker clears
- [ ] Saving to a read-only location fails without truncating an existing
      file, and the Log panel shows "Cannot save …"

## 15. Query history and snippets (Ctrl+H)

Stored in `%USERPROFILE%\.ragoshell\query_history.json`.

- [ ] Run a few queries (F5, Query Bar, double-click a collection) →
      Shell › Query History lists them newest first with database,
      connection, time, document count and duration; a failing query shows
      its error in red
- [ ] Running the same script again moves it to the top instead of adding
      a duplicate; paging (Load more / next page) adds nothing
- [ ] Search box filters by script text, snippet name and database
- [ ] ☆ on an entry makes it a snippet; it appears under "Snippets"; ✎
      renames it; Clear History keeps snippets
- [ ] Shell › Save Script as Snippet… saves the current editor script
      under the given name
- [ ] Load replaces the editor text (tab becomes modified, Ctrl+Z
      restores); Run also executes it; New Tab opens it on the entry's
      connection (or the current one if that is not connected)
- [ ] History survives a restart

## 16. Explain plan view

- [ ] `db.restaurants.find({borough: 'Brooklyn'}).sort({name: 1}).explain('executionStats')`
      opens the Explain view automatically: returned / examined counters,
      "No index used", COLLSCAN + SORT warnings, indented stage tree
- [ ] Query Bar › Explain gives the same view
- [ ] With an index (e.g. `db.zips.createIndex({state: 1})` on a test db)
      the view shows IXSCAN and the index name without the COLLSCAN warning
- [ ] `.explain()` (planner only) shows the plan with "—" counters and the
      "Planner output only" hint
- [ ] Aggregate `.explain()` shows the `$cursor` plan
- [ ] Running a normal query afterwards returns to the preferred view
      (Tree/Table/Text); Tree/Table/JSON buttons still show the raw explain
      document

## 17. Aggregation pipeline builder

Open from the Explorer (collection › Aggregation Builder…), Tools ›
Aggregation Builder… or Ctrl+Shift+A (uses the current shell tab's
collection).

- [ ] Starts with one `$match` stage and previews the first documents of
      the collection
- [ ] Add / delete / move stages; picking another operator fills in its
      template (a body you edited is kept)
- [ ] Clicking a stage (or its editor) previews that stage's output; edits
      to earlier stages refresh it after ~0.5 s; the Sample size limits it
- [ ] Unchecking a stage excludes it from the preview and from the script
- [ ] An invalid body turns the card red with the parse error; the preview
      names the failing stage
- [ ] `$out` / `$merge` are not previewed ("writes data" message)
- [ ] A slow or failing pipeline shows the server error (10 s limit) and
      never blocks the shell tabs
- [ ] Copy Pipeline puts `db.getCollection('…').aggregate([...])` on the
      clipboard; Open in Shell / Run in Shell open it in a new tab on the
      same connection and database
- [ ] Works through an SSH tunnel and with TLS connections

## 18. Schema analysis

Open from the Explorer (collection › Analyze Schema…) or Tools › Schema
Analysis… (current shell tab's collection).

- [ ] Reads every document of a collection with up to 5,000 documents;
      larger ones: 1000 by default, 30% the newest (ObjectId `_id`) and the
      rest random (`$sample`); lists every field path; embedded fields
      (`address.city`) and fields of documents inside arrays (`grades.score`)
      are indented under their parent
- [ ] Presence bar and % per field; type mix ("Int32 88% · Null 12%"; rare
      types show "<1%"); arrays also show their element types
- [ ] Numbers and dates show min / max; other scalars show the most
      frequent values with counts ("…" when there are more than 50); a string
      field with few repeating values (status, borough) lists all of them
- [ ] The result is stored in the schema catalog
      (`~/.ragoshell/schema/<connection>/<database>.json`) unless the catalog
      already holds a larger sample
- [ ] Changing Sample and pressing Analyze re-samples; a large sample on a
      big collection stays responsive (30 s limit) and never blocks shells
- [ ] Clicking a field name copies its path
- [ ] An unreachable server or missing permissions shows the error

## 19. Export query to language (Ctrl+Shift+E)

Shell › Export Query to Language… uses the editor selection, or the whole
script when nothing is selected; the Aggregation Builder has "Export Code".

- [ ] A `find(...)` chain with projection / sort / skip / limit and an
      `aggregate([...])` statement both export; anything else shows the
      "single find(...) chain or aggregate([...])" message
- [ ] Python / Node.js / Java / C# switch instantly; the choice is kept
      for the next export; Copy Code copies it
- [ ] BSON types survive: `ObjectId(...)`, `NumberLong(...)`,
      `NumberDecimal(...)`, `ISODate(...)`, `/regex/i`, UUID/BinData
- [ ] The connection string is the tab's server; a password is replaced by
      `<password>`, app-only options (appname, timeouts) are dropped
- [x] Generated code runs with the real drivers (2026-10-25, find + aggregate
      on MongoDB 4.0.28, results identical to the mongo shell): Node.js
      `mongodb` 5.9.2, PyMongo 4.10.1, Java sync driver 5.1.4, C#
      MongoDB.Driver 2.30.0 (3.13 also compiles the code). Note: PyMongo
      4.19 and MongoDB.Driver 3.13 refuse servers older than MongoDB 4.4.
- [ ] Regex literals now also work in the Query Bar filter and in pipeline
      stages (`{ city: /^BRO/i }`)

## 20. TLS with a self-signed certificate (OpenSSL on Windows)

The C driver is built with OpenSSL on Windows (vcpkg `mongo-c-driver[openssl]`,
`libssl-3-x64.dll` / `libcrypto-3-x64.dll` next to the exe) and the shell runs on
the same `mongocxx` / mongo-c-driver (OpenSSL) as the rest of the app, MongoDB 4.0 included.

- [x] Self-signed `CA:TRUE` server certificate, client PEM, Allow invalid
      certificates + Allow invalid hostnames, replica set, no CA file
      (2026-10-25, MongoDB 4.0.28 `requireSSL` and 8.0.34 `requireTLS`):
      Explorer, shell, Schema Analysis and Aggregation Builder all work on
      both servers. With SChannel the client certificate was rejected by
      MongoDB 8.0 (`0x80090331`); with OpenSSL it is accepted.
- [x] Controls are refused: Allow invalid certificates off without a CA file
      ("certificate verify failed: self-signed certificate"), and no client
      certificate when the server requires one.
- [ ] Production replica set with the same profile: Test Connection,
      Explorer, a shell `find()`, Schema Analysis
- [ ] A connection with a CA file (Allow invalid certificates off) and a
      public CA (e.g. Atlas, section 1) still validates
- [ ] `db.coll.findOneAndUpdate(...)` shows the document itself (as the
      mongo shell does), `returnNewDocument: true` returns the new version

## 21. AI query assistant (Ctrl+Shift+G)

Optional and off by default. Preferences › AI Query Assistant: enable it, pick
a provider (Anthropic Claude or Google Gemini), enter its API key (stored
encrypted; `ANTHROPIC_API_KEY`, or `GEMINI_API_KEY` / `GOOGLE_API_KEY`, is used
when the field is empty) and a model (⟳ loads the provider's current list).
Open from Tools › AI Query Assistant… (current shell tab's collection) or the
Explorer (collection › Ask AI for a Query…).

- [x] Against a local mock of the Messages API (2026-10-25, MongoDB 4.0.28):
      the request carries `anthropic-version: 2026-10-21`, structured output
      and `fallbacks: "default"`; the body holds only the request and the
      sampled field paths / types / presence (no document values); the reply
      fills the editor; a 401 shows "The API key was rejected…"; a `$out`
      answer shows the warning and disables Run; with no key nothing is sent
- [ ] With a real API key: a few requests on a test collection (filter +
      sort, date range, group/count) give queries that run in the shell
- [ ] "Data sent" shows exactly the collection description that is sent;
      turning on "Also send frequent values…" adds values and min/max ranges
      (a category lists all its values, "values are UPPERCASE" when they are)
- [ ] First use on a collection samples it (1000 documents, all up to 5,000)
      and writes `~/.ragoshell/schema/<connection>/<database>.json`; opening
      the assistant on it again answers without sampling; values are written
      only while "Also send frequent values…" is on, and turning it on later
      samples again
- [ ] A snapshot older than 7 days, or a collection whose size changed by
      more than 25%, is refreshed in the background while the stored one is
      used
- [ ] Copy / Export Code / Open in Shell / Run in Shell use the tab's
      connection and database
- [ ] Saving Preferences writes `aiAssistant.apiKeyEncrypted` (never the
      plain key) to `ragoshell.json`; the key survives a restart
- [ ] No network: "Cannot reach the API…" with Retry
- [ ] Gemini with a Google AI Studio key: ⟳ lists current `gemini-*` models
      (no embedding / TTS / image models); a request gives a query
- [x] Gemini quota (2026-10-25, mock `RAGOSHELL_AI_BASE_URL` returning 429
      `RESOURCE_EXHAUSTED` with a `…PerDay…PerModel…` quotaId for
      gemini-4-argon): a warning "<model>: quota used up until <time>" and
      gemini-flash-latest answers ("Answered by … instead of …")
- [ ] The next Generate skips the exhausted model without asking it
- [x] Per-minute 429 with `RetryInfo` on every model (2026-10-25, mock):
      three models tried, then "Retry in N s" counting down
- [ ] All models of one provider out of quota and a key for the other
      provider saved: "Send to <other provider> instead" asks it once,
      Preferences stay unchanged
- [ ] A retired model (404): warning, fallback to the next model; after ⟳
      Preferences flags the saved model as no longer listed
- [ ] "Try another model…" off: a quota error stops at the selected model
- [ ] Saving writes `aiAssistant.provider`, `geminiApiKeyEncrypted` and
      `geminiAvailableModels`; an old config with only `apiKeyEncrypted`
      still uses its Anthropic key

## 22. Read-only MCP server (`ragoshell-mcp`)

Setup and design: `docs/McpServer.md`. Binary (Debug):
`./build/windows/src/ragoshell-app/Debug/ragoshell-mcp.exe`.

- [x] Stdio end-to-end against Docker `mongo:4.0` (2026-10-25,
      `src/ragoshell-mcp/tests/e2e_stdio.js`, 18 checks): refuses to start
      until enabled, `--enable --allow`, initialize / tools/list, list
      collections, `find` capped at maxDocuments with `hasMore`, count,
      aggregate, `$out` rejected and nothing written, schema, explain
      (COLLSCAN), only JSON-RPC on stdout, clean exit on EOF
- [x] Claude Code 2.1 as the client (`claude -p --mcp-config`, 2026-10-25):
      server connected, `list_connections`, `list_collections` and a
      `find` with `limit: 5` returned 5 documents with `hasMore: true`
- [x] TLS profile on MongoDB 4.0.28 `requireSSL` (2026-10-25, throwaway
      container): self-signed server certificate with a mismatched name,
      client PEM, Allow invalid certificates + hostnames, no CA file —
      `list_collections` and `find` work; without the client PEM the
      connection is refused
- [ ] `ragoshell-mcp --status` lists the saved connections; SSH-tunnel
      connections are marked unsupported and `--allow` refuses them
- [ ] Registered with `claude mcp add --scope user ragoshell -- <path>`:
      `claude mcp list` shows it connected; `/mcp` lists 8 read-only tools
- [ ] Claude Desktop (`claude_desktop_config.json`) lists the tools and can
      run `list_collections`
- [ ] Production replica set (TLS profile of section 20) allowed:
      `list_databases`, `collection_schema` and a `find` with a filter
- [ ] A connection that is not allowed is not listed and cannot be named in
      a tool call; `--disable` makes the server refuse to start
- [ ] A slow query (large unindexed sort on a big collection) stops at
      maxTimeMS with an error, not a hang
- [ ] Saving settings in the app does not change `mcp.json` (unless an MCP
      setting in Preferences or a connection's MCP check box changed)
- [x] Settings in the app (2026-11-04, `docs/McpSettingsUi.md`): the check box
      on the Connection tab and Preferences › AI tools (MCP) render in Turkish,
      Arabic (right to left; commands stay left to right) and German at 480 px
      (`--open-view conn`, `--open-view prefs-mcp`)
- [x] Live revocation (2026-11-04, MongoDB 8.0): with a client connected,
      `--revoke` refuses the next `list_databases`, the connection leaves
      `list_connections`, `--allow` serves it again, `--disable` refuses with
      "turned off"; the stdio end-to-end test still passes
- [ ] In the app: tick a connection, Copy the Claude Code command, run it,
      `claude mcp list` shows it connected; untick it while a Claude session
      uses it and the next tool call is refused
- [ ] Delete an allowed connection: its uuid leaves `mcp.json`; a clone of an
      allowed connection is not allowed
- [ ] A hand-broken `mcp.json`: the dialog and Preferences show the error, the
      check boxes are disabled and the file is left unchanged

## 23. Input methods (Chinese / Japanese / Korean IME, Windows)

The editor shows an IME composition inline (underlined, outside the undo
history) and places the candidate window at the caret. Add a language with
an IME in Windows Settings › Time & language › Language (e.g. Japanese
Microsoft IME), switch to it with Win+Space and type into a shell tab.

- [x] Scripted checks (2026-10-25): `ragoshell --ime-selftest` sends
      QInputMethodEvents (compose, commit, cancel, Korean-style replacement,
      commit over a selection, focus loss, click, read-only, emoji) — 27/27
      pass; real keystrokes incl. AltGr `{ [ @ ] } \ |` still type normally
      and `db.` still opens autocomplete
- [ ] Japanese: `nihon` shows にほん underlined at the caret, the candidate
      window opens next to it, Space converts, Enter commits 日本; one Ctrl+Z
      removes the committed text
- [ ] Esc while composing removes the composition
- [ ] Chinese (Microsoft Pinyin): `zhongwen` + Space → 中文
- [ ] Korean: syllables combine while typing (한글)
- [ ] Clicking elsewhere or switching tabs while composing keeps the text
      (no duplicate insert later)
- [ ] Autocomplete does not open from uncommitted text; it still opens on
      `.` after the commit
- [ ] Read-only result editors (JSON view) do not start a composition

## 24. Call tips in the shell editor

Typing `(` or `,` inside a known shell call shows its signature above the
call with the current argument highlighted (collection, cursor, `db`, `rs`
methods and BSON constructors such as `ObjectId(`).

- [x] Scripted (2026-10-25): `ShellSignatures` unit tests (5) and
      `ragoshell --calltip-selftest` (8 checks: active argument, chained
      cursor methods, '(' position with UTF-8 text, no tip inside strings or
      after the call); `--shell … --script "db.items.find({ n: 1 }, " --calltip`
      renders `find(filter, **projection**, options)` at the call
- [ ] Typing `db.orders.find({ a: 1 }, ` shows the tip, `projection` bold;
      `)` closes it; Esc closes it; moving the caret out of the call hides it
- [ ] Nested calls: inside `ObjectId(` the tip switches to ObjectId and back
      to the outer call after `)`
- [ ] The tip does not cover the autocomplete list and hides while an IME
      composition is active

## 25. Languages (docs/Localization.md)

Preferences › Language lists "System default" and 15 languages by their own
name (plus the English name). A change shows at once; Cancel restores the
old language. `--lang <code>` overrides the stored choice for one run and
`--switch-lang <code>` switches 2.5 s after start (neither stores anything).

- [x] Scripted (2026-10-26): `LanguageCatalog` unit tests (12: 15 languages,
      only Arabic right to left, OS-language matching incl. `zh-Hant`,
      `az-Arab`, `kk-Arab` not matching, Latin digits in every locale,
      Turkish `index`/`INDEX` casing); `ragoshell --check-qml --lang
      en|ar|ja|tr` load without QML errors; `ci/check-i18n.mjs` passes (15
      files, placeholders and plural forms)
- [x] Screenshots (2026-10-26, `--snapshot`): ru, ja, hi, ar and tr main
      window with a connected shell tab, Preferences and the connection,
      collection and index dialogs — no cut-off text, no missing glyphs
      (Japanese, Hindi, Arabic); live switch en → ar and ar → tr
      (`--switch-lang`) re-translates everything, flips the layout and moves
      the explorer
- [ ] Choose each language in Preferences › Language, Save, restart: the
      language is kept; "System default (…)" follows the OS language
- [ ] Cancel after previewing a language restores the previous one, also the
      choices made in the other combo boxes of the dialog
- [ ] Pseudo-locale (README › Translating): no plain-English text left in the
      dialogs of this checklist, no cut-off `]`
- [ ] Russian: counts read "1 документ / 2 документа / 5 документов"; Arabic
      has its six forms ("لا مستندات", "مستند واحد", "مستندان", …)
- [ ] Turkish and Azerbaijani: İ/ı keep their dots in labels, and nothing
      typed in the editor changes case (`index`, `ID`)
- [ ] Arabic: the whole window is mirrored, the explorer is on the right;
      the code editor, JSON results, host/port/URI fields stay left to right;
      arrows (tree, tab, paging) point the other way; a database name inside
      an Arabic sentence is not reordered; check that the `?` marks, brackets
      and `…` around names read correctly
- [ ] Arabic: server monitor charts still run time from left to right
- [ ] Japanese / Chinese / Hindi: IME input in the editor (`--ime-selftest
      --lang ja`), Han characters use the Japanese or Chinese glyph shapes
      of the chosen language
- [ ] Linux: install `fonts-noto-cjk`, `fonts-noto-core` (Devanagari, Arabic)
      and check the same languages

## 26. Schema intelligence (docs/SchemaIntelligence.md)

Background schema collection, field-name autocomplete and related collections
for the AI assistant. A test server: `dev/mongo-test` (seeded `samples`
database); for the join checks add two related collections, for example
`customers { _id, name }` and `orders { customerId, total }`. The catalog is
`~/.ragoshell/schema/<connection uuid>/<database>.json`.

- [x] Scripted (2026-10-27): `SchemaService` (11), `SchemaCollector` (9),
      `FieldCompletion` (5) and the related-collection `AiQueryAssistant`
      tests pass on Windows and Ubuntu 24.04; the live `SchemaCollector`
      test skips without `localhost:27017`
- [ ] **Background collection (Databases in use, default):** expanding a
      database lists it once and analyzes its collections one at a time,
      smallest first, at most "Collections per database"; the Log panel shows
      "… collections queued" and "… finished"; system collections, views and
      `local` / `config` are left out
- [ ] **Tree:** a dot after analyzed collections (dim when older than 7
      days), the tooltip "Schema: N documents sampled, <time>"; `◷ 3/12` on
      the database while jobs run
- [ ] **Shell:** switching a shell to another database (`use x`) puts it in
      use; typing `db.orders.` analyzes `orders` first if it has no snapshot
- [ ] **Off:** Preferences › Schema analysis › Background analysis: Off; expanding
      databases analyzes nothing, the Schema dialog and the AI assistant still
      sample on request
- [ ] **All databases:** connecting lists and analyzes every database (still
      one job at a time)
- [ ] **Per connection:** the connection dialog's "Schema analysis" overrides
      Preferences; "As in Preferences" follows them; the choice survives a
      restart (`schemaCollectionMode` in `ragoshell.json`, absent for "As in
      Preferences")
- [ ] **Explicit:** database › Analyze Schema of All Collections queues every
      collection, also with Off and beyond the limit
- [ ] **Permissions:** a user without `find` on a database gets one warning
      in the Log panel and no retries; the Schema dialog still shows the error
- [ ] **Disconnect** stops the queue (the `◷` disappears); reconnecting
      collects again
- [ ] **Values:** with "Also send frequent values…" off the catalog files hold
      no `topValues` / ranges
- [ ] **Field autocomplete:** on an analyzed collection the list opens with
      paths, type and presence ("String · 98%") in `db.orders.find({ ▮`,
      after `,` in the filter, in `.sort({ ▮`, in `{ $set: { ▮` of an update,
      in `$match: { ▮`, in `"$▮` inside `$group` / `$project`, in
      `distinct("▮`; `"address.▮` lists the sub-fields; Enter inserts
      `"address.city"` (quoted) or `$status`; rare fields are at the end
- [ ] No list in values (`{ status: ▮`), on operators (`{ $▮` shows the
      shell's operators), in strings and comments; typing `:` closes it;
      Ctrl+Space and the `.` completions of the shell still work
- [ ] A collection without a snapshot: no field list the first time, the
      collection is analyzed, the next `{` shows its fields
- [ ] **AI with related collections:** with `customers` and `orders`
      analyzed, "orders with their customer name" on `orders` gives a
      `$lookup` from `customers`; "Data sent" lists the related collections
      (field names and types, no values) under "Related collections…"
- [ ] A model answer joining a collection that was not described is
      rejected ("… was not described to the model")
- [ ] With part of the database analyzed the dialog shows "N of M
      collections … analyzed" and Analyze the Rest; after it, the related
      list in "Data sent" grows
- [ ] **Forget Schema Data** (connection menu) empties the catalog folder of
      the connection and the tree dots; a connected server collects again
      when its databases are used; removing a saved connection removes its
      catalog folder

## 27. Recovering unsaved scripts (drafts)

Use a throw-away settings folder: `RAGOSHELL_CONFIG_DIR=C:\temp\ragotest`.

- [ ] Open a shell tab on a connected server, type in the editor: a second
      after you stop, `drafts.json` exists in the settings folder with the
      script, server, database and title
- [ ] Save the script (Ctrl+S): the draft is gone from the file at once;
      type again and it comes back
- [ ] Delete all the text of a modified tab: the draft is removed
- [ ] Close a modified tab ("Discard & Close"): its draft is removed
- [ ] Exit with modified tabs ("Discard & Exit"): `drafts.json` is gone
- [ ] End the process with modified tabs open (Task Manager → End task):
      the next start shows "Recover Unsaved Scripts" listing them, and
      nothing is connected yet (the server tree shows no connection)
- [ ] **Open** on a draft whose connection is closed connects, then opens a
      tab with the script, the database and the unsaved marker; the row
      leaves the list; the tab's draft is kept (end the process again and it
      comes back)
- [ ] **Open** on a draft whose connection is already open opens the tab
      at once; two drafts of the same server connect only once
- [ ] A draft of a deleted connection: Open reports that the connection no
      longer exists; Copy puts the script on the clipboard
- [ ] A connection that cannot connect: the error is shown, no tab opens, and
      connecting it by hand later does not open the script by surprise
- [ ] A draft of a script file: the recovered tab keeps the file path, and
      Ctrl+S writes back to that file
- [ ] Close without acting: File → Recover Unsaved Scripts opens the list
      again (disabled when empty); Discard All asks first
- [ ] Languages: the dialog and the menu entry follow the UI language

## 28. Key store and SSH host keys (docs/Platforms.md)

Use a throw-away settings folder (`RAGOSHELL_CONFIG_DIR`) except for the first
item, which needs the real profile.

- [x] **Migration** (real profile, once): after the first start of the new
      build `ragoshell.key` is gone from the settings folder, saved
      passwords still work, and the key is listed in Credential Manager
      (Windows) / Keychain Access (macOS) as "RagoShell" (2026-10-30)
- [ ] Preferences → Saved passwords names the key store; with
      `RAGOSHELL_KEYSTORE=file` it shows the warning and `ragoshell.key`
      is created
- [ ] Linux without a Secret Service (a bare session): falls back to the
      key file with the warning; with GNOME Keyring or KWallet unlocked the
      key goes there
- [ ] An old `ragoshell.key` next to an existing key in the store with a
      different value: the store's key is used, the file stays, the log warns
- [ ] AI assistant: under the API key field the text follows what is set
      (key typed, environment variable, none)
- [ ] **SSH, first connection:** a dialog shows the server, key type and
      SHA256 fingerprint; it matches `ssh-keygen -lf` on the server; Cancel
      (or Escape or ✕) refuses and nothing is saved in `ssh/known_hosts`
- [ ] Trust & Connect saves the key; the next connection asks nothing
- [ ] Reinstall the server's host key (or point the name at another server):
      "SSH Host Key Changed" in red, Refuse keeps the old entry, Trust New
      Key replaces it
- [ ] Two SSH connections started together show their questions one after
      the other
- [ ] Wrong password or key after trusting the host: the usual
      authentication error (the host key check runs before credentials are sent)
- [ ] Connection test (Connection dialog) with SSH shows the same question

## 29. Large transfers, page size and network changes (docs/Platforms.md)

- [ ] Import a 500 MB NDJSON file (or a CSV with a quoted multi-line cell
      every few rows) into a collection: the app's memory stays near a
      few hundred MB in Task Manager, progress counts up, Cancel stops it
- [ ] Import with "Drop before import" and a file whose last line is broken:
      the error appears and the collection is unchanged
- [ ] Export a 1M-document collection as CSV: memory stays flat; the file has
      one header with every field of every document
- [ ] Preferences: the page size cannot be set above 1000; a
      `cursor.batchSize(5000)` script pages 1000 at a time
- [ ] Turn on "Wait while the connection is metered", then mark the network as
      metered (Windows: Settings → Network → the connection → Metered
      connection): a database opened afterwards is not analyzed (log: no
      "queued" line) until the connection is unmetered again, then it is
- [ ] Turn the switch off: analysis runs on the metered connection too
- [ ] Put the laptop to sleep with a direct connection open and wake it:
      within a few seconds the status bar says it is checking the
      connections and the tabs work without a dialog
- [ ] Same with an SSH tunnel open: no error dialog; the status bar reports
      that the connection was lost; connecting again works
- [ ] Unplug the network cable for a minute and plug it back: same check runs
      once the network is back

## 30. Touch and narrow windows (docs/Platforms.md, docs/DesignSystem.md)

Needs a touchscreen (a Windows touch laptop or Surface, later a tablet).

- [ ] Preferences → Interface density: Touch makes buttons, fields, combo
      rows, menu items and icon buttons at least 44 px tall; Compact looks as
      before; Automatic is Compact on a desktop
- [ ] Resize the window to 480 px wide: the explorer folds away, Monitor, Log
      and Preferences leave the toolbar (File → Preferences still works),
      the Welcome page is not clipped; widen it again and the explorer comes back
- [ ] Close the explorer yourself, narrow and widen the window: it stays closed
- [ ] Quit while narrow with the explorer open: the next start shows it
- [ ] Press and hold on a result row (tree and table) with a finger: the same
      menu as the right button of a mouse; a long mouse press does nothing
- [ ] On-screen keyboard (Windows touch keyboard): host, user, database and
      collection fields start lowercase without suggestions; port and similar
      numeric fields show digits; password fields do not learn the text
- [ ] F10 and Alt+Shift+O both toggle the editor / results split
- [ ] A saved connection with GSSAPI opens and saves unchanged on a build that
      cannot offer GSSAPI (phones and tablets): the mechanism stays in the list

## 31. Tablets: Android and iPad (docs/Platforms.md)

Run on an Android tablet, the Android emulator and an iPad (Split View and Slide Over; Stage Manager only on an
M-series iPad or its simulator; docs/MacOsWork.md has the Mac and iPad steps, docs/UiMatrix.md what was looked at).
Items that need a MongoDB server use a test server on the host: `10.0.2.2:27017` from the emulator, the LAN
address from a device.

- [ ] The app installs and starts full screen inside the safe area (no content under the status bar,
      the camera cut-out, the home indicator or the navigation bar); the density is Touch
- [x] Android 17 emulator (2026-10-28, `Small_Tablet`, x86_64 debug APK): starts, creates the log, the
      settings file and an Android Keystore entry (no key file); Test Connection to `10.0.2.2:27017`
      first shows the system "nearby devices" permission dialog, then reports "Connected (MongoDB 4)";
      the saved connection lists the 20 databases. Not checked there: everything else in this section
- [ ] First connection to a LAN address on Android 17: the permission dialog appears once; answering
      "Don't allow" lets the connection fail with a timeout and the log says the permission was not granted
- [x] Command palette (2026-10-28, Windows snapshots, Android 17 emulator): Ctrl+K opens it, typing "pref"
      lists Preferences, Enter runs it; on a tablet it fits above the on-screen keyboard. Still to check
      by hand: Ctrl+Shift+P and Cmd+K, a saved connection and a loaded collection as search results
      (Enter connects / opens a shell), Recent after using commands, Arabic and Japanese titles,
      tapping a row with a finger
- [x] New top bar (2026-10-28, Windows snapshots, Android 17 emulator): one row, no menu bar; the search field
      opens the palette; ⋮ lists Log Panel, Preferences, Keyboard Shortcuts, Open Log Folder, About
      (plus Monitor below 720 px); the theme button switches System / Light / Dark. Still to check by
      hand: Arabic mirroring, Tab / Enter on the search field, the busy indicator while connecting,
      the tablet in portrait and in Split View
- [x] Shell action strip (2026-10-28, Windows snapshots, Android 17 emulator): the strip above the editor
      shows Format / History / Snippets / Export / Query Bar / Find / ⋮ by width of the tab (labels, icons,
      overflow), the ⋮ menu lists Execute All, Format, History, Snippets, Export, Save as Snippet, Duplicate
      Tab as the width requires; Query History opens from it. Still to check by hand: Format and Export
      and Execute All from the strip on a real script, the strip with the Query Bar open, Arabic
- [x] Android 10 tablet (2026-10-29): top bar, command palette (type, go key runs the row), shell tab with
      the action strip and its ⋮ menu, Query History from the strip; saved connection over `adb reverse`
- [x] Dialogs and the keyboard (2026-10-29, Android 17 emulator): New Connection with the keyboard open fits above it,
      Save / Cancel visible, Tab to Default DB scrolls the form to it. Still to check on the tablet: Preferences
      with a field focused ✅ (tablet), Import / Export, Create Collection, New Index, User and Function editors
- [ ] Rotate with a dialog open: the dialog stays, nothing restarts; resize the window (split screen at
      1/3, 1/2, 2/3; Stage Manager; Slide Over): the layout follows, the explorer folds in narrow widths
- [ ] Save a connection, connect, open a collection: browse documents by touch; the on-screen keyboard
      opens without capitals or suggestions in host, user, database and collection fields and does not
      cover the field being edited
- [ ] Press and hold on a result row, a connection and a tab: the context menu opens
- [ ] A hardware keyboard works (shortcuts, Ctrl on Android and Cmd on iPad); labels show Ctrl / the Command key
- [ ] Android back button and gesture: closes the top dialog or menu first; on the main page it does not
      leave the app while a connection is open
- [ ] Send the app to the background for 5 minutes and come back: open tabs and unsaved scripts are
      there (drafts on a kill), connections are checked (status bar), no error dialog
- [ ] Kill the app while a script is unsaved: the next start offers Recover Unsaved Scripts
- [ ] Preferences → Saved passwords names the key store of the platform (Android Keystore / Keychain)
- [ ] Export a collection to a file with the system file picker and import it again (Files app, Google
      Drive); a 100,000-document import does not get the app killed
- [ ] SSH tunnel with a key file picked from the file picker: the key is copied into the app's folder and
      the first connection asks to trust the host key
- [ ] TLS connection with a CA file picked the same way
- [ ] The JavaScript shell runs on the tablet (it no longer says "not available"): the whole shell checklist
      (section 32) applies
- [ ] Arabic UI at tablet sizes (right to left), and the AI assistant over HTTPS
- [ ] Google Play only: `zipalign -c -P 16 -v 4` passes for the release APK; Apple only: the archive
      validates with no privacy manifest warning and starts on an IPv6-only (NAT64) network
## 32. The JavaScript shell (docs/Shell.md)

The shell runs inside the app (QJSEngine) on every platform; the Node helper it replaced is gone
(docs/Shell.md). Run the list against a MongoDB 4.0 server and a current one; `ragoshell --shell-selftest=host:port` is a
quick automatic check of the engine (19 checks, `SHELLSELFTEST PASS` lines) and needs no UI.

- [ ] Open a shell tab and run `db.<collection>.find({}).limit(5)`: five documents in the grid, the status
      bar names the server and the database; with more documents than the page size the paging buttons
      load the next and previous page
- [ ] `db.<collection>.aggregate([{$group: {_id: '$field', n: {$sum: 1}}}, {$sort: {n: -1}}])`: aggregate
      result with paging; the Explain action of a find and of an aggregation opens the explain view
- [ ] Several statements in one script (`print('a'); db.c.find(); print('b')`): one result tab per
      statement; the output pane shows the printed lines without a stray `1` after each `print`
- [ ] `show dbs`, `show collections`, `show users`, `use admin` then `db.getName()` (the next statement and
      the next script run in `admin`); `rs.status()` on a standalone server shows an error result, not a
      dialog
- [ ] A server result used inside the statement: `for (var i = 0; i < 3; i++) { var d = db.c.findOne();
      print(d._id) }` prints three ids; `db.c.countDocuments({}) > 0` shows `true`
- [ ] Variables live on between the statements of a script (`var a = 1;` / `const b = a + 1;` / `b`) and
      not into the next script (`typeof a` → `"undefined"`)
- [ ] BSON values: `printjson({d: ISODate(), id: ObjectId(), n: NumberLong('9007199254740993'),
      x: NumberDecimal('0.1'), u: UUID(), b: BinData(0, 'AAEC')})` prints Extended JSON; inserting a
      `NumberLong('9007199254740993')` and reading it back in the grid and by `findOne().n.toString()`
      keeps every digit
- [ ] Writes: `insertOne`, `insertMany`, `updateOne` / `updateMany`, `replaceOne`, `deleteOne` /
      `deleteMany`, `findOneAndUpdate`, `bulkWrite`, `createIndex` / `getIndexes` / `dropIndex`, a duplicate
      key insert (an error result with the server's message)
- [ ] Errors: a syntax error, `undefinedThing`, `throw new Error('x')` and a server error each give an error
      result and the next script runs with no reconnect message
- [ ] Stop: `while (true) {}` ends within a second after Stop (or at the shell timeout, see Preferences →
      Shell timeout, with "Timeout reached"), and the next script runs
- [ ] Stop a server call: `db.c.find({$where: 'sleep(30000) || true'}).toArray()` (needs one document) ends
      within about a second after Stop; Server Monitor / `db.currentOp()` shows no operation left
- [ ] Shell timeout: set it to 3 seconds, run the same `$where` script: it ends with "Timeout reached" after
      about 3 seconds and the server has no operation left
- [ ] Startup script (Preferences): a function defined there works in every script; a syntax error in it
      is reported once and does not stop scripts
- [ ] Autocomplete: `db.` offers the collection names and the db methods, `db.<collection>.` the collection
      methods, `$ma` offers `$match`, `show ` / `use ` and `ObjectI` offer their words; a collection whose
      name needs `db.getCollection('...')` is offered in that form
- [ ] Connections: with TLS (CA file, client certificate, "allow invalid certificates"), through an SSH
      tunnel, to a replica set with a read preference of secondary preferred, with a user and password
      (authentication database other than `admin`), and `mongodb+srv://` on desktop
- [ ] Open five shell tabs on different connections: they work independently; closing a tab and opening
      it again reconnects; memory of the app grows by a few MB per tab (no `node` process in the task list)
- [ ] Tablets: the whole list on an Android tablet and on an iPad (the shell no longer says "not available")

## 33. Account and sync (docs/SyncClient.md, docs/SyncProtocol.md in ragoshell-api)

Prerequisite: a running API + portal (staging or local `ragoshell-api` + `ragoshell-infra`), two
machines or user profiles (A and B), and one verified account (every verified account has sync and
the hosted bridge, free with limits — no plan account is needed).

- [ ] **Device-flow sign-in (desktop)** — Preferences → Account → Sign in opens the device code
      dialog; the shown URL + user code authenticate in a browser; the dialog closes on its own and
      the Account section shows the signed-in account and the device name
- [ ] **Device-flow sign-in (tablet)** — the same flow on an Android tablet / iPad; the code entry
      screen is readable and the button stays reachable in narrow width
- [ ] **Wizard: passphrase entry** — Sync setup wizard → passphrase mode: set + confirm; a weak
      passphrase shows the strength hint; finish only on matching entries. Restart the app: the
      passphrase is not in `ragoshell.json` (search the file for it) and sync unlocks
- [ ] **Wizard: device-key mode** — same wizard with device key; no passphrase prompt afterwards
- [ ] **Collection selection** — uncheck e.g. `query_history` in the wizard; after a sync the
      collection list (Account section) shows it skipped, and no `query_history` row exists in
      `GET /v1/sync` on the server
- [ ] **Password opt-in** — a connection with a saved password: with opt-in **off**, the synced
      blob on the server decrypts (on machine B) to a connection without the password field; with
      opt-in **on**, machine B gets the working password
- [ ] **Two machines** — create a connection + snippet on A, Sync now; on B Sync now → the
      connection and snippet appear, identical fields
- [ ] **Conflict (last writer wins)** — edit the same preference/snippet on A and B while both are
      offline, then sync A then B: B's edit wins (its `updated_at`/`device_label` is newer); the
      Account section shows no error, the merged value matches B
- [ ] **412 recovery** — while A is mid-sync (or holding a stale etag), PUT from B; A's next sync
      retries once with a fresh GET and completes — no stuck "conflict" state
- [ ] **Sign-out mid-sync** — start a sync on A, then Sign out from Account during it: the run
      ends once (no repeated dialogs), Account returns to the signed-out state, no crash
- [ ] **Device revoke from portal** — sign in on the portal → Devices → remove machine A's device;
      on A run Sync now: exactly one sign-out notice, the app settles to signed-out, no retry loop
      (log panel shows no repeated requests)
- [ ] **3-device limit** — with three active devices on the account, start the device flow on a
      fourth: approving the code on the portal is refused (device limit reached); after removing one
      device under Devices the same approval works
- [ ] **500 uploads per day** — (staging with a lowered limit, or a scripted loop of Sync now) after
      the daily limit the PUT returns `429 sync_daily_write_limit` with `Retry-After`; the Account
      section shows the per-collection error (`put_http_429`), no retry loop, and downloads still work; it clears after
      the UTC day turns
- [ ] **Bridge: one session per account** — connect through the "RagoShell Cloud" channel on A, then
      on B with the same account: B is refused with "session limit reached: one cloud bridge session
      per account"; after A disconnects, B connects
- [ ] **Bridge: monthly limit** — (staging with a small `RAGOSHELL_GW_MONTHLY_BYTES`) run queries until
      the limit: the session closes with "monthly transfer limit reached" and a new connection is
      refused with the same message until the next UTC month
- [ ] **Automatic token refresh** — leave the app signed in and running past the token's half-life
      (3.5 days), or restart it after 4 days: the hosted channel still connects without a manual
      refresh; with no usable token, connecting shows "Your cloud access is being renewed…" and works
      on the next try (HostedGateway.md §9.8)
- [ ] **Inactivity rule** (describe only, not practical by hand) — an account inactive for 12 months
      gets a warning email; its sync data is deleted 30 days after delivery, the account stays. Check it
      with the API tests, not here
