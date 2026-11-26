# ragoshell: the desktop app (Qt Quick / QML) on top of rago-core.

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# iOS: install/ios/Info.plist.in has its own UILaunchScreen; Qt's default storyboard on top of it
# ends up as an unsigned LaunchScreen.storyboardc that makes codesign fail.
set(QT_NO_SET_DEFAULT_IOS_LAUNCH_SCREEN ON)

qt_add_executable(ragoshell WIN32 MACOSX_BUNDLE
    app/main.cpp
    ThemeBridge.cpp
    ThemeBridge.h
    BrandBridge.cpp
    BrandBridge.h
    PlatformInfo.cpp
    PlatformInfo.h
    QmlBackend.cpp
    QmlBackendUtils.cpp
    QmlBackendAdmin.cpp
    QmlBackendEvents.cpp
    QmlBackend.h
    QmlBackendForeign.h
    QmlSingleton.h
    QmlUiServices.cpp
    QmlUiServices.h
    ConnectionListModel.cpp
    ConnectionListModel.h
    ServerTreeModel.cpp
    ServerTreeModelPopulate.cpp
    ServerTreeModel.h
    ServerMonitorController.cpp
    ServerMonitorController.h
    ScintillaQuick.cpp
    ScintillaQuickInput.cpp
    ScintillaQuick.h
    ShellController.cpp
    ShellControllerResults.cpp
    ShellController.h
    QueryHistoryModel.cpp
    QueryHistoryModel.h
    PipelineBuilder.cpp
    PipelineBuilder.h
    SchemaAnalyzer.cpp
    SchemaAnalyzer.h
    CodeExporter.cpp
    CodeExporter.h
    AiAssistant.cpp
    AiAssistant.h
    CallTipSelfTest.cpp
    CallTipSelfTest.h
    ImeSelfTest.cpp
    ImeSelfTest.h
    MatrixSelfTest.cpp
    MatrixSelfTestTopologies.cpp
    MatrixSelfTest.h
    ShellSelfTest.cpp
    ShellSelfTest.h
    TabModel.cpp
    TabModel.h
    TabModelSelfTest.cpp
    TabModelSelfTest.h
    DraftsSelfTest.cpp
    DraftsSelfTest.h
    ExplorerSelfTest.cpp
    ExplorerSelfTest.h
    LogModel.cpp
    LogModel.h
    QmlObjectListModel.cpp
    QmlObjectListModel.h
    BackgroundMonitor.cpp
    LocalNetworkPermission.cpp
    LocalNetworkPermission.h
    BackgroundMonitor.h
    ScriptDraftModel.cpp
    ScriptDraftModel.h
    McpSettingsBridge.cpp
    McpSettingsBridge.h
    SettingsBridge.cpp
    SettingsBridge.h
    CloudService.cpp
    CloudService.h
    UpdateService.cpp
    UpdateService.h
    LanguageManager.cpp
    LanguageManager.h
    qml.qrc
)

# QML files form the "RagoShell" module URI so qmlcachegen/qmlsc
# compile them ahead of time and qmllint can check them (ragoshell_qmllint).
# Aliases flatten qml/Foo.qml to the module root.
set(RAGO_QML_FILES
    qml/Main.qml
    qml/MainWorkspaceArea.qml
    qml/MainShortcuts.qml
    qml/MainServiceDialogs.qml
    qml/MainDebugHarness.qml
    qml/WelcomePage.qml
    qml/ConnectionsPane.qml
    qml/SavedConnectionsSection.qml
    qml/ExplorerNodeMenu.qml
    qml/ExplorerConfirmDialog.qml
    qml/ConnectionDialog.qml
    qml/ConnectionReplicaSetMembers.qml
    qml/ConnectionAuthTab.qml
    qml/ConnectionSshTab.qml
    qml/ConnectionTlsTab.qml
    qml/ConnectionDiagnosticsFooter.qml
    qml/ShellTabPage.qml
    qml/ShellEditorOverlays.qml
    qml/ShellResultsHeader.qml
    qml/ShellResultTreeView.qml
    qml/ShellResultTableView.qml
    qml/ShellResultsViewport.qml
    qml/ShellResultContextMenu.qml
    qml/ShellConfirmDialogs.qml
    qml/ServerMonitorPage.qml
    qml/ServerMonitorOpsPane.qml
    qml/RagoCodeEditor.qml
    qml/QueryBar.qml
    qml/DocumentEditorDialog.qml
    qml/CreateCollectionDialog.qml
    qml/UsersManagerDialog.qml
    qml/UserEditorDialog.qml
    qml/FunctionEditorDialog.qml
    qml/ShortcutsDialog.qml
    qml/ImportExportDialog.qml
    qml/LogPanel.qml
    qml/PreferencesDialog.qml
    qml/PreferencesSchemaSection.qml
    qml/PreferencesMcpSection.qml
    qml/PreferencesAccountSection.qml
    qml/CloudSyncDialog.qml
    qml/MandatoryUpdateDialog.qml
    qml/AboutDialog.qml
    qml/LicensesDialog.qml
    qml/InputDialog.qml
    qml/LazyDialog.qml
    qml/AppActions.qml
    qml/AppBar.qml
    qml/StripButton.qml
    qml/VisibleArea.qml
    qml/FocusReveal.qml
    qml/HScrollRow.qml
    qml/CommandMenuItem.qml
    qml/CommandPalette.qml
    qml/HistoryDialog.qml
    qml/RecoverDraftsDialog.qml
    qml/HostKeyDialog.qml
    qml/ExplainView.qml
    qml/PipelineBuilderDialog.qml
    qml/SchemaDialog.qml
    qml/ExportCodeDialog.qml
    qml/AiAssistantDialog.qml
    qml/IndexDialog.qml
    qml/IndexFilterCollationTab.qml
    qml/IndexTextSearchTab.qml
    qml/RagoPillButton.qml
    qml/RagoIconButton.qml
    qml/RagoStatusDot.qml
    qml/RagoGlassCard.qml
    qml/RagoBadge.qml
    qml/RagoDialog.qml
    qml/RagoTextField.qml
    qml/RagoComboBox.qml
    qml/RagoCheckBox.qml
    qml/RagoSpinBox.qml
    qml/RagoMenu.qml
    qml/RagoMenuItem.qml
    qml/RagoMenuSeparator.qml
)
foreach(qml_file IN LISTS RAGO_QML_FILES)
    get_filename_component(qml_name "${qml_file}" NAME)
    set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
endforeach()

# Visual Studio generators run AUTOMOC as a PRE_BUILD event and create no
# <target>_autogen target, which QML type registration depends on. A file
# dependency makes CMake emit the real autogen target.
set_property(TARGET ragoshell APPEND PROPERTY
    AUTOGEN_TARGET_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ScintillaQuick.h")

qt_add_qml_module(ragoshell
    URI RagoShell
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    QML_FILES ${RAGO_QML_FILES}
)

# ---- UI translations (docs/Localization.md) --------------------------------
# One <product id>_<lang>.ts per language in i18n/ (ragoshell_de.ts; source language: English;
# ragoshell_en.ts only carries the plural forms). The .qm files are embedded
# under :/i18n/ and loaded by LanguageManager. Adding a language: a row in
# core/utils/LanguageCatalog.cpp, the code below, and the .ts file.
#   cmake --build <dir> --target update_translations   # refresh the .ts files
set(RAGO_UI_LANGUAGES hi zh_CN es pt_BR ru de fr ja ar tr az kk ky uz)
set(QT_I18N_SOURCE_LANGUAGE en)
set(QT_I18N_TRANSLATED_LANGUAGES ${RAGO_UI_LANGUAGES})
qt_add_translations(ragoshell
    SOURCE_TARGETS ragoshell rago-core
    TS_FILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/i18n"
    TS_FILE_BASE ${RAGO_PRODUCT_ID}
    RESOURCE_PREFIX /i18n
    LUPDATE_OPTIONS -no-obsolete -locations none)

# Qt's own catalogs (qtbase: standard buttons, text-edit menus; qtdeclarative:
# Qt Quick). Embedded for the languages Qt ships so every platform behaves the
# same; windeployqt runs with --no-translations.
set(RAGO_QT_TRANSLATIONS_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}")
set(RAGO_QT_QM_FILES)
foreach(lang IN LISTS RAGO_UI_LANGUAGES)
    foreach(catalog qtbase qtdeclarative)
        set(qt_qm "${RAGO_QT_TRANSLATIONS_DIR}/${catalog}_${lang}.qm")
        if(EXISTS "${qt_qm}")
            set_source_files_properties("${qt_qm}" PROPERTIES QT_RESOURCE_ALIAS "${catalog}_${lang}.qm")
            list(APPEND RAGO_QT_QM_FILES "${qt_qm}")
        endif()
    endforeach()
endforeach()
if(RAGO_QT_QM_FILES)
    qt_add_resources(ragoshell rago_qt_translations
        PREFIX /i18n/qt
        FILES ${RAGO_QT_QM_FILES})
else()
    message(WARNING "No Qt translation files found in ${RAGO_QT_TRANSLATIONS_DIR}; "
                    "Qt's own dialogs stay English.")
endif()

# The icons in the UI are Unicode symbols (check marks, arrows, dingbats). Phones and tablets
# do not ship fonts that have all of them (Android 10 shows empty boxes), so the app carries
# DejaVu Sans (assets/fonts/DEJAVU-LICENSE) as a fallback font. Desktop systems have them.
if(SYSTEM_MOBILE)
    qt_add_resources(ragoshell rago_symbol_font
        PREFIX /fonts
        BASE "${CMAKE_SOURCE_DIR}/assets/fonts"
        FILES "${CMAKE_SOURCE_DIR}/assets/fonts/DejaVuSans.ttf")
endif()

target_link_libraries(ragoshell
    PRIVATE
        rago-core
        qscintilla
        Qt6::Gui
        Qt6::Network
        Qt6::Widgets
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Threads::Threads)

target_include_directories(ragoshell
    PRIVATE
        ${CMAKE_HOME_DIRECTORY}/src)

# The shell is the QJSEngine runtime compiled into rago-core (core/engine/js/shell-runtime.js): there is
# nothing to stage next to the executable.

if(SYSTEM_WINDOWS)
    set(windows_icon "${CMAKE_SOURCE_DIR}/install/windows/ragoshell.ico")
    configure_file(
        "${CMAKE_SOURCE_DIR}/install/windows/winres.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/winres.rc")
    unset(windows_icon)
    target_sources(ragoshell PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/winres.rc")

    set_target_properties(ragoshell PROPERTIES
        OUTPUT_NAME "${RAGO_PRODUCT_ID}")
    target_link_libraries(ragoshell PRIVATE Qt6::EntryPointPrivate)

    if(TARGET Qt6::windeployqt)
        add_custom_command(TARGET ragoshell POST_BUILD
            COMMAND Qt6::windeployqt
                $<IF:$<CONFIG:Debug>,--debug,--release>
                --no-translations
                --no-compiler-runtime
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                "$<TARGET_FILE:ragoshell>"
            COMMENT "Deploying Qt and QML runtime for ragoshell")
    endif()
elseif(SYSTEM_MACOSX)
    set(macos_icon "${CMAKE_SOURCE_DIR}/install/macosx/ragoshell.icns")
    set_source_files_properties("${macos_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(ragoshell PRIVATE "${macos_icon}")
    string(TIMESTAMP CURRENT_YEAR "%Y")
    set_target_properties(ragoshell PROPERTIES
        OUTPUT_NAME "${RAGO_PRODUCT_NAME}"
        INSTALL_RPATH "@executable_path/../Frameworks"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/install/macosx/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "${RAGO_PRODUCT_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${RAGO_BUNDLE_ID}"
        MACOSX_BUNDLE_INFO_STRING "${RAGO_PRODUCT_NAME} ${PROJECT_VERSION}. Copyright ${RAGO_PRODUCT_NAME} contributors, 2014 - ${CURRENT_YEAR}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright ${RAGO_PRODUCT_NAME} contributors, 2014 - ${CURRENT_YEAR}"
        MACOSX_BUNDLE_ICON_FILE "ragoshell")
    set(RAGO_MACOS_CODESIGN_IDENTITY "" CACHE STRING
        "Optional macOS codesign identity (e.g. 'Apple Development' or 'Developer ID Application: ...') for stable Keychain ACLs across rebuilds")
    if(RAGO_MACOS_CODESIGN_IDENTITY)
        add_custom_command(TARGET ragoshell POST_BUILD
            COMMAND codesign --force --deep
                --entitlements "${CMAKE_SOURCE_DIR}/install/macosx/entitlements.plist"
                -s "${RAGO_MACOS_CODESIGN_IDENTITY}"
                "$<TARGET_BUNDLE_DIR:ragoshell>"
            COMMENT "Signing ${RAGO_PRODUCT_NAME}.app with ${RAGO_MACOS_CODESIGN_IDENTITY}")
    endif()
elseif(SYSTEM_ANDROID)
    # Phones and tablets (docs/Platforms.md). androiddeployqt and Qt6AndroidGradleHelpers read
    # the QT_ANDROID_* properties to populate build.gradle (minSdk = 28, targetSdk = 37) and AndroidManifest.xml.
    math(EXPR RAGO_ANDROID_VERSION_CODE "${PROJECT_VERSION_MAJOR} * 1000000 + ${PROJECT_VERSION_MINOR} * 10000 + ${PROJECT_VERSION_PATCH} * 100")
    set_target_properties(ragoshell PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/install/android"
        QT_ANDROID_PACKAGE_NAME "${RAGO_BUNDLE_ID}"
        QT_ANDROID_APP_NAME "${RAGO_PRODUCT_NAME}"
        QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}"
        QT_ANDROID_VERSION_CODE "${RAGO_ANDROID_VERSION_CODE}"
        QT_ANDROID_MIN_SDK_VERSION "${RAGO_ANDROID_MIN_SDK}"
        QT_ANDROID_TARGET_SDK_VERSION "${RAGO_ANDROID_TARGET_SDK}")
    if(RAGO_ANDROID_COMPILE_SDK)
        set_target_properties(ragoshell PROPERTIES
            QT_ANDROID_COMPILE_SDK_VERSION "${RAGO_ANDROID_COMPILE_SDK}")
    endif()
    # Enforce 16 KB ELF LOAD segment alignment (p_align >= 0x4000) required by Android 15+ and Google Play.
    target_link_options(ragoshell PRIVATE "-Wl,-z,max-page-size=16384")
    target_link_libraries(ragoshell PRIVATE android)

    # Qt Network needs OpenSSL on Android (AI assistant / HTTPS updates; M15).
    # Point RAGO_ANDROID_OPENSSL_DIR at an extracted Qt "android_openssl" tree
    # (ssl_3/<abi>/lib{crypto,ssl}_3.so); env or -D both work.
    if(NOT DEFINED RAGO_ANDROID_OPENSSL_DIR)
        set(RAGO_ANDROID_OPENSSL_DIR "$ENV{RAGO_ANDROID_OPENSSL_DIR}")
    endif()
    if(RAGO_ANDROID_OPENSSL_DIR)
        set(_abi_dir "${RAGO_ANDROID_OPENSSL_DIR}/ssl_3/${ANDROID_ABI}")
        if(EXISTS "${_abi_dir}/libcrypto_3.so" AND EXISTS "${_abi_dir}/libssl_3.so")
            set_target_properties(ragoshell PROPERTIES
                QT_ANDROID_EXTRA_LIBS "${_abi_dir}/libcrypto_3.so;${_abi_dir}/libssl_3.so")
            message(STATUS "Android OpenSSL bundled from ${_abi_dir}")
        else()
            message(WARNING "RAGO_ANDROID_OPENSSL_DIR set but ssl_3/${ANDROID_ABI} libs missing "
                            "— Qt Network on Android will have no TLS")
        endif()
    else()
        message(STATUS "RAGO_ANDROID_OPENSSL_DIR unset: Android build has no OpenSSL "
                       "(AI assistant and other HTTPS calls unavailable on Android)")
    endif()
elseif(SYSTEM_IOS)
    # iPhone and iPad (docs/Platforms.md): Info.plist, privacy manifest, deployment floor (18.0) and bundle identity.
    # Base SDK stays "iphoneos" (latest installed Xcode iOS SDK, as required by App Store Connect; no upper OS cap).
    set_target_properties(ragoshell PROPERTIES
        OUTPUT_NAME "${RAGO_PRODUCT_NAME}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/install/ios/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "${RAGO_PRODUCT_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${RAGO_BUNDLE_ID}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${RAGO_BUNDLE_ID}"
        # The app icon set (light, dark and tinted 1024 px icons, dev/icons/make-icons.sh); Xcode compiles it into
        # Assets.car and adds CFBundleIcons to the Info.plist.
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon")
    # A catalog is built only from the "Copy Bundle Resources" phase: the RESOURCE property puts it there.
    target_sources(ragoshell PRIVATE "${CMAKE_SOURCE_DIR}/install/ios/Assets.xcassets")
    set_target_properties(ragoshell PROPERTIES RESOURCE "${CMAKE_SOURCE_DIR}/install/ios/Assets.xcassets")
    set(rago_privacy_manifest "${CMAKE_SOURCE_DIR}/install/ios/PrivacyInfo.xcprivacy")
    set_source_files_properties("${rago_privacy_manifest}" PROPERTIES MACOSX_PACKAGE_LOCATION "")
    target_sources(ragoshell PRIVATE "${rago_privacy_manifest}")
    # Localized system permission prompts (NSLocalNetworkUsageDescription, NSPhotoLibraryUsageDescription)
    # placed at the root of the flat iOS .app bundle (<lang>.lproj/InfoPlist.strings — never under Resources/).
    # Xcode automatically preserves the parent <lang>.lproj directory for localized .strings files when
    # MACOSX_PACKAGE_LOCATION is "".
    set(RAGO_IOS_LPROJ_LANGUAGES en hi zh-Hans es pt-BR ru de fr ja ar tr az kk ky uz)
    foreach(rago_ios_lproj IN LISTS RAGO_IOS_LPROJ_LANGUAGES)
        set(rago_ios_infoplist_strings "${CMAKE_SOURCE_DIR}/install/ios/lproj/${rago_ios_lproj}.lproj/InfoPlist.strings")
        set_source_files_properties("${rago_ios_infoplist_strings}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "")
        target_sources(ragoshell PRIVATE "${rago_ios_infoplist_strings}")
    endforeach()
    # Exclude unused Darwin hardware permission plugins (Camera, Location, Contacts, Bluetooth,
    # Calendar, Microphone) so static iOS builds never pull restricted frameworks into the binary.
    qt_import_plugins(ragoshell EXCLUDE_BY_TYPE permissions)
    # Is a hardware keyboard attached (shortcut text is shown only then): GameController
    target_sources(ragoshell PRIVATE PlatformInfo_ios.mm)
    target_link_libraries(ragoshell PRIVATE "-framework GameController")
elseif(SYSTEM_LINUX)
    set_target_properties(ragoshell PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
        OUTPUT_NAME "${RAGO_PRODUCT_ID}")
    # Executables, launcher entry, AppStream metainfo and icon theme (hicolor, 16 to 512 px and scalable):
    # cmake --install puts them where the desktop looks for them; ci/build-linux.sh stages the same files
    # into the tar.gz archive and the self-contained AppImage AppDir.
    include(GNUInstallDirs)
    install(TARGETS ragoshell RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/install/linux/icons/hicolor" DESTINATION "${CMAKE_INSTALL_DATADIR}/icons")
    install(FILES "${CMAKE_SOURCE_DIR}/install/linux/${RAGO_PRODUCT_ID}.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES "${CMAKE_SOURCE_DIR}/install/linux/${RAGO_BUNDLE_ID}.metainfo.xml" DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
    install(FILES
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "${CMAKE_SOURCE_DIR}/LICENSE.LGPLv3"
        "${CMAKE_SOURCE_DIR}/COPYRIGHT"
        "${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}")
elseif(SYSTEM_WASM)
    # IDBFS persistence (docs/WebStorage.md §1, roadmap 8.2): --pre-js mounts
    # IDBFS on /persist and delays main() until IndexedDB is populated.
    target_link_options(ragoshell PRIVATE
        "SHELL:--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/wasm/persist-pre.js"
        "-lidbfs.js")
    # Mod A playground (docs/WebAssembly.md): brotli-compress the .wasm next to
    # ragoshell.js/.html so the site can serve precompressed output.
    find_program(RAGO_BROTLI_EXECUTABLE NAMES brotli)
    if(RAGO_BROTLI_EXECUTABLE)
        add_custom_command(TARGET ragoshell POST_BUILD
            COMMAND "${RAGO_BROTLI_EXECUTABLE}" -9 -k -f
                "$<TARGET_FILE_DIR:ragoshell>/ragoshell.wasm"
            COMMENT "Brotli-compressing ragoshell.wasm -> ragoshell.wasm.br")
    else()
        message(WARNING "brotli not found — ragoshell.wasm.br will not be generated")
    endif()
endif()
