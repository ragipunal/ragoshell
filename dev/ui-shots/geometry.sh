#!/usr/bin/env bash
# Layout guard without a device (docs/DesignSystem.md, section 7, and docs/UiMatrix.md): starts the desktop
# app at the size of a phone or tablet with the touch sizes, opens each view and lists the visible items that
# reach past the edge of the window or of their dialog (Main.qml, geometryReport; the left edge too, for the
# right-to-left languages). Exit status 1 when any view has such an item.
#
#   dev/ui-shots/geometry.sh [-e path/to/ragoshell] [-s 360x800] [-d Touch] [-l tr] [-c configdir] [view...]
#
#   -s  window size     -d  density (Touch or Compact)     -l  language code (en tr de ar ja ...)
#   -c  a folder with a ragoshell.json that has a connection "Local test" to 127.0.0.1:27017 (the docker server of
#       a local docker mongo:4.0); needed for the views that open a tab. See dev/ui-shots/README.md.
#
# Views: the debug views of Main.qml (conn prefs prefs-mcp about licenses shortcuts history create-collection index user doc palette
# more-menu theme-menu export-python hostkey hostkey-changed recover log tabs) and, with -c, the tabs
# `shell` (a collection of the seed data in a shell tab) and `monitor` (the server monitor).
# Nothing is saved to the user's settings.
set -uo pipefail

exe=build/windows/src/ragoshell-app/Debug/ragoshell.exe
size=360x800
density=Touch
lang=""
cfg=""
while getopts "e:s:d:l:c:" opt; do
    case "$opt" in
        e) exe="$OPTARG" ;;
        s) size="$OPTARG" ;;
        d) density="$OPTARG" ;;
        l) lang="$OPTARG" ;;
        c) cfg="$OPTARG" ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
views=("$@")
[ ${#views[@]} -gt 0 ] || views=(conn prefs prefs-mcp about licenses shortcuts history create-collection index user doc palette more-menu theme-menu
                                 export-python hostkey hostkey-changed recover log tabs update-banner mandatory-update)
uuid="{11111111-2222-3333-4444-555555555555}"

status=0
for view in "${views[@]}"; do
    extra=(); wait_ms=2500
    case "$view" in
        shell)   extra=(--connect "$uuid" --shell catalog products); wait_ms=9000 ;;
        monitor) extra=(--connect "$uuid" --monitor); wait_ms=9000 ;;
        *)       extra=(--open-view "$view") ;;
    esac
    [ -n "$lang" ] && extra+=(--lang "$lang")
    if [ -n "$cfg" ]; then export RAGOSHELL_CONFIG_DIR="$cfg"; fi
    export RAGOSHELL_KEYSTORE="${RAGOSHELL_KEYSTORE:-file}"
    out=$("$exe" --window-size "$size" --density "$density" "${extra[@]}" --check-geometry "$wait_ms" 2>/dev/null | grep '^\[geometry\]')
    # "right=" / "left=" lines are items beyond an edge, "cut=" a button whose text was shortened; "wants" lines only say which layout asked for the width.
    n=$(printf '%s\n' "$out" | grep -cE ' (right|left|cut)=')
    echo "== $view @ $size $density ${lang:+$lang }: $n item(s) beyond an edge"
    if [ "$n" -gt 0 ]; then
        printf '%s\n' "$out" | grep -v 'item(s) beyond' | head -14
        status=1
    fi
done
exit $status
