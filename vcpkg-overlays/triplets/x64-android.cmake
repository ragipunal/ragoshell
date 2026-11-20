# RagoShell Android x86_64 emulator triplet.
# API 28 (Android 9.0 Pie) matches Qt 6.12's ANDROID_PLATFORM=android-28 floor.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Android)
set(VCPKG_CMAKE_SYSTEM_VERSION 28)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=x86_64-linux-android")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DANDROID_ABI=x86_64)
# Enforce 16 KB ELF LOAD segment alignment (Android 15+ / Google Play requirement).
set(VCPKG_LINKER_FLAGS "-Wl,-z,max-page-size=16384")
