#!/usr/bin/env bash
# Screenshots of the app's screens on an Android device, for the per-platform UI check
# (docs/UiMatrix.md). The app is started with `--open-view <name>` (Main.qml, openDebugView) and the screen
# is captured with `adb exec-out screencap`.
#
#   dev/ui-shots/shots.sh [options] <view>...
#
#   -s SERIAL     adb device (default: the only one; or $ANDROID_SERIAL)
#   -o DIR        output folder (default build/ui-shots)
#   -p NAME       size preset that stands in for another screen, see below
#   -w SECONDS    wait after the start before the capture (default 10; the first start after an install is slower)
#
# Size presets change the device's `wm size` and `wm density` and put them back when the script ends:
#   phone          360x800 dp portrait      (1080x2400 at 480 dpi)
#   tablet-p       800x1280 dp portrait
#   tablet-l       1280x800 dp landscape
#   native         the device as it is (default)
# A dp is density/160 pixels, so the pixel size is dp * density / 160.
#
# Views: conn prefs about shortcuts history more-menu theme-menu palette recover hostkey export-python log
#        doc create-collection index user tabs  (the list in Main.qml, openDebugView), and shell / monitor (a tab
#        of the seed data: they need the test connection, dev/ui-shots/README.md)
# Windows, Linux and macOS: start the app with `--open-view <name>` and take the picture with the OS tool;
# iPad: docs/Platforms.md.
set -euo pipefail

PKG=tr.com.ragoshell.app
ACTIVITY=org.qtproject.qt.android.bindings.QtActivity
serial="${ANDROID_SERIAL:-}"
out=build/ui-shots
preset=native
wait_s=10

while getopts "s:o:p:w:" opt; do
    case "$opt" in
        s) serial="$OPTARG" ;;
        o) out="$OPTARG" ;;
        p) preset="$OPTARG" ;;
        w) wait_s="$OPTARG" ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -gt 0 ] || { sed -n '2,24p' "$0"; exit 2; }

adb_() { if [ -n "$serial" ]; then adb -s "$serial" "$@"; else adb "$@"; fi; }

# wm size is given in the display's natural orientation; the rotation is set so that the picture has the
# shape of the preset (the device's own sensor rotation is put back at the end).
case "$preset" in
    native) size=""; density=""; rotation="" ;;
    phone) size=1080x2400; density=480; rotation=0 ;;     # 360x800 dp portrait
    tablet-p) size=1200x1920; density=240; rotation=0 ;;  # 800x1280 dp portrait
    tablet-l) size=1200x1920; density=240; rotation=1 ;;  # 1280x800 dp landscape
    *) echo "unknown preset: $preset" >&2; exit 2 ;;
esac

if [ -n "$size" ]; then
    old_accel=$(adb_ shell settings get system accelerometer_rotation | tr -d '')
    old_rot=$(adb_ shell settings get system user_rotation | tr -d '')
    restore() {
        adb_ shell wm size reset >/dev/null 2>&1 || true
        adb_ shell wm density reset >/dev/null 2>&1 || true
        adb_ shell settings put system user_rotation "${old_rot:-0}" >/dev/null 2>&1 || true
        adb_ shell settings put system accelerometer_rotation "${old_accel:-1}" >/dev/null 2>&1 || true
    }
    trap restore EXIT
    adb_ shell wm size "$size" >/dev/null
    adb_ shell wm density "$density" >/dev/null
    adb_ shell settings put system accelerometer_rotation 0 >/dev/null
    adb_ shell settings put system user_rotation "$rotation" >/dev/null
fi

mkdir -p "$out"
for view in "$@"; do
    file="$out/${preset}-${view//[:\/]/_}.png"
    adb_ shell am force-stop "$PKG"
    # The device shell splits the arguments again, so the value of -e needs its own quotes.
    case "$view" in
        # a tab needs the "Local test" connection of dev/ui-shots/README.md in the device's ragoshell.json
        shell)   app_args="--connect {11111111-2222-3333-4444-555555555555} --shell catalog products"; extra_wait=6 ;;
        monitor) app_args="--connect {11111111-2222-3333-4444-555555555555} --monitor"; extra_wait=6 ;;
        *)       app_args="--open-view $view"; extra_wait=0 ;;
    esac
    adb_ shell "am start -n $PKG/$ACTIVITY -e applicationArguments '$app_args'" >/dev/null
    sleep $(( wait_s + extra_wait ))
    adb_ exec-out screencap -p > "$file"
    echo "$file"
done
adb_ shell am force-stop "$PKG"
