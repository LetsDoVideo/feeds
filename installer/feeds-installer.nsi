; Feeds - Isolated Feeds for OBS
; NSIS Installer Script

!define PRODUCT_NAME "Feeds - Isolated Feeds for OBS"
!define PRODUCT_VERSION "1.0"
!define PRODUCT_PUBLISHER "LetsDoVideo"
!define PRODUCT_WEB_SITE "https://letsdovideo.com/feeds"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Feeds-OBS"
!define FEEDS_REG_KEY "Software\LetsDoVideo\Feeds"

; Modern UI
!include "MUI2.nsh"
!include "LogicLib.nsh"

Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "Feeds-Setup.exe"
InstallDir "$PROGRAMFILES64\obs-studio"
InstallDirRegKey HKLM "SOFTWARE\OBS Studio" ""
RequestExecutionLevel admin
SetCompressor /SOLID lzma

; UI Settings
!define MUI_ABORTWARNING
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"
!define MUI_WELCOMEPAGE_TITLE "Welcome to Feeds Setup"
!define MUI_WELCOMEPAGE_TEXT "This will install Feeds - Isolated Feeds for OBS $\r$\n$\r$\nFeeds allows you to pull individual Zoom participant video feeds directly into OBS.$\r$\n$\r$\nClick Next to continue."
!define MUI_FINISHPAGE_TITLE "Feeds Installation Complete"
!define MUI_FINISHPAGE_TEXT "Feeds has been installed successfully.$\r$\n$\r$\nStart OBS and look for the Feeds menu in the menu bar to get started."

; Pages
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${ROOT_DIR}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------
; VC++ 2015-2022 REDISTRIBUTABLE CHECK
; Silently installs the MSVC runtime if not already present.
; The redist installer is bundled in the installer package.
; ---------------------------------------------------------------------------
Section "-VCRedist" SecVCRedist
    ; Check if VC++ 2015-2022 x64 redist is already installed by looking
    ; for the registry key it writes on install
    ReadRegDWORD $0 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Installed"
    ${If} $0 == 1
        ; Already installed — skip
        Goto VCRedistDone
    ${EndIf}

    ; Not found — install silently
    SetOutPath "$TEMP"
    File "${ROOT_DIR}\installer\VC_redist.x64.exe"
    ExecWait '"$TEMP\VC_redist.x64.exe" /install /quiet /norestart'
    Delete "$TEMP\VC_redist.x64.exe"

    VCRedistDone:
SectionEnd

; ---------------------------------------------------------------------------
; INSTALLER SECTIONS
; ---------------------------------------------------------------------------
Section "Feeds Plugin" SecMain
    SectionIn RO  ; Required, cannot be deselected

    SetOverwrite on

    ; Plugin DLL -> obs-plugins/64bit/
    SetOutPath "$INSTDIR\obs-plugins\64bit"
    File "${ROOT_DIR}\dist\obs-plugins\64bit\feeds.dll"

    ; Locale -> data/obs-plugins/feeds/locale/
    SetOutPath "$INSTDIR\data\obs-plugins\feeds\locale"
    File "${ROOT_DIR}\dist\data\obs-plugins\feeds\locale\en-US.ini"

    ; Zoom SDK runtime DLLs -> bin/64bit/
    SetOutPath "$INSTDIR\bin\64bit"
    File "${ROOT_DIR}\dist\bin\64bit\*.dll"
    File "${ROOT_DIR}\dist\bin\64bit\*.exe"

    ; Zoom SDK language files -> bin/64bit/language/
    SetOutPath "$INSTDIR\bin\64bit\language"
    File "${ROOT_DIR}\dist\bin\64bit\language\*.*"

    ; Zoom SDK ringtone files -> bin/64bit/ringtone/
    SetOutPath "$INSTDIR\bin\64bit\ringtone"
    File "${ROOT_DIR}\dist\bin\64bit\ringtone\*.*"

    ; Write uninstaller
    WriteUninstaller "$INSTDIR\Feeds-Uninstall.exe"

    ; Write registry for Add/Remove Programs
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "UninstallString" "$INSTDIR\Feeds-Uninstall.exe"
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegDWORD HKLM "${PRODUCT_UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${PRODUCT_UNINST_KEY}" "NoRepair" 1

SectionEnd

; ---------------------------------------------------------------------------
; UNINSTALLER
; ---------------------------------------------------------------------------
Section "Uninstall"

    ; Remove plugin files
    Delete "$INSTDIR\obs-plugins\64bit\feeds.dll"
    Delete "$INSTDIR\data\obs-plugins\feeds\locale\en-US.ini"
    RMDir "$INSTDIR\data\obs-plugins\feeds\locale"
    RMDir "$INSTDIR\data\obs-plugins\feeds"

    ; Remove uninstaller
    Delete "$INSTDIR\Feeds-Uninstall.exe"

    ; Remove registry entries
    DeleteRegKey HKLM "${PRODUCT_UNINST_KEY}"

    ; Remove stored tokens (user data) - optional, ask user
    MessageBox MB_YESNO "Remove saved Zoom login credentials?" IDNO SkipTokens
        DeleteRegKey HKCU "${FEEDS_REG_KEY}"
    SkipTokens:

    ; Note: We do NOT remove Zoom SDK DLLs from bin/64bit as other
    ; apps may depend on them

    MessageBox MB_OK "Feeds has been uninstalled successfully."

SectionEnd

; ---------------------------------------------------------------------------
; DIRECTORY PAGE CUSTOMIZATION
; Show a warning if the selected directory doesn't look like OBS
; ---------------------------------------------------------------------------
Function .onVerifyInstDir
    IfFileExists "$INSTDIR\obs64.exe" DirOK
    IfFileExists "$INSTDIR\bin\64bit\obs64.exe" DirOK
        MessageBox MB_YESNO \
            "The selected folder does not appear to be an OBS installation.$\r$\n$\r$\nInstall here anyway?" \
            IDYES DirOK
        Abort
    DirOK:
FunctionEnd
