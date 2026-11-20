# Looking at every screen (docs/UiMatrix.md)

Three tools, all start the app with the debug arguments of `Main.qml` (`openDebugView`) and `main.cpp`.

| Tool | What it does | Needs |
|---|---|---|
| `shots.sh` | screenshots of the screens on an Android device, at a phone / tablet preset (`wm size`, `wm density`, rotation; put back afterwards) | `adb`, a debug build installed |
| `geometry.sh` | the layout guard on the desktop build: window size, density and language are set from the command line, every view is opened and the items beyond the edge of the window or of their dialog (the left edge too, for Arabic), and buttons whose text had to be cut, are listed | the desktop build |
| `matrix-table.mjs` | turns the output of a run of `geometry.sh` over sizes and languages into the tables of `docs/UiMatrix.md` | node |

The desktop build can also take a real picture of its window (the screen capture of a Windows desktop session is
black for a GPU window): `ragoshell --window-size 800x1280 --density Touch --lang ar --open-view prefs --snapshot out.png 5000 --exit-after-snapshot`.

## The test connection

The views `shell` and `monitor` open a tab on a server, so they need a saved connection called "Local test" with the
uuid `{11111111-2222-3333-4444-555555555555}`. The seed data is a local docker `mongo:4.0` server (the author's setup is `dev/mongo-test/`, which is git-ignored).

Put it in a settings file of its own, never in the real one:

- **Desktop:** a folder with a `ragoshell.json` (copy of a real one with `connections` replaced by one entry:
  direct connection to `127.0.0.1:27017`, no TLS, no credentials), and `RAGOSHELL_CONFIG_DIR=<folder>`;
  `geometry.sh -c <folder>`.
- **Android (debug build):** the same file with the PC's address as host, copied into the app with
  `adb push x.json /data/local/tmp/` and `adb shell run-as tr.com.ragoshell.app cp /data/local/tmp/x.json files/ragoshell.json`
  (back up `files/ragoshell.json` first; on Git Bash set `MSYS_NO_PATHCONV=1`). The firewall of the PC has to let the
  device reach port 27017.
- **iPad:** over the Mac, `xcrun devicectl device process launch --device <udid> tr.com.ragoshell.app --connect ... --shell ...`
  and `xcrun devicectl device capture screenshot --device <udid> --destination x.png`; the connection goes into the
  app's data folder with `devicectl device copy to` (see docs/UiMatrix.md for the path).

## The whole matrix

```bash
for size in "360x800 Touch" "800x1280 Touch" "1280x800 Touch" "1100x700 Compact"; do
  set -- $size
  for lang in en tr de ar ja; do
    dev/ui-shots/geometry.sh -s "$1" -d "$2" -l "$lang" -c "$CFG" | grep -v wants
  done
done > matrix.txt
node dev/ui-shots/matrix-table.mjs matrix.txt
```

About 35 minutes. Run it from a copy of the build folder if you want to keep building meanwhile.
