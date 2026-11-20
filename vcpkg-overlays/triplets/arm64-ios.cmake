# RagoShell iOS / iPadOS device triplet (universal iPhone + iPad, arm64).
# Pins VCPKG_OSX_DEPLOYMENT_TARGET to 18.0 to match Qt 6.12's minimum
# (QT_SUPPORTED_MIN_IOS_VERSION = 18) so OpenSSL 3, libssh2, libbson,
# mongo-c-driver and mongo-cxx-driver never embed a newer host SDK minos
# (e.g. 27.0) or bind unguarded newer-SDK libc symbols on iOS 18+ devices.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_SYSROOT iphoneos)
set(VCPKG_OSX_DEPLOYMENT_TARGET "18.0")
