Unicode True
RequestExecutionLevel user

!define APP_NAME "14a Bridge"
!define APP_VERSION "1.1.0"
!define APP_PUBLISHER "MES"
!define APP_EXE "14a_Bridge.exe"
!define APP_DIR "$LOCALAPPDATA\Programs\MES\14a Bridge"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\14aBridge"

Name "${APP_NAME} ${APP_VERSION}"
OutFile "..\dist\14a_Bridge_Setup_V1.1.0.exe"
InstallDir "${APP_DIR}"
InstallDirRegKey HKCU "${UNINSTALL_KEY}" "InstallLocation"
Icon "..\release\mes_logo.ico"
UninstallIcon "..\release\mes_logo.ico"
BrandingText "${APP_PUBLISHER}"
SetCompressor /SOLID lzma
VIProductVersion "1.1.0.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 MES"
VIAddVersionKey "FileDescription" "14a Bridge Windows Installer"
VIAddVersionKey "FileVersion" "${APP_VERSION}"

!include "MUI2.nsh"

!define MUI_ICON "..\release\mes_logo.ico"
!define MUI_UNICON "..\release\mes_logo.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APP_NAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "!${APP_NAME}" SecMain
  SectionIn RO

  SetOutPath "$INSTDIR"
  File "..\release\14a_Bridge.exe"
  File "..\release\mes_logo_light.png"
  File "..\release\mes_logo.ico"

  SetOutPath "$INSTDIR\firmware"
  File "..\release\firmware\bootloader.bin"
  File "..\release\firmware\boot_app0.bin"
  File "..\release\firmware\firmware.bin"
  File "..\release\firmware\partitions.bin"
  File "..\release\firmware\StampPLC_Esptool.exe"

  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\MES"
  CreateShortCut "$SMPROGRAMS\MES\14a Bridge.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\mes_logo.ico"
  CreateShortCut "$SMPROGRAMS\MES\Uninstall 14a Bridge.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortCut "$DESKTOP\14a Bridge.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\mes_logo.ico"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\14a Bridge.lnk"
  Delete "$SMPROGRAMS\MES\14a Bridge.lnk"
  Delete "$SMPROGRAMS\MES\Uninstall 14a Bridge.lnk"
  RMDir "$SMPROGRAMS\MES"

  Delete "$INSTDIR\firmware\bootloader.bin"
  Delete "$INSTDIR\firmware\boot_app0.bin"
  Delete "$INSTDIR\firmware\firmware.bin"
  Delete "$INSTDIR\firmware\partitions.bin"
  Delete "$INSTDIR\firmware\StampPLC_Esptool.exe"
  RMDir "$INSTDIR\firmware"

  Delete "$INSTDIR\14a_Bridge.exe"
  Delete "$INSTDIR\mes_logo_light.png"
  Delete "$INSTDIR\mes_logo.ico"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  DeleteRegKey HKCU "${UNINSTALL_KEY}"
SectionEnd
