#########################################
###   Unit Testing with Google Test   ###
#########################################

set(CMAKE_AUTOMOC ON)

### --- Enable and setup gtest
enable_testing()
include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR})
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

### --- Setup test source files
# New test & source file pairs MUST have the following format: 
#  Test file:   /path/Foo_test.cpp
#  Source file: /path/Foo.cpp or /path/Foo.h
set(RAGO_SRC_DIR ${CMAKE_HOME_DIRECTORY}/src/ragoshell)
set(SOURCES_TEST
    ${RAGO_SRC_DIR}/core/BrandInfo_test.cpp
    ${RAGO_SRC_DIR}/core/cloud/EntitlementToken_test.cpp
    ${RAGO_SRC_DIR}/core/cloud/CloudClient_test.cpp
    ${RAGO_SRC_DIR}/core/cloud/SyncCrypto_test.cpp
    ${RAGO_SRC_DIR}/core/cloud/SyncCollections_test.cpp
    ${RAGO_SRC_DIR}/core/cloud/SyncEngine_test.cpp
    ${RAGO_SRC_DIR}/core/transport/InMemoryDemoTransport_test.cpp
    ${RAGO_SRC_DIR}/core/transport/NativeMongoTransport_test.cpp
    ${RAGO_SRC_DIR}/core/transport/EgressPolicy_test.cpp
    ${RAGO_SRC_DIR}/core/transport/WebSocketGatewayTransport_test.cpp
    ${RAGO_SRC_DIR}/core/mongodb/MongoClientTransport_test.cpp
    ${RAGO_SRC_DIR}/utils/RagoCrypt_test.cpp
    ${RAGO_SRC_DIR}/utils/KeyStore_test.cpp
    ${RAGO_SRC_DIR}/utils/KeyStore_passfile_test.cpp
    ${RAGO_SRC_DIR}/utils/StringOperations_test.cpp
    ${RAGO_SRC_DIR}/core/EventBus_test.cpp
    ${RAGO_SRC_DIR}/core/HexUtils_test.cpp
    ${RAGO_SRC_DIR}/core/utils/KeyLabel_test.cpp
    ${RAGO_SRC_DIR}/core/bson/BsonTreeModel_test.cpp
    ${RAGO_SRC_DIR}/core/mongodb/MongoUriUtils_test.cpp
    ${RAGO_SRC_DIR}/core/mongodb/SshTunnelWorker_test.cpp
    ${RAGO_SRC_DIR}/core/utils/MongoJsonUtils_test.cpp
    ${RAGO_SRC_DIR}/core/utils/CollectionTransfer_test.cpp
    ${RAGO_SRC_DIR}/core/utils/RecordSplitter_test.cpp
    ${RAGO_SRC_DIR}/core/utils/CollectionTransferLive_test.cpp
    ${RAGO_SRC_DIR}/core/utils/LanguageCatalog_test.cpp
    ${RAGO_SRC_DIR}/core/engine/ScriptEngine_test.cpp
    ${RAGO_SRC_DIR}/core/engine/JsEngineCapabilities_test.cpp
    ${RAGO_SRC_DIR}/core/engine/ShellHost_test.cpp
    ${RAGO_SRC_DIR}/core/engine/ShellCorpus_test.cpp
    ${RAGO_SRC_DIR}/core/engine/JsShellBackendLive_test.cpp
    ${RAGO_SRC_DIR}/core/settings/SettingsManager_test.cpp
    ${RAGO_SRC_DIR}/core/settings/QueryHistoryStore_test.cpp
    ${RAGO_SRC_DIR}/core/settings/ScriptDraftStore_test.cpp
    ${RAGO_SRC_DIR}/core/utils/ExplainPlan_test.cpp
    ${RAGO_SRC_DIR}/core/utils/AggregatePaging_test.cpp
    ${RAGO_SRC_DIR}/core/utils/AggregationPipeline_test.cpp
    ${RAGO_SRC_DIR}/core/utils/SchemaAnalysis_test.cpp
    ${RAGO_SRC_DIR}/core/utils/SchemaCatalog_test.cpp
    ${RAGO_SRC_DIR}/core/utils/SchemaService_test.cpp
    ${RAGO_SRC_DIR}/core/utils/SchemaCollector_test.cpp
    ${RAGO_SRC_DIR}/core/utils/FieldCompletion_test.cpp
    ${RAGO_SRC_DIR}/core/utils/ShellCompletion_test.cpp
    ${RAGO_SRC_DIR}/core/utils/QueryCodeExport_test.cpp
    ${RAGO_SRC_DIR}/core/utils/AiQueryAssistant_test.cpp
    ${RAGO_SRC_DIR}/core/utils/AppLog_test.cpp
    ${RAGO_SRC_DIR}/core/utils/AppPaths_test.cpp
    ${RAGO_SRC_DIR}/core/utils/BackgroundPolicy_test.cpp
    ${RAGO_SRC_DIR}/core/utils/LocalNetwork_test.cpp
    ${RAGO_SRC_DIR}/core/utils/Minisign_test.cpp
    ${RAGO_SRC_DIR}/core/utils/UpdatePulse_test.cpp
    ${RAGO_SRC_DIR}/core/utils/UiDensity_test.cpp
    ${RAGO_SRC_DIR}/core/utils/HostKeyPrompter_test.cpp
    ${RAGO_SRC_DIR}/core/utils/ShellSignatures_test.cpp
    ${RAGO_SRC_DIR}/core/mcp/McpConfig_test.cpp
    ${RAGO_SRC_DIR}/core/mcp/McpGuards_test.cpp
    ${RAGO_SRC_DIR}/core/mcp/McpServer_test.cpp
    ${RAGO_SRC_DIR}/core/mcp/McpClientSetup_test.cpp
)

### --- Setup rago_unit_tests exec. (code under test comes from rago-core)
add_executable(rago_unit_tests ${SOURCES_TEST})
target_include_directories(rago_unit_tests PRIVATE ${CMAKE_HOME_DIRECTORY}/src)

target_link_libraries(rago_unit_tests
    gtest
    gtest_main
    rago-core
)
# The recorded results of the shell scripts (ShellCorpus_test.cpp)
target_compile_definitions(rago_unit_tests PRIVATE
    RAGO_SHELL_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shell-corpus")

### --- Deploy the Qt6 runtime for Windows unit tests
if(SYSTEM_WINDOWS)
    if(TARGET Qt6::windeployqt)
        add_custom_command(TARGET rago_unit_tests POST_BUILD
            COMMAND Qt6::windeployqt
                $<IF:$<CONFIG:Debug>,--debug,--release>
                --no-translations
                --no-compiler-runtime
                "$<TARGET_FILE:rago_unit_tests>"
            COMMENT "Deploying Qt6 runtime for rago_unit_tests")
    endif()
endif()