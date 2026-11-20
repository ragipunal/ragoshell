#!/bin/bash
# On the Mac (docs/Platforms.md): screenshots of the app in an iOS simulator.
#   dev/mac/sim-shots.sh <simulator udid> <name> shell monitor prefs ...   -> /tmp/sim-<name>-<view>.png
# Needs build/ios-sim (dev/mac/build-ios.sh ios-sim simulator) and ~/mac-test-config.json (dev/ui-shots/README.md).
U=$1; N=$2; shift 2
export PATH=/opt/homebrew/bin:$PATH
APP=${APP:-$PWD/build/ios-sim/src/ragoshell-app/Debug-iphonesimulator/RagoShell.app}
B=tr.com.ragoshell.app
xcrun simctl boot $U 2>/dev/null; xcrun simctl bootstatus $U -b >/dev/null 2>&1
xcrun simctl uninstall $U $B 2>/dev/null
xcrun simctl install $U $APP || exit 1
xcrun simctl launch $U $B >/dev/null; sleep 5; xcrun simctl terminate $U $B 2>/dev/null
C=$(xcrun simctl get_app_container $U $B data)
D="$C/Library/Application Support/RagoShell/RagoShell"; mkdir -p "$D"; cp $HOME/mac-test-config.json "$D/ragoshell.json"
for v in "$@"; do
  case $v in
    shell)   args="--connect {11111111-2222-3333-4444-555555555555} --shell catalog products"; w=14 ;;
    monitor) args="--connect {11111111-2222-3333-4444-555555555555} --monitor"; w=14 ;;
    *)       args="--open-view $v"; w=8 ;;
  esac
  xcrun simctl terminate $U $B 2>/dev/null
  xcrun simctl launch $U $B $args >/dev/null; sleep $w
  xcrun simctl io $U screenshot /tmp/sim-$N-$v.png 2>&1 | tail -0
  echo "/tmp/sim-$N-$v.png"
done
