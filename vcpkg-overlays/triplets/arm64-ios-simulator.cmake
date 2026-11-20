# RagoShell iOS / iPadOS Simulator triplet (Apple Silicon host, arm64).
# Pins VCPKG_OSX_DEPLOYMENT_TARGET to 18.0 to match Qt 6.12's minimum.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_SYSROOT iphonesimulator)
set(VCPKG_OSX_DEPLOYMENT_TARGET "18.0")
