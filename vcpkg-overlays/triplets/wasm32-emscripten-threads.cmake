# Multithreaded WebAssembly triplet (Qt 6.12 wasm_multithread + Emscripten 5.0.5 pthreads).
# Compiles all static vcpkg dependencies (libbson, mongo-c-driver, mongo-cxx-driver,
# openssl, libssh2, argon2, zlib, utf8proc) with -pthread (+atomics, +bulk-memory) so
# wasm-ld accepts --shared-memory and libc mutexes are thread-safe across QThread workers.
set(VCPKG_ENV_PASSTHROUGH_UNTRACKED EMSCRIPTEN_ROOT EMSDK PATH)

if(NOT DEFINED ENV{EMSCRIPTEN_ROOT})
    if(DEFINED ENV{EMSDK} AND EXISTS "$ENV{EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
        set(EMSCRIPTEN_ROOT "$ENV{EMSDK}/upstream/emscripten")
    else()
        find_path(EMSCRIPTEN_ROOT NAMES "emcc" "emcc.bat")
    endif()
else()
    set(EMSCRIPTEN_ROOT "$ENV{EMSCRIPTEN_ROOT}")
endif()

if(NOT EMSCRIPTEN_ROOT)
    if(NOT DEFINED ENV{EMSDK})
        message(FATAL_ERROR "The emcc compiler was not found in PATH and EMSDK is not set")
    endif()
    set(EMSCRIPTEN_ROOT "$ENV{EMSDK}/upstream/emscripten")
endif()

if(NOT EXISTS "${EMSCRIPTEN_ROOT}/cmake/Modules/Platform/Emscripten.cmake")
    message(FATAL_ERROR "Emscripten.cmake toolchain file not found under ${EMSCRIPTEN_ROOT}")
endif()

set(VCPKG_TARGET_ARCHITECTURE wasm32)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Emscripten)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_C_FLAGS "-pthread")
set(VCPKG_CXX_FLAGS "-pthread")
set(VCPKG_LINKER_FLAGS "-pthread")
# Chainload vcpkg's wrapper toolchain so VCPKG_C_FLAGS / VCPKG_CXX_FLAGS / VCPKG_LINKER_FLAGS
# are propagated into both CMake ports and Autotools/Configure ports (e.g. OpenSSL).
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${VCPKG_ROOT_DIR}/scripts/toolchains/emscripten.cmake")
