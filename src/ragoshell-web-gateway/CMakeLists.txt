# ragoshell-web-gateway — Mod B self-hosted bridge: static WASM bundle +
# /ws gateway sessions on one QAbstractHttpServer port (docs/WebGateway.md).
add_executable(ragoshell-web-gateway
    main.cpp
)

target_link_libraries(ragoshell-web-gateway
    PRIVATE
        rago-core
        Qt6::Core
        Qt6::Network
        Qt6::WebSockets
        Qt6::HttpServer
        ${MONGO_CXX_LIBS}
        ${MONGO_C_LIBS}
)

set_target_properties(ragoshell-web-gateway PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/ragoshell-app
)
