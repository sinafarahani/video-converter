; Per-user installer for the Video and Audio Compressor and Converter.
;
; Deliberately per-user, not per-machine: the target machines are locked-down
; org PCs where the user has no administrator rights. Nothing here writes
; outside HKCU or the user's own profile, and nothing needs elevation.
;
; Build with:  makensis -DVERSION=2.0.1 -DSOURCE_DIR=..\build\windows-x64\app\Release converter.nsi

!include "MUI2.nsh"
!include "FileFunc.nsh"

!ifndef VERSION
  !define VERSION "2.0.1"
!endif
!ifndef SOURCE_DIR
  !define SOURCE_DIR "..\build\windows-x64\app\Release"
!endif

!define APP_NAME    "Video Converter"
!define APP_EXE     "converter.exe"
!define PUBLISHER   "Sina0"
!define REG_KEY     "Software\Microsoft\Windows\CurrentVersion\Uninstall\VideoConverter"

Name        "${APP_NAME}"
OutFile     "VideoConverter-${VERSION}-windows-x64-setup.exe"
Unicode     True
SetCompressor /SOLID lzma

; No elevation, ever. If this is set to anything else the install will prompt
; for admin on the machines this is meant for and simply fail.
RequestExecutionLevel user

; Under the user's profile, so no write permission outside it is required.
InstallDir "$LOCALAPPDATA\Programs\VideoConverter"
InstallDirRegKey HKCU "Software\VideoConverter" "InstallDir"

VIProductVersion "${VERSION}.0"
VIAddVersionKey  "ProductName"     "${APP_NAME}"
VIAddVersionKey  "FileVersion"     "${VERSION}"
VIAddVersionKey  "ProductVersion"  "${VERSION}"
VIAddVersionKey  "CompanyName"     "${PUBLISHER}"
VIAddVersionKey  "LegalCopyright"  "${PUBLISHER}"
VIAddVersionKey  "FileDescription" "${APP_NAME} Setup"

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"

; The application icon, carried over from the JavaFX build.
!define MUI_ICON   "..\app\res\converter.ico"
!define MUI_UNICON "..\app\res\converter.ico"

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; The UI is Persian; the installer follows. NSIS names this language file
; "Farsi", not "Persian" -- using the latter fails to find it.
!insertmacro MUI_LANGUAGE "Farsi"
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"

  ; The application, CEF's runtime, the Vue bundle and the bundled ffmpeg.
  ; SOURCE_DIR is the staged build output, which already has the correct layout.
  File /r "${SOURCE_DIR}\*.*"

  ; FFmpeg is GPL, so shipping the binary obliges us to convey the licence text
  ; and to offer the corresponding source. Both files live in ffmpeg\ and are
  ; installed by the recursive File command above; this check just fails the
  ; install loudly if they ever go missing from the staged build, rather than
  ; quietly shipping a non-compliant package.
  IfFileExists "$INSTDIR\ffmpeg\LICENSE.txt" +3 0
    MessageBox MB_ICONSTOP "Build error: ffmpeg\LICENSE.txt is missing. This package cannot be distributed."
    Abort
  IfFileExists "$INSTDIR\ffmpeg\FFMPEG-SOURCE-OFFER.txt" +3 0
    MessageBox MB_ICONSTOP "Build error: ffmpeg\FFMPEG-SOURCE-OFFER.txt is missing. This package cannot be distributed."
    Abort

  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Shortcuts pick the icon up from the executable's own resource.
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortcut  "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
  CreateShortcut  "$DESKTOP\${APP_NAME}.lnk"                "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0

  ; HKCU, so no elevation is needed to register the uninstall entry.
  WriteRegStr HKCU "Software\VideoConverter" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${REG_KEY}" "DisplayName"     "${APP_NAME}"
  WriteRegStr HKCU "${REG_KEY}" "DisplayVersion"  "${VERSION}"
  WriteRegStr HKCU "${REG_KEY}" "Publisher"       "${PUBLISHER}"
  WriteRegStr HKCU "${REG_KEY}" "DisplayIcon"     "$INSTDIR\${APP_EXE}"
  WriteRegStr HKCU "${REG_KEY}" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegDWORD HKCU "${REG_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${REG_KEY}" "NoRepair" 1

  ; Report the real footprint in Add/Remove Programs.
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${REG_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  RMDir  "$SMPROGRAMS\${APP_NAME}"
  Delete "$DESKTOP\${APP_NAME}.lnk"

  RMDir /r "$INSTDIR"

  DeleteRegKey HKCU "${REG_KEY}"
  DeleteRegKey HKCU "Software\VideoConverter"

  ; Logs, the encoder benchmark cache and CEF's own cache live in LocalAppData
  ; and are left alone -- a reinstall should not lose the user's run history.
  ; Remove them by hand from %LOCALAPPDATA%\VideoConverter if that is wanted.
SectionEnd
