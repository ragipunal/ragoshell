; Windows x64 Installer (NSIS 3.x). The names (APP_NAME, APP_ID, APP_EXE, ICON_NAME) come from
; cmake/RagoShellDefaults.cmake through ci/build-windows.ps1 (docs/Brand.md); a hand-run
; makensis must pass /INPUTCHARSET UTF8, /DAPP_NAME and /DAPP_ID itself. Upgrades in-place while preserving the user's settings.

Unicode True
SetCompressor /SOLID lzma
SetCompressorDictSize 64

!ifndef APP_NAME
  !error "APP_NAME is required: run ci/build-windows.ps1 (it reads cmake/RagoShellDefaults.cmake)"
!endif
!ifndef APP_ID
  !error "APP_ID is required: run ci/build-windows.ps1 (it reads cmake/RagoShellDefaults.cmake)"
!endif
!define /IfNDef APP_PUBLISHER "${APP_NAME}"
!define /IfNDef APP_URL ""
!define /IfNDef APP_EXE "${APP_ID}.exe"
!define /IfNDef MCP_EXE "${APP_ID}-mcp.exe"
!define /IfNDef APP_VERSION "2.1.2"
!define /IfNDef APP_VERSION_NUMERIC "2.1.2.0"
!define /IfNDef STAGE_DIR "..\..\dist\${APP_ID}-${APP_VERSION}-windows-x64"
!define /IfNDef OUT_FILE "..\..\dist\${APP_ID}-${APP_VERSION}-windows-x64-setup.exe"
!define /IfNDef ICON_NAME "${APP_ID}.ico"
!define /IfNDef ICON_FILE "${ICON_NAME}"
!define /IfNDef SIDEBAR_BMP "nsis-sidebar.bmp"
!define /IfNDef TOPBAR_BMP "nsis-topbar.bmp"
!define /IfNDef LICENSE_FILE "..\..\LICENSE"

!define REG_APP_KEY "Software\${APP_NAME}"
!define REG_UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"

Name "${APP_NAME} ${APP_VERSION}"
OutFile "${OUT_FILE}"
BrandingText "${APP_NAME} ${APP_VERSION}"

VIProductVersion "${APP_VERSION_NUMERIC}"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey "FileDescription" "${APP_NAME} Setup"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "LegalCopyright" "Copyright (C) ${APP_NAME} Contributors"

; Multi-user setup (AllUsers -> %ProgramFiles%\${APP_NAME}, CurrentUser -> %LocalAppData%\Programs\${APP_NAME})
; Automatically reuses previous InstallMode & InstallLocation on upgrades.
!define MULTIUSER_EXECUTIONLEVEL Highest
!define MULTIUSER_MUI
!define MULTIUSER_INSTALLMODE_COMMANDLINE
!define MULTIUSER_USE_PROGRAMFILES64
!define MULTIUSER_INSTALLMODE_INSTDIR "${APP_NAME}"
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_KEY "${REG_APP_KEY}"
!define MULTIUSER_INSTALLMODE_DEFAULT_REGISTRY_VALUENAME "InstallMode"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_KEY "${REG_APP_KEY}"
!define MULTIUSER_INSTALLMODE_INSTDIR_REGISTRY_VALUENAME "InstallLocation"

!include "MultiUser.nsh"
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

!define MUI_ABORTWARNING
!define MUI_ICON "${ICON_FILE}"
!define MUI_UNICON "${ICON_FILE}"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "${TOPBAR_BMP}"
!define MUI_HEADERIMAGE_RIGHT
!define MUI_WELCOMEFINISHPAGE_BITMAP "${SIDEBAR_BMP}"
!define MUI_WELCOMEFINISHPAGE_BITMAP_STRETCH FitControl
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "${SIDEBAR_BMP}"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP_STRETCH FitControl

; Wizard Pages
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!insertmacro MULTIUSER_PAGE_INSTALLMODE
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APP_NAME}"
!insertmacro MUI_PAGE_FINISH

; Uninstaller Pages
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; Installer languages follow the OS language; English comes first and is the fallback.
; A language file the local NSIS lacks is skipped, so an older NSIS still builds.
; (No NSIS language exists for Azerbaijani, Kazakh and Kyrgyz: those users get English.)
!insertmacro MUI_LANGUAGE "English"
!macro RagoLanguage name
  !if /FileExists "${NSISDIR}\Contrib\Language files\${name}.nlf"
    !insertmacro MUI_LANGUAGE "${name}"
  !endif
!macroend
!insertmacro RagoLanguage "German"
!insertmacro RagoLanguage "French"
!insertmacro RagoLanguage "Spanish"
!insertmacro RagoLanguage "PortugueseBR"
!insertmacro RagoLanguage "Russian"
; NSIS ships the Turkish and Uzbek wizard text without the MultiUser page strings;
; predefining them before MUI_LANGUAGE feeds LANGFILE_INCLUDE_WITHDEFAULT directly
; (avoiding both English-fallback warnings and duplicate LangString 6030 warnings).
!if /FileExists "${NSISDIR}\Contrib\Language files\Turkish.nlf"
  !define MULTIUSER_TEXT_INSTALLMODE_TITLE "Kullanıcı Seçimi"
  !define MULTIUSER_TEXT_INSTALLMODE_SUBTITLE "$(^NameDA) uygulamasının hangi kullanıcılar için kurulacağını seçin."
  !define MULTIUSER_INNERTEXT_INSTALLMODE_TOP "$(^NameDA) uygulamasını yalnızca kendiniz için mi yoksa bu bilgisayarı kullanan herkes için mi kurmak istediğinizi seçin. $(^ClickNext)"
  !define MULTIUSER_INNERTEXT_INSTALLMODE_ALLUSERS "Bu bilgisayarı kullanan herkes için kur"
  !define MULTIUSER_INNERTEXT_INSTALLMODE_CURRENTUSER "Yalnızca benim için kur"
  !insertmacro MUI_LANGUAGE "Turkish"
!endif
!insertmacro RagoLanguage "Japanese"
!insertmacro RagoLanguage "SimpChinese"
!insertmacro RagoLanguage "Arabic"
!insertmacro RagoLanguage "Hindi"
!if /FileExists "${NSISDIR}\Contrib\Language files\Uzbek.nlf"
  !define MULTIUSER_TEXT_INSTALLMODE_TITLE "Foydalanuvchilarni tanlash"
  !define MULTIUSER_TEXT_INSTALLMODE_SUBTITLE "$(^NameDA) qaysi foydalanuvchilar uchun o'rnatilishini tanlang."
  !define MULTIUSER_INNERTEXT_INSTALLMODE_TOP "$(^NameDA) faqat o'zingiz uchun yoki ushbu kompyuterning barcha foydalanuvchilari uchun o'rnatilishini tanlang. $(^ClickNext)"
  !define MULTIUSER_INNERTEXT_INSTALLMODE_ALLUSERS "Bu kompyuterning barcha foydalanuvchilari uchun o'rnatish"
  !define MULTIUSER_INNERTEXT_INSTALLMODE_CURRENTUSER "Faqat men uchun o'rnatish"
  !insertmacro MUI_LANGUAGE "Uzbek"
!endif

Var CmdLineInstDir

Function .onInit
  SetRegView 64
  StrCpy $CmdLineInstDir $INSTDIR
  !insertmacro MULTIUSER_INIT
  ${If} $CmdLineInstDir != ""
    StrCpy $INSTDIR $CmdLineInstDir
  ${EndIf}
FunctionEnd

Function un.onInit
  SetRegView 64
  !insertmacro MULTIUSER_UNINIT
FunctionEnd

; Terminate the GUI app and any background helper processes (such as the stdio MCP
; server spawned by AI IDEs/clients) so no process holds a lock on Qt6Core.dll or
; driver DLLs in $INSTDIR during upgrade or uninstall.
!macro StopProductProcesses
  nsExec::ExecToStack 'taskkill /F /T /IM "${APP_EXE}"'
  Pop $0
  Pop $1
  nsExec::ExecToStack 'taskkill /F /T /IM "${MCP_EXE}"'
  Pop $0
  Pop $1
  nsExec::ExecToStack 'taskkill /F /T /IM "${APP_ID}-agent.exe"'
  Pop $0
  Pop $1
  nsExec::ExecToStack 'taskkill /F /T /IM "${APP_ID}-web-gateway.exe"'
  Pop $0
  Pop $1
  Sleep 400
!macroend

Section "${APP_NAME}" SecMain
  SectionIn RO

  ; Close any running instances so in-place upgrade can replace binaries cleanly
  !insertmacro StopProductProcesses

  ; Remove executables immediately first so an MCP host client cannot respawn
  ; ${MCP_EXE} and re-lock Qt6Core.dll while old plugin directories are being cleaned.
  Delete "$INSTDIR\${MCP_EXE}"
  Delete "$INSTDIR\${APP_EXE}"
  ${If} ${FileExists} "$INSTDIR\${MCP_EXE}"
  ${OrIf} ${FileExists} "$INSTDIR\${APP_EXE}"
    !insertmacro StopProductProcesses
    Delete "$INSTDIR\${MCP_EXE}"
    Delete "$INSTDIR\${APP_EXE}"
  ${EndIf}

  ; Clean previous version's application binaries/modules in $INSTDIR on upgrade.
  ; NOTE: The user's settings folder in %USERPROFILE% is intentionally NEVER touched.
  ${If} ${FileExists} "$INSTDIR\Uninstall.exe"
  ${OrIf} ${FileExists} "$INSTDIR\Qt6Core.dll"
  ${OrIf} ${FileExists} "$INSTDIR\${ICON_NAME}"
    DetailPrint "Upgrading existing ${APP_NAME} installation in $INSTDIR..."
    ; rago-shell and node.exe (removed with the *.exe below) belong to versions before the embedded shell
    RMDir /r "$INSTDIR\rago-shell"
    RMDir /r "$INSTDIR\qml"
    RMDir /r "$INSTDIR\generic"
    RMDir /r "$INSTDIR\iconengines"
    RMDir /r "$INSTDIR\imageformats"
    RMDir /r "$INSTDIR\networkinformation"
    RMDir /r "$INSTDIR\platforminputcontexts"
    RMDir /r "$INSTDIR\platforms"
    RMDir /r "$INSTDIR\qmltooling"
    RMDir /r "$INSTDIR\styles"
    RMDir /r "$INSTDIR\tls"
    RMDir /r "$INSTDIR\translations"
    RMDir /r "$INSTDIR\vectorimageformats"
    RMDir /r "$INSTDIR\licenses"
    Delete "$INSTDIR\*.dll"
    Delete "$INSTDIR\*.exe"
    Delete "$INSTDIR\qt.conf"
  ${EndIf}

  SetOutPath "$INSTDIR"
  SetOverwrite on
  ; Extract all libraries, plugins, QML modules and licenses BEFORE the executables so
  ; background MCP clients polling for ${MCP_EXE} cannot start it mid-extraction and lock DLLs.
  File /r /x "${MCP_EXE}" /x "${APP_EXE}" "${STAGE_DIR}\*"
  File "${STAGE_DIR}\${APP_EXE}"
  File "${STAGE_DIR}\${MCP_EXE}"
  File "/oname=${ICON_NAME}" "${ICON_FILE}"

  ; Uninstaller
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Start Menu & Desktop shortcuts pointing to the QML application (${APP_EXE})
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  SetOutPath "$INSTDIR"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" \
    "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${ICON_NAME}" 0 SW_SHOWNORMAL "" \
    "${APP_NAME} - MongoDB Management Tool"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" \
    "$INSTDIR\Uninstall.exe" "/$MultiUser.InstallMode" "$INSTDIR\Uninstall.exe" 0

  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" \
    "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${ICON_NAME}" 0 SW_SHOWNORMAL "" \
    "${APP_NAME} - MongoDB Management Tool"

  ; Persist installation metadata for seamless future upgrades & Add/Remove Programs
  WriteRegStr SHCTX "${REG_APP_KEY}" "InstallMode" "$MultiUser.InstallMode"
  WriteRegStr SHCTX "${REG_APP_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr SHCTX "${REG_APP_KEY}" "Version" "${APP_VERSION}"

  WriteRegStr SHCTX "${REG_UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr SHCTX "${REG_UNINSTALL_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr SHCTX "${REG_UNINSTALL_KEY}" "Publisher" "${APP_PUBLISHER}"
!if "${APP_URL}" != ""
  WriteRegStr SHCTX "${REG_UNINSTALL_KEY}" "URLInfoAbout" "${APP_URL}"
!endif
  WriteRegStr SHCTX "${REG_UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\${ICON_NAME}"
  WriteRegStr SHCTX "${REG_UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr SHCTX "${REG_UNINSTALL_KEY}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\" /$MultiUser.InstallMode"
  WriteRegStr SHCTX "${REG_UNINSTALL_KEY}" "QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /$MultiUser.InstallMode /S"
  WriteRegDWORD SHCTX "${REG_UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD SHCTX "${REG_UNINSTALL_KEY}" "NoRepair" 1

  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD SHCTX "${REG_UNINSTALL_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "Uninstall"
  !insertmacro StopProductProcesses
  Delete "$INSTDIR\${MCP_EXE}"
  Delete "$INSTDIR\${APP_EXE}"

  Delete "$DESKTOP\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"

  ; Remove application installation files only (never touch the settings folder in %USERPROFILE%)
  ; (rago-shell: the Node shell folder of versions before the embedded shell)
  RMDir /r "$INSTDIR\rago-shell"
  RMDir /r "$INSTDIR\qml"
  RMDir /r "$INSTDIR\generic"
  RMDir /r "$INSTDIR\iconengines"
  RMDir /r "$INSTDIR\imageformats"
  RMDir /r "$INSTDIR\networkinformation"
  RMDir /r "$INSTDIR\platforminputcontexts"
  RMDir /r "$INSTDIR\platforms"
  RMDir /r "$INSTDIR\qmltooling"
  RMDir /r "$INSTDIR\styles"
  RMDir /r "$INSTDIR\tls"
  RMDir /r "$INSTDIR\translations"
  RMDir /r "$INSTDIR\vectorimageformats"
  RMDir /r "$INSTDIR\licenses"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\*.exe"
  Delete "$INSTDIR\*.ico"
  Delete "$INSTDIR\qt.conf"
  RMDir "$INSTDIR"

  DeleteRegKey SHCTX "${REG_UNINSTALL_KEY}"
  DeleteRegKey SHCTX "${REG_APP_KEY}"
SectionEnd

; Sign and verify the generated Uninstall.exe at compile time when ci/build-windows.ps1 enables code signing (NSIS 3.08+).
!ifdef UNINST_SIGN_CMD
  !uninstfinalize `${UNINST_SIGN_CMD}` = 0
!endif
