# ragoshell-mcp: read-only MCP server (stdio) for saved connections.
# A console program next to ragoshell so MCP clients get clean stdio pipes
# and the Qt / driver DLLs are shared with the GUI.

add_executable(ragoshell-mcp main.cpp)

target_link_libraries(ragoshell-mcp
    PRIVATE
        rago-core
        Threads::Threads)

target_include_directories(ragoshell-mcp
    PRIVATE
        ${CMAKE_HOME_DIRECTORY}/src)

# Same output folder as the QML frontend: packaging copies that folder, and
# the executable finds the deployed Qt / mongocxx / OpenSSL DLLs there.
set_target_properties(ragoshell-mcp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/ragoshell-app")

if(SYSTEM_WINDOWS AND TARGET Qt6::windeployqt)
    add_custom_command(TARGET ragoshell-mcp POST_BUILD
        COMMAND Qt6::windeployqt
            $<IF:$<CONFIG:Debug>,--debug,--release>
            --no-translations
            --no-compiler-runtime
            "$<TARGET_FILE:ragoshell-mcp>"
        COMMENT "Deploying Qt runtime for ragoshell-mcp")
elseif(SYSTEM_LINUX)
    include(GNUInstallDirs)
    set_target_properties(ragoshell-mcp PROPERTIES
        OUTPUT_NAME "${RAGO_PRODUCT_ID}-mcp"
        INSTALL_RPATH "$ORIGIN/../lib")
    install(TARGETS ragoshell-mcp RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
