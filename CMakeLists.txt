cmake_minimum_required(VERSION 3.20)

# Apple deployment targets must be set before project() so CMake compiler probes,
# Xcode project settings, and all third-party targets inherit the exact floor:
#   - iOS / iPadOS: 18.0 (Qt 6.12 QT_SUPPORTED_MIN_IOS_VERSION; Base SDK = latest iphoneos)
#   - macOS:        14.4 (Qt 6.12 QT_SUPPORTED_MIN_MACOS_VERSION; Base SDK = latest macosx)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "18")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "18.0" CACHE STRING "Minimum iOS/iPadOS deployment version" FORCE)
    elseif(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "18.0")
        message(FATAL_ERROR "RagoShell on iOS/iPadOS requires CMAKE_OSX_DEPLOYMENT_TARGET >= 18.0 (Qt 6.12 minimum).")
    endif()
elseif(CMAKE_HOST_APPLE AND (NOT DEFINED CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "14.4" CACHE STRING "Minimum macOS deployment version" FORCE)
    elseif(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "14.4")
        message(FATAL_ERROR "RagoShell on macOS requires CMAKE_OSX_DEPLOYMENT_TARGET >= 14.4 (Qt 6.12 minimum).")
    endif()
endif()

project(RagoShell)

# Where to look first for CMake modules, before "${CMAKE_ROOT}/Modules" is checked
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# Project version
set(PROJECT_VERSION_MAJOR "2")
set(PROJECT_VERSION_MINOR "1")
set(PROJECT_VERSION_PATCH "0")
set(PROJECT_VERSION_BUILD "")
set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}${PROJECT_VERSION_BUILD}")

# Enable C++17 features 
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(RAGO_STORE_BUILD "Disable self-updater for managed app stores (App Store, Google Play, Flatpak)" OFF)
# Docker builds only the web gateway + its library deps (docs/WebGateway.md §5):
# skips Qt Quick/Widgets, the desktop app, tests and third-party editor bits.
option(RAGO_GATEWAY_ONLY "Build only rago-core + ragoshell-web-gateway (self-hosted Docker)" OFF)

# Modules
include(RagoShellPrintUtils)
include(RagoShellCMakeDefaults)
include(RagoShellPlatform)
include(RagoShellDefaults)
include(RagoShellCommon)
include(RagoShellTrashSymbols)

if(SYSTEM_MACOSX)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
elseif(SYSTEM_IOS)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
elseif(SYSTEM_ANDROID)
    # Android SDK bounds (docs/Platforms.md):
    #   - Min SDK 28 (Android 9.0 Pie): Qt 6.12's ANDROID_PLATFORM floor, vcpkg's
    #     VCPKG_CMAKE_SYSTEM_VERSION=28, and Bionic libc getrandom() required by OpenSSL 3 & libmongoc.
    #   - Target SDK 37 (Android 17): Google Play latest target SDK requirement; runtime
    #     ACCESS_LOCAL_NETWORK handling is implemented in LocalNetworkPermission.cpp.
    #   - Compile SDK: auto-detected by Qt from ANDROID_SDK_ROOT/platforms (e.g. android-37 / 37.2)
    #     unless explicitly overridden via RAGO_ANDROID_COMPILE_SDK.
    set(RAGO_ANDROID_MIN_SDK "28" CACHE STRING "Minimum Android API level (Android 9.0 Pie)")
    set(RAGO_ANDROID_TARGET_SDK "37" CACHE STRING "Target Android API level for Google Play (Android 17)")
    set(RAGO_ANDROID_COMPILE_SDK "" CACHE STRING "Optional explicit Android compileSdk (auto-detected from latest installed SDK when empty)")
    if(RAGO_ANDROID_MIN_SDK LESS 28)
        message(FATAL_ERROR "RagoShell on Android requires RAGO_ANDROID_MIN_SDK >= 28 (Qt 6.12, OpenSSL 3 and mongo-c-driver minimum).")
    endif()
    if(RAGO_ANDROID_TARGET_SDK LESS RAGO_ANDROID_MIN_SDK)
        message(FATAL_ERROR "RAGO_ANDROID_TARGET_SDK (${RAGO_ANDROID_TARGET_SDK}) must be >= RAGO_ANDROID_MIN_SDK (${RAGO_ANDROID_MIN_SDK}).")
    endif()
    # Android 15+ / Google Play requirement: 16 KB ELF LOAD segment alignment (p_align >= 0x4000).
    add_link_options("-Wl,-z,max-page-size=16384")
endif()

# Search for dependencies
find_package(Threading REQUIRED)    # Wrapper arround CMake "Threads" module
# Widgets, PrintSupport and Core5Compat are for QScintilla (the shell editor).
# QtHttpServer is only used by ragoshell-web-gateway on desktop platforms —
# mobile targets (Android/iOS) and Emscripten do not build web-gateway.
set(QT_COMPONENTS_HTTPSERVER "")
if(NOT SYSTEM_MOBILE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(QT_COMPONENTS_HTTPSERVER HttpServer)
endif()
if(RAGO_GATEWAY_ONLY)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Xml Core5Compat Qml WebSockets ${QT_COMPONENTS_HTTPSERVER})
else()
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets PrintSupport Network Xml Core5Compat Qml Quick QuickControls2 WebSockets ${QT_COMPONENTS_HTTPSERVER})
    # lupdate / lrelease for the UI translations (docs/Localization.md).
    find_package(Qt6 REQUIRED COMPONENTS LinguistTools)
endif()
if(SYSTEM_WINDOWS)
    find_package(Qt6 REQUIRED COMPONENTS EntryPointPrivate)
endif()
# mongo-cxx-driver / mongo-c-driver via vcpkg manifest mode (see vcpkg.json)
# Qt's WASM macros (_qt_test_emscripten_version etc.) live in this helper,
# which qt.toolchain.cmake normally includes; with the vcpkg+Emscripten
# chainload we include it directly (docs/WasmBuildReport.md §6).
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    include("${Qt6_DIR}/QtPublicWasmToolchainHelpers.cmake" OPTIONAL)
endif()
find_package(bsoncxx CONFIG REQUIRED)
find_package(mongocxx CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
# SSH tunnels: libssh2 from vcpkg with the OpenSSL backend. The in-tree 1.9.0 predates
# OpenSSL 3 and fails every handshake with it (LIBSSH2_ERROR_ENCRYPT, -44).
find_package(Libssh2 CONFIG REQUIRED)
set(LIBSSH2_VERSION ${Libssh2_VERSION})
set(LIBSSH2_DIR "vcpkg")

# Backwards-compatible alias: existing targets link against "mongodb".
add_library(mongodb INTERFACE)
target_link_libraries(mongodb INTERFACE
    $<IF:$<TARGET_EXISTS:mongo::mongocxx_static>,mongo::mongocxx_static,mongo::mongocxx_shared>
    $<IF:$<TARGET_EXISTS:mongo::bsoncxx_static>,mongo::bsoncxx_static,mongo::bsoncxx_shared>)
set(MongoDB_VERSION ${mongocxx_VERSION})

# Projects
set(QJSON_VERSION 0.8.1)
set(QJSON_DIR src/third-party/qjson-${QJSON_VERSION})
set(QSCINTILLA_VERSION 2.8.4)
set(QSCINTILLA_DIR src/third-party/qscintilla-${QSCINTILLA_VERSION})
# todo: Use equivalent vars from googletest cmake files. Currently they are somehow not working.
set(GOOGLE_TEST_VERSION 1.8.1)
set(GOOGLE_TEST_DIR src/third-party/googletest-${GOOGLE_TEST_VERSION})
add_subdirectory(src/ragoshell/ssh)
add_subdirectory(${QJSON_DIR})
if(NOT RAGO_GATEWAY_ONLY)
    add_subdirectory(${QSCINTILLA_DIR})
endif()
# The unit tests and the MCP server (a stdio process) are desktop tools: not built for phones and tablets.
if(NOT SYSTEM_MOBILE AND NOT RAGO_GATEWAY_ONLY)
    set(INSTALL_GTEST OFF CACHE BOOL "Disable googletest install rules when embedded" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "Disable unused gmock targets" FORCE)
    add_subdirectory(${GOOGLE_TEST_DIR})
endif()
add_subdirectory(src/ragoshell)
if(NOT RAGO_GATEWAY_ONLY)
    add_subdirectory(src/ragoshell-app)
endif()
if(NOT SYSTEM_MOBILE AND NOT RAGO_GATEWAY_ONLY)
    add_subdirectory(src/ragoshell-mcp)
    add_subdirectory(src/ragoshell-agent)
    add_subdirectory(src/ragoshell-unit-tests)
endif()
if(NOT SYSTEM_MOBILE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    add_subdirectory(src/ragoshell-web-gateway)
endif()

# Install root notices, embedded third-party licenses, and vcpkg port copyrights into licenses/
include(RagoShellLicenses)

# Show configuration summary
include(RagoShellConfigurationSummary)
