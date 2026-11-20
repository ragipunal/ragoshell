#!/bin/bash
# On the Mac: build the iOS app in a fresh folder (Xcode keeps a project folder it has open, so every build gets its
# own name) (docs/Platforms.md).
#   ~/build-ragoshell/dev/mac/build-ios.sh ios-ck9 [device|simulator]
# device: unsigned build for iPad (Xcode signs it when you press Run); simulator: for the iOS simulator.
D=${1:?folder name, e.g. ios-ck9}; KIND=${2:-device}
export PATH=/opt/homebrew/bin:$PATH VCPKG_ROOT=${VCPKG_ROOT:-$HOME/vcpkg} Qt6_DIR=${Qt6_DIR:-$HOME/Qt/6.12.0/macos}
cd "$(dirname "$0")/../.." || exit 1
if [ "$KIND" = simulator ]; then
    export Qt6_DIR_IOS=$HOME/Qt/6.12.0/ios_simulator_arm64
    extra=(-DVCPKG_TARGET_TRIPLET=arm64-ios-simulator -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY=)
    sdk=(-sdk iphonesimulator -arch arm64)
    if [ -d "$PWD/build/ios-sim/vcpkg_installed" ]; then
        vcpkg_dir=(-DVCPKG_INSTALLED_DIR="$PWD/build/ios-sim/vcpkg_installed")
    elif [ -d "$HOME/build-ragoshell/build/ios-sim/vcpkg_installed" ]; then
        vcpkg_dir=(-DVCPKG_INSTALLED_DIR="$HOME/build-ragoshell/build/ios-sim/vcpkg_installed")
    else
        vcpkg_dir=()
    fi
else
    export Qt6_DIR_IOS=$HOME/Qt/6.12.0/ios_device
    extra=(-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY=)
    sdk=(-sdk iphoneos)
    vcpkg_dir=(-DVCPKG_INSTALLED_DIR=$PWD/build/ios/vcpkg_installed)
fi
if [ ! -f "build/$D/RagoShell.xcodeproj/project.pbxproj" ] || [ "$FORCE" = "1" ]; then
    rm -rf "build/$D"
    cmake --preset ios -B "build/$D" "${extra[@]}" "${vcpkg_dir[@]}" > "$HOME/$D-config.log" 2>&1 || { echo "configure failed: ~/$D-config.log"; exit 1; }
fi
(cd "build/$D" && xcodebuild -project RagoShell.xcodeproj -target ragoshell -configuration Debug "${sdk[@]}" CODE_SIGNING_ALLOWED=NO > "$HOME/$D-build.log" 2>&1)
echo "build exit=$? (log: ~/$D-build.log)"
