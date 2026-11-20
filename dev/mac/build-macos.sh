#!/bin/bash
# On the Mac: debug build of the macOS app, unit tests, QML check (docs/Platforms.md).
#   ~/build-ragoshell/dev/mac/build-macos.sh
export PATH=/opt/homebrew/bin:$PATH VCPKG_ROOT=${VCPKG_ROOT:-$HOME/vcpkg} Qt6_DIR=${Qt6_DIR:-$HOME/Qt/6.12.0/macos}
cd "$(dirname "$0")/../.." || exit 1
echo "== configure"
cmake --preset macos-debug 2>&1 | grep -i "error" | head
echo "== build"
cmake --build --preset macos-debug --target rago_unit_tests ragoshell ragoshell-mcp -j8 2>&1 | grep -E "error:|FAILED|^\[.*Linking" | tail -15
echo "== unit tests"
export RAGOSHELL_CONFIG_DIR=/tmp/rago-mac-cfg; rm -rf "$RAGOSHELL_CONFIG_DIR"
export RAGOSHELL_KEYSTORE=file
./build/macos-debug/src/ragoshell-unit-tests/rago_unit_tests 2>&1 | grep -E "FAILED|PASSED|tests ran|Failure" | head -12
echo "== qml check"
APP=build/macos-debug/src/ragoshell-app/RagoShell.app/Contents/MacOS/RagoShell
QT_QPA_PLATFORM=offscreen "$APP" --check-qml >/dev/null 2>&1; echo "check-qml exit=$?"
echo "== done"
