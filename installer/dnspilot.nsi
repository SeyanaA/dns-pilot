; DNS Pilot 安装包（NSIS 3）
; 每用户安装，无需管理员；安装到 %LOCALAPPDATA%\Programs\DNSPilot
; 安装前会优雅关闭正在运行的 DNS Pilot（它会按设置恢复系统 DNS）

Unicode true
!define MUI_ICON "..\assets\dnspilot.ico"
!define MUI_UNICON "..\assets\dnspilot.ico"
!include "MUI2.nsh"
!include "WinMessages.nsh"
!include "LogicLib.nsh"

; 语言 ID：SimpChinese=2052，English=1033
!define LANG_ZH 2052
!define LANG_EN 1033

!define APP_NAME "DNS Pilot"
!define APP_VERSION "1.1.3"
!define APP_EXE "dnspilot.exe"
!define APP_PUBLISHER "DNS Pilot"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\DNSPilot"

Name "${APP_NAME}"
BrandingText "${APP_NAME} ${APP_VERSION}"
OutFile "..\dist\DNS-Pilot-Setup-${APP_VERSION}.exe"
InstallDir "$LOCALAPPDATA\Programs\DNSPilot"
InstallDirRegKey HKCU "${UNINST_KEY}" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 64
ManifestDPIAware true
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "1.1.3.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "FileDescription" "${APP_NAME} 安装程序"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "LegalCopyright" "${APP_PUBLISHER}"
VIAddVersionKey "CompanyName" "${APP_PUBLISHER}"

; ── 界面文案 ────────────────────────────────────────────────────────
LangString STR_APP_RUNNING ${LANG_ZH} "DNS Pilot 正在运行。安装前需要先关闭它（程序会在退出时恢复系统 DNS）。$\r$\n$\r$\n是否立即关闭并继续安装？"
LangString STR_APP_RUNNING ${LANG_EN} "DNS Pilot is running. It must be closed before installing (it restores the system DNS on exit).$\r$\n$\r$\nClose it now and continue?"
LangString STR_SEC_MAIN ${LANG_ZH} "DNS Pilot 主程序（必需）"
LangString STR_SEC_MAIN ${LANG_EN} "DNS Pilot application (required)"
LangString STR_SEC_DESKTOP ${LANG_ZH} "创建桌面快捷方式"
LangString STR_SEC_DESKTOP ${LANG_EN} "Create a desktop shortcut"
LangString STR_UNINST_SHORTCUT ${LANG_ZH} "卸载 DNS Pilot"
LangString STR_UNINST_SHORTCUT ${LANG_EN} "Uninstall DNS Pilot"
LangString STR_RUN_TEXT ${LANG_ZH} "运行 DNS Pilot"
LangString STR_RUN_TEXT ${LANG_EN} "Run DNS Pilot"
LangString STR_SECTION_DESC_MAIN ${LANG_ZH} "安装 DNS Pilot 桌面程序到当前用户目录。"
LangString STR_SECTION_DESC_MAIN ${LANG_EN} "Installs the DNS Pilot desktop application for the current user."
LangString STR_SECTION_DESC_DESKTOP ${LANG_ZH} "在桌面创建 DNS Pilot 快捷方式。"
LangString STR_SECTION_DESC_DESKTOP ${LANG_EN} "Creates a DNS Pilot shortcut on the desktop."

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "$(STR_RUN_TEXT)"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

; ── 安装前检查 ──────────────────────────────────────────────────────
Function .onInit
    SetShellVarContext current
    FindWindow $0 "DNSPilotNative" ""
    ${If} $0 == 0
        Goto done
    ${EndIf}
    ${IfNot} ${Silent}
        MessageBox MB_OKCANCEL|MB_ICONQUESTION "$(STR_APP_RUNNING)" IDOK closeApp
        Abort
    ${EndIf}
closeApp:
    SendMessage $0 ${WM_CLOSE} 0 0
    Sleep 2500
done:
FunctionEnd

; ── 安装 ────────────────────────────────────────────────────────────
Section "$(STR_SEC_MAIN)" SecMain
    SectionIn RO
    SetOutPath "$INSTDIR"
    File "..\${APP_EXE}"

    WriteUninstaller "$INSTDIR\uninstall.exe"

    CreateDirectory "$SMPROGRAMS\${APP_NAME}"
    CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR" 0
    CreateShortcut "$SMPROGRAMS\${APP_NAME}\$(STR_UNINST_SHORTCUT).lnk" "$INSTDIR\uninstall.exe"

    WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${APP_PUBLISHER}"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
    WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
    WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" "$\"$INSTDIR\uninstall.exe$\" /S"
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
    WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" 1300
SectionEnd

Section "$(STR_SEC_DESKTOP)" SecDesktop
    CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR" 0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "$(STR_SECTION_DESC_MAIN)"
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "$(STR_SECTION_DESC_DESKTOP)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ── 卸载 ────────────────────────────────────────────────────────────
Function un.onInit
    SetShellVarContext current
FunctionEnd

Section "Uninstall"
    ; 先优雅关闭（程序退出时会按设置恢复系统 DNS）
    FindWindow $0 "DNSPilotNative" ""
    ${If} $0 != 0
        SendMessage $0 ${WM_CLOSE} 0 0
        Sleep 2500
    ${EndIf}

    Delete "$DESKTOP\${APP_NAME}.lnk"
    RMDir /r "$SMPROGRAMS\${APP_NAME}"

    Delete "$INSTDIR\${APP_EXE}"
    Delete "$INSTDIR\dnspilot.json"
    Delete "$INSTDIR\dnspilot.log"
    Delete "$INSTDIR\dnspilot.pdb"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"

    DeleteRegKey HKCU "${UNINST_KEY}"
SectionEnd
