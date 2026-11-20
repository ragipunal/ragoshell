# ragoshell-agent: Mod C local bridge for the WASM build (docs/LocalAgent.md,
# roadmap 8.3). Desktop tool — not built for phones/tablets/WASM itself.

add_executable(ragoshell-agent main.cpp)

target_link_libraries(ragoshell-agent
    PRIVATE
        rago-core
        Qt6::WebSockets
        Threads::Threads)

target_include_directories(ragoshell-agent
    PRIVATE
        ${CMAKE_HOME_DIRECTORY}/src)

# Same output folder as the QML frontend and ragoshell-mcp: packaging copies
# that folder and the binary finds the deployed Qt / driver DLLs there.
set_target_properties(ragoshell-agent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/ragoshell-app")

if(SYSTEM_WINDOWS AND TARGET Qt6::windeployqt)
    add_custom_command(TARGET ragoshell-agent POST_BUILD
        COMMAND Qt6::windeployqt
            $<IF:$<CONFIG:Debug>,--debug,--release>
            --no-translations
            --no-compiler-runtime
            "$<TARGET_FILE:ragoshell-agent>"
        COMMENT "Deploying Qt runtime for ragoshell-agent")
elseif(SYSTEM_LINUX)
    include(GNUInstallDirs)
    set_target_properties(ragoshell-agent PROPERTIES
        OUTPUT_NAME "${RAGO_PRODUCT_ID}-agent"
        INSTALL_RPATH "$ORIGIN/../lib")
    install(TARGETS ragoshell-agent RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
