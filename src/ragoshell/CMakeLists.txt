# rago-core: GUI-independent core library used by the QML frontend
# (src/ragoshell-app), the MCP server (src/ragoshell-mcp) and the unit tests.
set(CMAKE_AUTOMOC ON)

set(CORE_SOURCES
    # Isolated Scope #1
    core/utils/QtUtils.cpp
    core/utils/KeyLabel.cpp
    core/utils/StdUtils.cpp
    core/utils/AppPaths.cpp
    core/utils/WasmPersist.cpp
    core/utils/HostKeyPrompter.cpp
    core/utils/AppLog.cpp
    core/utils/BackgroundPolicy.cpp
    core/utils/LocalNetwork.cpp
    core/utils/Minisign.cpp
    core/utils/Minisign.h
    core/utils/UpdatePulse.cpp
    core/utils/Logger.cpp
    core/HexUtils.cpp
    core/utils/BsonUtils.cpp
    core/utils/MongoJsonUtils.cpp
    core/utils/MongoJsonQueryUtils.cpp
    core/utils/ExplainPlan.cpp
    core/utils/AggregatePaging.cpp
    core/utils/AggregationPipeline.cpp
    core/utils/SchemaAnalysis.cpp
    core/utils/SchemaCatalog.cpp
    core/utils/SchemaService.cpp
    core/utils/SchemaCollector.cpp
    core/utils/FieldCompletion.cpp
    core/utils/ShellCompletion.cpp
    core/utils/ShellCompletionAnalyze.cpp
    core/utils/ShellCompletionCatalog.cpp
    core/utils/ShellCompletionChain.cpp
    core/utils/QueryCodeExport.cpp
    core/utils/AiQueryAssistant.cpp
    core/utils/AiQueryAssistantProviders.cpp
    core/utils/AiQueryAssistantEndpoint.cpp
    core/utils/ShellSignatures.cpp
    core/utils/CollectionTransfer.cpp
    core/utils/RecordSplitter.cpp
    core/utils/LanguageCatalog.cpp
    core/mcp/McpConfig.cpp
    core/mcp/McpGuards.cpp
    core/mcp/McpBackend.cpp
    core/mcp/McpServer.cpp
    core/mcp/McpClientSetup.cpp
    core/settings/CredentialSettings.cpp
    core/settings/ConnectionSettings.cpp
    core/Event.cpp
    core/Enums.cpp
    core/EventError.cpp
    core/EventBusSubscriber.cpp
    core/EventBusDispatcher.cpp
    core/EventWrapper.cpp
    core/EventBus.cpp
    core/domain/MongoNamespace.cpp
    core/domain/MongoFunction.cpp
    core/domain/MongoUtils.cpp
    core/domain/MongoCollection.cpp
    core/domain/MongoCollectionInfo.cpp
    core/domain/MongoQueryInfo.cpp
    core/domain/CursorPosition.cpp
    core/domain/ScriptInfo.cpp
    core/events/MongoEventsInfo.cpp
    core/bson/BsonTreeItem.cpp
    core/bson/BsonTreeModel.cpp
    core/bson/BsonTableModel.cpp
    core/bson/BsonTreeItem.h
    core/bson/BsonTreeModel.h
    core/bson/BsonTableModel.h
    shell/db/ptimeutil.cpp

    # Isolated Scope #2
    core/engine/ScriptEngine.cpp
    core/engine/ShellBackend.cpp
    core/engine/JsShellBackend.cpp
    core/engine/BsonJsConverter.cpp
    core/engine/ShellHost.cpp
    core/events/MongoEvents.cpp
    core/domain/MongoDocument.cpp
    core/transport/BridgeQuota.cpp
    core/transport/BridgeQuota.h
    core/transport/DeviceSessionLedger.h
    core/transport/IMongoTransport.h
    core/transport/InMemoryDemoTransport.cpp
    core/transport/InMemoryDemoTransport.h
    core/transport/NativeMongoTransport.cpp
    core/transport/NativeMongoTransport.h
    core/transport/EgressPolicy.cpp
    core/transport/EgressPolicy.h
    core/transport/GatewayProtocol.cpp
    core/transport/GatewayProtocol.h
    core/transport/GatewaySession.cpp
    core/transport/GatewaySession.h
    core/transport/WebSocketGatewayTransport.cpp
    core/transport/WebSocketGatewayTransport.h
    core/domain/MongoServer.cpp
    core/domain/MongoShell.cpp
    core/domain/MongoDatabase.cpp
    core/domain/App.cpp
    core/mongodb/MongoClient.cpp
    core/mongodb/MongoUriUtils.cpp
    core/mongodb/MongoWorker.cpp
    core/mongodb/MongoWorkerHandlers.cpp
    core/mongodb/ReplicaSet.cpp
    core/settings/SettingsManager.cpp
    core/settings/QueryHistoryStore.cpp
    core/settings/ScriptDraftStore.cpp
    core/AppRegistry.cpp
    core/UiSignals.h
    core/services/UiServices.cpp
    utils/StringOperations.cpp
    utils/common.cpp
    utils/SimpleCrypt.cpp
    utils/RagoCrypt.cpp
    utils/KeyStore.cpp
    utils/KeyStore_win.cpp
    utils/KeyStore_apple.cpp
    utils/KeyStore_linux.cpp
    utils/KeyStore_android.cpp
    utils/KeyStore_wasm.cpp
    utils/KeyStore_passfile.cpp

    # Settings & SSH plumbing (also GUI-free)
    core/settings/SshSettings.cpp
    core/settings/SslSettings.cpp
    core/settings/ReplicaSetSettings.cpp
    core/mongodb/SshTunnelWorker.cpp

    # Cloud identity & entitlement verification (RFC 8628 + ES256 / P-256)
    core/cloud/EntitlementToken.cpp
    core/cloud/EntitlementToken.h
    core/cloud/CloudClient.cpp
    core/cloud/CloudClient.h

    # Zero-knowledge encrypted cloud sync (Roadmap 5.3, docs/SyncClient.md)
    core/cloud/SyncCrypto.cpp
    core/cloud/SyncCrypto.h
    core/cloud/SyncCollections.cpp
    core/cloud/SyncCollections.h
    core/cloud/SyncEngine.cpp
    core/cloud/SyncEngine.h
)

# The shell runtime (core/engine/js/*.js, docs/Shell.md) is concatenated in order and compiled into
# the library as a byte array: no resource system, so it works the same in a static library on every
# platform. Editing any part re-runs CMake.
set(RAGO_SHELL_RUNTIME_PARTS
    ${CMAKE_CURRENT_SOURCE_DIR}/core/engine/js/01-preamble-ejson-writes.js
    ${CMAKE_CURRENT_SOURCE_DIR}/core/engine/js/02-cursors-collection.js
    ${CMAKE_CURRENT_SOURCE_DIR}/core/engine/js/03-db-globals-eval.js
    ${CMAKE_CURRENT_SOURCE_DIR}/core/engine/js/04-completion-exports.js
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RAGO_SHELL_RUNTIME_PARTS})
set(rago_runtime_hex "")
foreach(rago_js_part IN LISTS RAGO_SHELL_RUNTIME_PARTS)
    file(READ ${rago_js_part} rago_part_hex HEX)
    string(APPEND rago_runtime_hex "${rago_part_hex}")
endforeach()
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," rago_runtime_bytes "${rago_runtime_hex}")
set(rago_runtime_cpp "// Generated from core/engine/js/*.js by CMake; do not edit.
#include <cstddef>
namespace RagoShell
{
    extern const unsigned char kShellRuntimeScript[];
    extern const std::size_t kShellRuntimeScriptSize;
    const unsigned char kShellRuntimeScript[] = { ${rago_runtime_bytes} 0x00 };
    const std::size_t kShellRuntimeScriptSize = sizeof(kShellRuntimeScript) - 1;
}
")
set(rago_runtime_file ${CMAKE_CURRENT_BINARY_DIR}/ShellRuntimeScript.cpp)
set(rago_runtime_old "")
if(EXISTS ${rago_runtime_file})
    file(READ ${rago_runtime_file} rago_runtime_old)
endif()
if(NOT rago_runtime_old STREQUAL rago_runtime_cpp)
    file(WRITE ${rago_runtime_file} "${rago_runtime_cpp}")
endif()

# Brand constants (docs/Brand.md) — generated header consumed by C++ and QML Brand.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/src/ragoshell/core")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/core/BrandInfo.h.in"
    "${CMAKE_BINARY_DIR}/src/ragoshell/core/BrandInfo.h"
    @ONLY)

add_library(rago-core STATIC ${CORE_SOURCES} ${rago_runtime_file})
set_target_properties(rago-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(unofficial-argon2 CONFIG REQUIRED)

target_link_libraries(rago-core
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        Qt6::WebSockets
        Qt6::Qml
        Qt6::Xml
        OpenSSL::Crypto
        unofficial::argon2::libargon2
        qjson
        mongodb
        ssh
        Threads::Threads)

target_include_directories(rago-core
    PUBLIC
        ${CMAKE_HOME_DIRECTORY}/src
        ${CMAKE_BINARY_DIR}/src)

if(WIN32)
    # Credential Manager (KeyStore_win.cpp)
    target_link_libraries(rago-core PUBLIC advapi32)
endif()

if(APPLE)
    find_library(SECURITY NAMES Security)
    find_library(CORE_FOUNDATION NAMES CoreFoundation)
    target_link_libraries(rago-core PUBLIC ${SECURITY} ${CORE_FOUNDATION} -lresolv)
endif()

# Get build number
find_package(Git)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE BUILD_NUMBER   
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif(GIT_FOUND)

# Remote builds (Visual Studio copies the sources to ~/.vs/<project>) are not
# git checkouts; the packaging scripts in ci/ always run in a clone.
if(BUILD_NUMBER STREQUAL "")
   MESSAGE( WARNING "Failed to get BUILD_NUMBER from git, using 'local'" )
   set(BUILD_NUMBER local)
endif()

set(BUILD_NUMBER ${BUILD_NUMBER} CACHE INTERNAL "")

# Project defines are PUBLIC on rago-core so every target inherits them.
target_compile_definitions(rago-core
    PUBLIC
        BUILDING_RAGO
        PROJECT_NAME="${PROJECT_NAME}"
        PROJECT_NAME_TITLE="${PROJECT_NAME_TITLE}"
        PROJECT_COPYRIGHT="${PROJECT_COPYRIGHT}"
        PROJECT_DOMAIN="${PROJECT_DOMAIN}"
        PROJECT_COMPANYNAME="${PROJECT_COMPANYNAME}"
        PROJECT_COMPANYNAME_DOMAIN="${PROJECT_COMPANYNAME_DOMAIN}"
        PROJECT_GITHUB_FORK="${PROJECT_GITHUB_FORK}"
        PROJECT_GITHUB_ISSUES="${PROJECT_GITHUB_ISSUES}"
        PROJECT_VERSION="${PROJECT_VERSION}"
        PROJECT_VERSION_SHORT="${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
        BUILD_NUMBER="${BUILD_NUMBER}"
        PROJECT_NAME_LOWERCASE="${PROJECT_NAME_LOWERCASE}"
        PROJECT_QT_VERSION="${Qt6Core_VERSION}"
        MongoDB_VERSION="${MongoDB_VERSION}"
        OPENSSL_VERSION="${OPENSSL_VERSION}"
        LIBSSH2_VERSION="${LIBSSH2_VERSION}"
        QJSON_VERSION="${QJSON_VERSION}"
        GOOGLE_TEST_VERSION="${GOOGLE_TEST_VERSION}"
)
