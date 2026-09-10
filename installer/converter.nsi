; Per-user installer for the Video and Audio Compressor and Converter.
;
; Deliberately per-user, not per-machine: the target machines are locked-down
; org PCs where the user has no administrator rights. Nothing here writes
; outside HKCU or the user's own profile, and nothing needs elevation.
;
; Build with:  makensis -DVERSION=2.1.0 -DSOURCE_DIR=..\build\windows-x64\app\Release converter.nsi
;
; This file is UTF-8 WITH a byte-order mark, and must stay that way: makensis
; reads a file without one in the ANSI code page, which garbles the Persian
; text below (Unicode True only sets the installer's own string type).
; cmake/CheckMediaExtensions.cmake fails the configure if the mark goes missing.

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "Integration.nsh"   ; ${NotifyShell_AssocChanged}
!include "LogicLib.nsh"

; 3.06 is where DeleteRegKey /IfEmpty started checking values as well as
; subkeys. The uninstaller relies on that to leave other apps' entries in
; shared keys such as .mp4\OpenWithProgids alone; an older makensis would
; delete them together with the key.
!if ${NSIS_PACKEDVERSION} < 0x03006000
  !error "NSIS 3.06 or newer is required"
!endif

!ifndef VERSION
  !define VERSION "2.1.0"
!endif
!ifndef SOURCE_DIR
  !define SOURCE_DIR "..\build\windows-x64\app\Release"
!endif

!define APP_NAME    "Video Converter"
!define APP_EXE     "converter.exe"
!define PUBLISHER   "Sina0"
!define REG_KEY     "Software\Microsoft\Windows\CurrentVersion\Uninstall\VideoConverter"

; Shell integration: "Open with", the folder right-click verb and Send To.
; Everything lives under HKCU\Software\Classes, which needs no elevation, and
; the app is only ever offered, never made the default for anything.
!define CLASSES      "Software\Classes"
!define PROGID       "Sina0.VideoConverter.Media.1"   ; Vendor.Component.Version, as Microsoft asks
!define DIR_VERB     "Sina0.VideoConverter.Convert"   ; vendor-prefixed, so no other app's verb clashes
!define FILE_EXTS    "Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts"
!define ASSOC_TOASTS "Software\Microsoft\Windows\CurrentVersion\ApplicationAssociationToasts"

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

; The folder right-click verb's label, in whichever language the installer ran in.
LangString VERB_CONVERT ${LANG_FARSI}   "تبدیل با مبدل ویدیو"
LangString VERB_CONVERT ${LANG_ENGLISH} "Convert with Video Converter"

; ---------------------------------------------------------------------------
; File types
; ---------------------------------------------------------------------------
; Every extension the engine accepts (kVideoExtensions and kAudioExtensions in
; engine/src/util.cpp), minus .pcm: raw PCM has no header, so ffprobe cannot
; identify it and "Open with" could only end in a failed conversion.
; cmake/CheckMediaExtensions.cmake compares this list with the engine's at
; configure time, so the two cannot drift apart unnoticed.
!macro _ForEachMediaExt M
  !insertmacro ${M} ".mp4"
  !insertmacro ${M} ".avi"
  !insertmacro ${M} ".mkv"
  !insertmacro ${M} ".mov"
  !insertmacro ${M} ".flv"
  !insertmacro ${M} ".wmv"
  !insertmacro ${M} ".mxf"
  !insertmacro ${M} ".gxf"
  !insertmacro ${M} ".lxf"
  !insertmacro ${M} ".webm"
  !insertmacro ${M} ".3gp"
  !insertmacro ${M} ".ts"
  !insertmacro ${M} ".m2ts"
  !insertmacro ${M} ".ogv"
  !insertmacro ${M} ".mp3"
  !insertmacro ${M} ".aac"
  !insertmacro ${M} ".wav"
  !insertmacro ${M} ".flac"
  !insertmacro ${M} ".ogg"
  !insertmacro ${M} ".opus"
  !insertmacro ${M} ".wma"
  !insertmacro ${M} ".amr"
  !insertmacro ${M} ".ape"
  !insertmacro ${M} ".aiff"
  !insertmacro ${M} ".aif"
  !insertmacro ${M} ".au"
  !insertmacro ${M} ".mpc"
  !insertmacro ${M} ".oma"
  !insertmacro ${M} ".aa3"
  !insertmacro ${M} ".wv"
  !insertmacro ${M} ".m4a"
!macroend

; Removes one value this installer wrote, then its key if that left it empty.
; Most keys touched here are shared with other apps, so the uninstaller never
; deletes a key that still holds anything.
!macro _DeleteValue KEY NAME
  DeleteRegValue HKCU "${KEY}" "${NAME}"
  DeleteRegKey /IfEmpty HKCU "${KEY}"
!macroend

; 1 when Applications\converter.exe is this app's to write or remove. The key
; is named after the generic converter.exe alone, so another program may own it
; (set in the Install and Uninstall sections).
Var OwnAppKey

; Adds the app to "Open with" for one extension, and nothing more. NEVER write
; the (Default) value of Software\Classes\<ext>: in the merged HKCR view an
; HKCU value there overrides the machine's and IS the default handler.
!macro _RegMediaExt EXT
  WriteRegNone HKCU "${CLASSES}\${EXT}\OpenWithProgids" "${PROGID}"
  ${If} $OwnAppKey == 1
    WriteRegNone HKCU "${CLASSES}\Applications\${APP_EXE}\SupportedTypes" "${EXT}"
  ${EndIf}
!macroend

!macro _UnregMediaExt EXT
  !insertmacro _DeleteValue "${CLASSES}\${EXT}\OpenWithProgids" "${PROGID}"
  DeleteRegKey /IfEmpty HKCU "${CLASSES}\${EXT}"
  ${If} $OwnAppKey == 1
    DeleteRegValue HKCU "${CLASSES}\Applications\${APP_EXE}\SupportedTypes" "${EXT}"
  ${EndIf}
  ; What Explorer itself recorded once the app was picked from "Open with".
  ; UserChoice is hash-protected and left alone: Windows asks again once the
  ; ProgID it names is gone.
  !insertmacro _DeleteValue "${FILE_EXTS}\${EXT}\OpenWithProgids" "${PROGID}"
  DeleteRegValue HKCU "${ASSOC_TOASTS}" "${PROGID}_${EXT}"
  DeleteRegValue HKCU "${ASSOC_TOASTS}" "Applications\${APP_EXE}_${EXT}"
!macroend

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

  ; ---- "Open with" ----
  ; A ProgID the extensions list as an alternative, never as their default.
  ; Its (Default) stays empty so Explorer keeps each type's own description.
  ; Player: Explorer offers the verb for up to 100 selected files instead of
  ; 15, starting one process per file; the running window collects them (a
  ; second launch hands its paths over to it).
  WriteRegNone HKCU "${CLASSES}\${PROGID}" "AllowSilentDefaultTakeOver"   ; Windows 8+: ignored when a default is chosen
  WriteRegStr  HKCU "${CLASSES}\${PROGID}\DefaultIcon" "" "$INSTDIR\${APP_EXE},0"
  WriteRegStr  HKCU "${CLASSES}\${PROGID}\shell\open" "MultiSelectModel" "Player"
  WriteRegStr  HKCU "${CLASSES}\${PROGID}\shell\open\command" "" '"$INSTDIR\${APP_EXE}" "%1"'

  ; The app's own entry, behind "Open with > Choose another app". In the merged
  ; HKCR view these HKCU values would override another program's
  ; Applications\converter.exe, so they are written only while that key has no
  ; command yet or is already this app's. The ProgID above is enough for
  ; "Open with" without it.
  ReadRegStr $0 HKCR "Applications\${APP_EXE}\shell\open\command" ""
  ReadRegStr $1 HKCU "${CLASSES}\Applications\${APP_EXE}" "FriendlyAppName"
  StrCpy $OwnAppKey 0
  ${If} $0 == ""
  ${OrIf} $0 == '"$INSTDIR\${APP_EXE}" "%1"'
  ${OrIf} $1 == "${APP_NAME}"
    StrCpy $OwnAppKey 1
    WriteRegStr  HKCU "${CLASSES}\Applications\${APP_EXE}" "FriendlyAppName" "${APP_NAME}"
    WriteRegStr  HKCU "${CLASSES}\Applications\${APP_EXE}\DefaultIcon" "" "$INSTDIR\${APP_EXE},0"
    WriteRegStr  HKCU "${CLASSES}\Applications\${APP_EXE}\shell\open" "MultiSelectModel" "Player"
    WriteRegStr  HKCU "${CLASSES}\Applications\${APP_EXE}\shell\open\command" "" '"$INSTDIR\${APP_EXE}" "%1"'
  ${EndIf}
  !insertmacro _ForEachMediaExt _RegMediaExt

  ; ---- Folder right-click verb ----
  ; Only our own subkey. NEVER write the (Default) value of Directory\shell
  ; itself: HKLM holds "none" there, and an HKCU value would change what
  ; double-clicking a folder does. On Windows 11 the verb sits under
  ; "Show more options"; the compact menu needs a packaged app.
  WriteRegStr HKCU "${CLASSES}\Directory\shell\${DIR_VERB}" "" "$(VERB_CONVERT)"
  WriteRegStr HKCU "${CLASSES}\Directory\shell\${DIR_VERB}" "Icon" "$INSTDIR\${APP_EXE},0"
  WriteRegStr HKCU "${CLASSES}\Directory\shell\${DIR_VERB}" "MultiSelectModel" "Player"
  WriteRegStr HKCU "${CLASSES}\Directory\shell\${DIR_VERB}\command" "" '"$INSTDIR\${APP_EXE}" "%1"'

  ; Send To passes a mixed selection of files and folders in ONE command line,
  ; which neither verb above can do.
  CreateShortcut "$SENDTO\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0

  ; Explorer caches associations; tell it they changed.
  ${NotifyShell_AssocChanged}
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  RMDir  "$SMPROGRAMS\${APP_NAME}"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  Delete "$SENDTO\${APP_NAME}.lnk"

  ; Applications\converter.exe only if this app wrote it (see the Install section).
  ReadRegStr $0 HKCU "${CLASSES}\Applications\${APP_EXE}\shell\open\command" ""
  ReadRegStr $1 HKCU "${CLASSES}\Applications\${APP_EXE}" "FriendlyAppName"
  StrCpy $OwnAppKey 0
  ${If} $0 == '"$INSTDIR\${APP_EXE}" "%1"'
  ${OrIf} $1 == "${APP_NAME}"
    StrCpy $OwnAppKey 1
  ${EndIf}

  ; Shell integration, undone value by value (see _DeleteValue).
  !insertmacro _ForEachMediaExt _UnregMediaExt

  !insertmacro _DeleteValue "${CLASSES}\${PROGID}\shell\open\command" ""
  !insertmacro _DeleteValue "${CLASSES}\${PROGID}\shell\open" "MultiSelectModel"
  DeleteRegKey /IfEmpty HKCU "${CLASSES}\${PROGID}\shell"
  !insertmacro _DeleteValue "${CLASSES}\${PROGID}\DefaultIcon" ""
  !insertmacro _DeleteValue "${CLASSES}\${PROGID}" "AllowSilentDefaultTakeOver"

  ; converter.exe is a generic name: another app may share this key.
  ${If} $OwnAppKey == 1
    !insertmacro _DeleteValue "${CLASSES}\Applications\${APP_EXE}\shell\open\command" ""
    !insertmacro _DeleteValue "${CLASSES}\Applications\${APP_EXE}\shell\open" "MultiSelectModel"
    DeleteRegKey /IfEmpty HKCU "${CLASSES}\Applications\${APP_EXE}\shell"
    !insertmacro _DeleteValue "${CLASSES}\Applications\${APP_EXE}\DefaultIcon" ""
    DeleteRegKey /IfEmpty HKCU "${CLASSES}\Applications\${APP_EXE}\SupportedTypes"
    !insertmacro _DeleteValue "${CLASSES}\Applications\${APP_EXE}" "FriendlyAppName"
  ${EndIf}

  !insertmacro _DeleteValue "${CLASSES}\Directory\shell\${DIR_VERB}\command" ""
  DeleteRegValue HKCU "${CLASSES}\Directory\shell\${DIR_VERB}" "Icon"
  DeleteRegValue HKCU "${CLASSES}\Directory\shell\${DIR_VERB}" "MultiSelectModel"
  !insertmacro _DeleteValue "${CLASSES}\Directory\shell\${DIR_VERB}" ""
  DeleteRegKey /IfEmpty HKCU "${CLASSES}\Directory\shell"
  DeleteRegKey /IfEmpty HKCU "${CLASSES}\Directory"

  ; The name and publisher Explorer cached for the "Open with" list.
  DeleteRegValue HKCU "Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\MuiCache" "$INSTDIR\${APP_EXE}.FriendlyAppName"
  DeleteRegValue HKCU "Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\MuiCache" "$INSTDIR\${APP_EXE}.ApplicationCompany"

  ${NotifyShell_AssocChanged}

  RMDir /r "$INSTDIR"

  DeleteRegKey HKCU "${REG_KEY}"
  DeleteRegKey HKCU "Software\VideoConverter"

  ; Logs, the encoder benchmark cache and CEF's own cache live in LocalAppData
  ; and are left alone -- a reinstall should not lose the user's run history.
  ; Remove them by hand from %LOCALAPPDATA%\VideoConverter if that is wanted.
SectionEnd
