; RadioS Windows installer - hand-written NSIS 3.13 script (MSYS2 UCRT64 makensis, amd64-unicode).
;
; Built by the CMake target radios_installer from the verified package stage:
;   makensis -NOCD -INPUTCHARSET UTF8 -V2 -DRADIOS_VERSION=0.19.0 -DRADIOS_VERSION_QUAD=0.19.0.0
;            -DRADIOS_NSIS_DIR=<build>\packaging\nsis -DRADIOS_SIGNING_NSH=<build>\packaging\nsis-signing.nsh
;            -DRADIOS_OUT=<root>\dist\RadioS-0.19.0-windows-x64-setup.exe [-DRADIOS_ICON=<ico>] radios.nsi
; with the stage as working directory (-NOCD): the generated lists use stage-relative paths.
;
; Behaviour
; * Per-user install (no elevation), default %LOCALAPPDATA%\Programs\RadioS. RadioS keeps data\ and
;   config\ inside its prefix (D4), so Program Files targets are refused.
; * Upgrade in place: the previous location is reused (HKCU uninstall key); the database service of
;   that installation is stopped, the old program trees are removed (never data\ or config\ user
;   files) and the new files are installed.
; * Refuses to run while RadioS.exe of the target installation is running (its image is locked).
; * The uninstaller deletes exactly the files this version installed and empty folders; data\ and
;   user configuration are never removed.
; * Silent use: Setup.exe /S [/D=C:\path\RadioS]   Uninstall.exe /S

!ifndef RADIOS_VERSION | RADIOS_VERSION_QUAD | RADIOS_NSIS_DIR | RADIOS_OUT | RADIOS_SIGNING_NSH
  !error "radios.nsi: RADIOS_VERSION, RADIOS_VERSION_QUAD, RADIOS_NSIS_DIR, RADIOS_SIGNING_NSH and RADIOS_OUT must be defined (use the radios_installer target)"
!endif

Unicode true
Target amd64-unicode
ManifestDPIAware true
ManifestLongPathAware true
ManifestSupportedOS {8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}
SetCompressor /SOLID lzma
SetCompressorDictSize 64
RequestExecutionLevel user
AllowRootDirInstall false

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\RadioS"
!define RADIOS_REG_KEY "Software\RadioS"

Name "RadioS ${RADIOS_VERSION}"
OutFile "${RADIOS_OUT}"
InstallDir "$LOCALAPPDATA\Programs\RadioS"
InstallDirRegKey HKCU "${UNINST_KEY}" "InstallLocation"
BrandingText "RadioS ${RADIOS_VERSION}"
ShowInstDetails show
ShowUninstDetails show
SetOverwrite on

!include "${RADIOS_SIGNING_NSH}"
!include "${RADIOS_NSIS_DIR}\package-defines.nsh"
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WinVer.nsh"

; --- Interface ------------------------------------------------------------------------------
!ifdef RADIOS_ICON
  !define MUI_ICON "${RADIOS_ICON}"
  !define MUI_UNICON "${RADIOS_ICON}"
!endif
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_SMALLDESC
!define MUI_LANGDLL_REGISTRY_ROOT HKCU
!define MUI_LANGDLL_REGISTRY_KEY "${RADIOS_REG_KEY}"
!define MUI_LANGDLL_REGISTRY_VALUENAME "InstallerLanguage"
!define MUI_LANGDLL_ALLLANGUAGES

!insertmacro MUI_PAGE_WELCOME
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE DirectoryLeave
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\RadioS.exe"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "$(UN_FINISH_TEXT)"
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "Turkish"
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_RESERVEFILE_LANGDLL

; --- Texts (Turkish first: the station's language) ------------------------------------------
LangString SEC_MAIN ${LANG_TURKISH} "RadioS (gerekli)"
LangString SEC_MAIN ${LANG_ENGLISH} "RadioS (required)"
LangString SEC_DESKTOP ${LANG_TURKISH} "Masaüstü kısayolu"
LangString SEC_DESKTOP ${LANG_ENGLISH} "Desktop shortcut"
LangString SEC_SYMBOLS ${LANG_TURKISH} "Hata ayıklama sembolleri (RadioS.pdb)"
LangString SEC_SYMBOLS ${LANG_ENGLISH} "Debug symbols (RadioS.pdb)"
LangString DESC_MAIN ${LANG_TURKISH} "Uygulama, GTK çalışma ortamı, Python veritabanı servisi ve araçlar."
LangString DESC_MAIN ${LANG_ENGLISH} "Application, GTK runtime, Python database service and tools."
LangString DESC_DESKTOP ${LANG_TURKISH} "Masaüstüne RadioS kısayolu koyar."
LangString DESC_DESKTOP ${LANG_ENGLISH} "Puts a RadioS shortcut on the desktop."
LangString DESC_SYMBOLS ${LANG_TURKISH} "Çökme dökümlerini (data\logs\crash) çözümlemek için; normal kullanımda gerekmez."
LangString DESC_SYMBOLS ${LANG_ENGLISH} "Needed only to analyse crash dumps (data\logs\crash)."
LangString MSG_WIN10 ${LANG_TURKISH} "RadioS Windows 10 veya üstünü (64 bit) gerektirir."
LangString MSG_WIN10 ${LANG_ENGLISH} "RadioS requires 64-bit Windows 10 or later."
LangString MSG_PROGRAMFILES ${LANG_TURKISH} "RadioS istasyon verilerini ve ayarlarını kendi klasöründe tutar; bu yüzden yönetici izni isteyen Program Files ya da Windows klasörlerine kurulamaz.$\r$\nKullanıcı klasörünüzde bir yer seçin (varsayılan: %LOCALAPPDATA%\Programs\RadioS)."
LangString MSG_PROGRAMFILES ${LANG_ENGLISH} "RadioS keeps the station data and settings inside its own folder, so it cannot be installed into Program Files or the Windows folder (they need administrator rights).$\r$\nChoose a folder in your user profile (default: %LOCALAPPDATA%\Programs\RadioS)."
LangString MSG_FOREIGN ${LANG_TURKISH} "Hedef klasör boş değil ve bir RadioS kurulumu değil:$\r$\n$INSTDIR$\r$\nBoş bir klasör ya da mevcut RadioS kurulumunun klasörünü seçin."
LangString MSG_FOREIGN ${LANG_ENGLISH} "The target folder is not empty and is not a RadioS installation:$\r$\n$INSTDIR$\r$\nChoose an empty folder or the folder of the existing RadioS installation."
LangString MSG_LONGPATH ${LANG_TURKISH} "Kurulum yolu uzun (80 karakterden fazla). Windows'ta uzun yol desteği kapalıysa bazı dosyalara erişilemeyebilir. Yine de devam edilsin mi?"
LangString MSG_LONGPATH ${LANG_ENGLISH} "The installation path is long (more than 80 characters). If long path support is disabled in Windows some files may become unreachable. Continue anyway?"
LangString MSG_RUNNING ${LANG_TURKISH} "RadioS şu anda açık:$\r$\n$INSTDIR\bin\RadioS.exe$\r$\nLütfen RadioS'u kapatıp yeniden deneyin."
LangString MSG_RUNNING ${LANG_ENGLISH} "RadioS is running:$\r$\n$INSTDIR\bin\RadioS.exe$\r$\nPlease close RadioS and try again."
LangString MSG_STOPPING ${LANG_TURKISH} "RadioS veritabanı servisi durduruluyor..."
LangString MSG_STOPPING ${LANG_ENGLISH} "Stopping the RadioS database service..."
LangString MSG_SERVICE_BUSY ${LANG_TURKISH} "RadioS veritabanı servisi durdurulamadı ($INSTDIR\python\python.exe hâlâ kullanımda).$\r$\nRadioS'u ve bu kurulumun Python araçlarını kapatıp yeniden deneyin. Hiçbir dosya değiştirilmedi."
LangString MSG_SERVICE_BUSY ${LANG_ENGLISH} "The RadioS database service could not be stopped ($INSTDIR\python\python.exe is still in use).$\r$\nClose RadioS and the Python tools of this installation and try again. No file was changed."
LangString MSG_WIPE_FAILED ${LANG_TURKISH} "Eski program dosyaları kaldırılamadı (kullanımda olabilir): $0$\r$\nRadioS'u ve veritabanı servisini kapatıp kurulumu yineleyin. Veriler ve ayarlar değiştirilmedi."
LangString MSG_WIPE_FAILED ${LANG_ENGLISH} "The old program files could not be removed (they may be in use): $0$\r$\nClose RadioS and its database service and run the installer again. Data and settings were not changed."
LangString MSG_UPGRADE ${LANG_TURKISH} "Mevcut RadioS kurulumu güncelleniyor; data\ ve config\ korunuyor."
LangString MSG_UPGRADE ${LANG_ENGLISH} "Updating the existing RadioS installation; data\ and config\ are kept."
LangString UN_FINISH_TEXT ${LANG_TURKISH} "RadioS kaldırıldı.$\r$\n$\r$\nİstasyon verileri ve ayarlar SİLİNMEDİ:$\r$\n$INSTDIR\data$\r$\n$INSTDIR\config$\r$\n$\r$\nArtık gerekmiyorsa bu klasörleri yedekledikten sonra elle silebilirsiniz."
LangString UN_FINISH_TEXT ${LANG_ENGLISH} "RadioS has been removed.$\r$\n$\r$\nThe station data and settings were NOT deleted:$\r$\n$INSTDIR\data$\r$\n$INSTDIR\config$\r$\n$\r$\nIf they are no longer needed, back them up and delete the folders manually."

; --- Version information ----------------------------------------------------------------------
VIProductVersion "${RADIOS_VERSION_QUAD}"
VIFileVersion "${RADIOS_VERSION_QUAD}"
VIAddVersionKey /LANG=${LANG_TURKISH} "ProductName" "RadioS"
VIAddVersionKey /LANG=${LANG_TURKISH} "FileDescription" "RadioS ${RADIOS_VERSION} kurulumu"
VIAddVersionKey /LANG=${LANG_TURKISH} "FileVersion" "${RADIOS_VERSION}"
VIAddVersionKey /LANG=${LANG_TURKISH} "ProductVersion" "${RADIOS_VERSION}"
VIAddVersionKey /LANG=${LANG_TURKISH} "CompanyName" "RadioS"
VIAddVersionKey /LANG=${LANG_TURKISH} "LegalCopyright" "RadioS"
VIAddVersionKey /LANG=${LANG_ENGLISH} "ProductName" "RadioS"
VIAddVersionKey /LANG=${LANG_ENGLISH} "FileDescription" "RadioS ${RADIOS_VERSION} setup"
VIAddVersionKey /LANG=${LANG_ENGLISH} "FileVersion" "${RADIOS_VERSION}"
VIAddVersionKey /LANG=${LANG_ENGLISH} "ProductVersion" "${RADIOS_VERSION}"
VIAddVersionKey /LANG=${LANG_ENGLISH} "CompanyName" "RadioS"
VIAddVersionKey /LANG=${LANG_ENGLISH} "LegalCopyright" "RadioS"

; --- Helpers shared by installer and uninstaller ------------------------------------------------
!macro RADIOS_HELPERS UN
; Push <path>; Call RadiosFileLocked; Pop <"1" locked | "0" free or missing>.
; A running program maps its image without FILE_SHARE_WRITE, so opening it for writing fails
; with a sharing violation; the file is not modified (OPEN_EXISTING, no write).
Function ${UN}RadiosFileLocked
  Exch $0
  Push $1
  Push $2
  StrCpy $2 "0"
  ${If} ${FileExists} "$0"
    System::Call 'kernel32::CreateFileW(w r0, i 0x40000000, i 7, p 0, i 3, i 0x80, p 0) p .r1 ?e'
    Pop $2
    IntPtrCmp $1 -1 0 handle_ok handle_ok
      ; ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND: vanished meanwhile -> not locked
      ${If} $2 = 2
      ${OrIf} $2 = 3
        StrCpy $2 "0"
      ${Else}
        StrCpy $2 "1"
      ${EndIf}
      Goto done
    handle_ok:
      System::Call 'kernel32::CloseHandle(p r1)'
      StrCpy $2 "0"
    done:
  ${EndIf}
  StrCpy $0 $2
  Pop $2
  Pop $1
  Exch $0
FunctionEnd

; Push <attempts>; Call RadiosWaitPythonFree; Pop <"1" still locked | "0" free>.
; Polls $INSTDIR\python\python.exe every 500 ms.
Function ${UN}RadiosWaitPythonFree
  Exch $1
  Push $0
  ${Do}
    Push "$INSTDIR\python\python.exe"
    Call ${UN}RadiosFileLocked
    Pop $0
    ${If} $0 == "0"
    ${OrIf} $1 <= 0
      ${Break}
    ${EndIf}
    IntOp $1 $1 - 1
    Sleep 500
  ${Loop}
  StrCpy $1 $0
  Pop $0
  Exch $1
FunctionEnd

; Stops the SDBASE service of $INSTDIR when its python.exe is in use; aborts when it stays busy.
Function ${UN}RadiosStopService
  Push $0
  Push $1
  Push "$INSTDIR\python\python.exe"
  Call ${UN}RadiosFileLocked
  Pop $0
  ${If} $0 == "1"
  ${AndIf} ${FileExists} "$INSTDIR\tools\radios_database.py"
    DetailPrint "$(MSG_STOPPING)"
    ; Graceful stop first (named stop event, D6); "stop --force" only when the service is still
    ; running after 20 s (it terminates exactly the process recorded for this installation).
    nsExec::ExecToLog '"$INSTDIR\python\python.exe" -E -s -B -X utf8 "$INSTDIR\tools\radios_database.py" --project-root "$INSTDIR\." stop'
    Pop $1
    DetailPrint "radios_database.py stop: $1"
    Push 40
    Call ${UN}RadiosWaitPythonFree
    Pop $0
    ${If} $0 == "1"
      nsExec::ExecToLog '"$INSTDIR\python\python.exe" -E -s -B -X utf8 "$INSTDIR\tools\radios_database.py" --project-root "$INSTDIR\." stop --force'
      Pop $1
      DetailPrint "radios_database.py stop --force: $1"
      Push 20
      Call ${UN}RadiosWaitPythonFree
      Pop $0
    ${EndIf}
  ${EndIf}
  ${If} $0 == "1"
    MessageBox MB_ICONSTOP|MB_OK "$(MSG_SERVICE_BUSY)" /SD IDOK
    Abort
  ${EndIf}
  Pop $1
  Pop $0
FunctionEnd

; Aborts when RadioS.exe of $INSTDIR is running.
Function ${UN}RadiosRefuseWhileRunning
  Push $0
  Push "$INSTDIR\bin\RadioS.exe"
  Call ${UN}RadiosFileLocked
  Pop $0
  ${If} $0 == "1"
    MessageBox MB_ICONSTOP|MB_OK "$(MSG_RUNNING)" /SD IDOK
    Abort
  ${EndIf}
  Pop $0
FunctionEnd
!macroend
!insertmacro RADIOS_HELPERS ""
!insertmacro RADIOS_HELPERS "un."

; Push <prefix>; Call StartsWith (on $INSTDIR); Pop <"1"|"0"> - case-insensitive.
Function InstDirStartsWith
  Exch $0
  Push $1
  Push $2
  StrLen $1 $0
  StrCpy $2 "0"
  ${If} $1 > 0
    StrCpy $1 $INSTDIR $1
    ${If} $1 == $0
      StrCpy $2 "1"
    ${EndIf}
  ${EndIf}
  StrCpy $0 $2
  Pop $2
  Pop $1
  Exch $0
FunctionEnd

; Sets $R9 to "" when $INSTDIR is acceptable, otherwise to "programfiles" or "foreign".
Function CheckInstallDir
  Push $0
  Push $1
  StrCpy $R9 ""
  ${ForEach} $1 0 3 + 1
    ${Select} $1
      ${Case} 0
        StrCpy $0 "$PROGRAMFILES64"
      ${Case} 1
        StrCpy $0 "$PROGRAMFILES32"
      ${Case} 2
        StrCpy $0 "$WINDIR"
      ${CaseElse}
        StrCpy $0 "$COMMONFILES64"
    ${EndSelect}
    Push "$0\"
    Call InstDirStartsWith
    Pop $0
    ${If} $0 == "1"
      StrCpy $R9 "programfiles"
    ${EndIf}
  ${Next}
  ${If} $R9 == ""
  ${AndIfNot} ${FileExists} "$INSTDIR\bin\RadioS.exe"
    ; Not an existing installation: the folder must be missing, empty, or hold only what an
    ; uninstall deliberately leaves behind (data\ and the user's config\) - reinstalling into
    ; it brings the station back with its data.
    ClearErrors
    FindFirst $0 $1 "$INSTDIR\*"
    ${DoUntil} ${Errors}
      ${If} $1 != "."
      ${AndIf} $1 != ".."
      ${AndIf} $1 != ""
      ${AndIf} $1 != "data"
      ${AndIf} $1 != "config"
        StrCpy $R9 "foreign"
        ${Break}
      ${EndIf}
      FindNext $0 $1
    ${Loop}
    FindClose $0
  ${ElseIf} $R9 == ""
    ; An existing RadioS.exe alone is not enough to wipe program trees later: require the
    ; root markers of D4 as well.
    ${IfNot} ${FileExists} "$INSTDIR\config\radios.example.ini"
    ${OrIfNot} ${FileExists} "$INSTDIR\database\SDBASE\pyproject.toml"
      StrCpy $R9 "foreign"
    ${EndIf}
  ${EndIf}
  Pop $1
  Pop $0
FunctionEnd

Function ReportInstallDirProblem
  ${If} $R9 == "programfiles"
    MessageBox MB_ICONSTOP|MB_OK "$(MSG_PROGRAMFILES)" /SD IDOK
  ${ElseIf} $R9 == "foreign"
    MessageBox MB_ICONSTOP|MB_OK "$(MSG_FOREIGN)" /SD IDOK
  ${EndIf}
FunctionEnd

Function DirectoryLeave
  Call CheckInstallDir
  ${If} $R9 != ""
    Call ReportInstallDirProblem
    Abort   ; stay on the directory page
  ${EndIf}
  StrLen $0 $INSTDIR
  ${If} $0 > 80
    MessageBox MB_ICONEXCLAMATION|MB_YESNO "$(MSG_LONGPATH)" /SD IDYES IDYES long_ok
    Abort
    long_ok:
  ${EndIf}
FunctionEnd

Function .onInit
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP|MB_OK "$(MSG_WIN10)" /SD IDOK
    SetErrorLevel 2
    Quit
  ${EndIf}
  !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd

Function un.onInit
  !insertmacro MUI_UNGETLANGUAGE
FunctionEnd

; --- Sections ---------------------------------------------------------------------------------
Section "!$(SEC_MAIN)" SecMain
  SectionIn RO
  ; Checks repeated here for silent installs (/S /D=...), where no page callback runs.
  Call CheckInstallDir
  ${If} $R9 != ""
    Call ReportInstallDirProblem
    Abort
  ${EndIf}
  StrLen $0 $INSTDIR
  ${If} $0 > 80
    DetailPrint "$(MSG_LONGPATH)"
  ${EndIf}

  ${If} ${FileExists} "$INSTDIR\bin\RadioS.exe"
    DetailPrint "$(MSG_UPGRADE)"
    Call RadiosRefuseWhileRunning
    Call RadiosStopService
    ; Remove the previous program trees so that files dropped by this version do not linger
    ; (a stale database\SDBASE\src\*.py would change the SDBASE source digest). data\ and the
    ; user's files in config\ are not touched.
    ClearErrors
    ${ForEach} $1 0 8 + 1
      ${Select} $1
        ${Case} 0
          StrCpy $0 "$INSTDIR\bin"
        ${Case} 1
          StrCpy $0 "$INSTDIR\lib"
        ${Case} 2
          StrCpy $0 "$INSTDIR\share"
        ${Case} 3
          StrCpy $0 "$INSTDIR\etc"
        ${Case} 4
          StrCpy $0 "$INSTDIR\python"
        ${Case} 5
          StrCpy $0 "$INSTDIR\tools"
        ${Case} 6
          StrCpy $0 "$INSTDIR\assets"
        ${Case} 7
          StrCpy $0 "$INSTDIR\docs"
        ${CaseElse}
          StrCpy $0 "$INSTDIR\database\SDBASE\src"
      ${EndSelect}
      RMDir /r "$0"
      ${If} ${FileExists} "$0\*"
        MessageBox MB_ICONSTOP|MB_OK "$(MSG_WIPE_FAILED)" /SD IDOK
        Abort
      ${EndIf}
    ${Next}
  ${EndIf}

  !include "${RADIOS_NSIS_DIR}\files-install.nsh"

  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "RadioS"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${RADIOS_VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "RadioS"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\bin\RadioS.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" ${RADIOS_ESTIMATED_SIZE_KB}
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1

  ; Start menu entry; the working directory is the installation (root discovery, D4).
  SetShellVarContext current
  Delete "$SMPROGRAMS\RadioS.lnk"
  Delete "$DESKTOP\RadioS.lnk"
  SetOutPath "$INSTDIR"
  CreateShortCut "$SMPROGRAMS\RadioS.lnk" "$INSTDIR\bin\RadioS.exe" "" "$INSTDIR\bin\RadioS.exe" 0
SectionEnd

Section "$(SEC_DESKTOP)" SecDesktop
  SetShellVarContext current
  SetOutPath "$INSTDIR"
  CreateShortCut "$DESKTOP\RadioS.lnk" "$INSTDIR\bin\RadioS.exe" "" "$INSTDIR\bin\RadioS.exe" 0
SectionEnd

!ifdef RADIOS_HAVE_SYMBOLS
Section /o "$(SEC_SYMBOLS)" SecSymbols
  !include "${RADIOS_NSIS_DIR}\files-symbols-install.nsh"
SectionEnd
!endif

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "$(DESC_MAIN)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "$(DESC_DESKTOP)"
!ifdef RADIOS_HAVE_SYMBOLS
  !insertmacro MUI_DESCRIPTION_TEXT ${SecSymbols} "$(DESC_SYMBOLS)"
!endif
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  Call un.RadiosRefuseWhileRunning
  Call un.RadiosStopService

  SetShellVarContext current
  Delete "$SMPROGRAMS\RadioS.lnk"
  Delete "$DESKTOP\RadioS.lnk"

  ; Exactly the files of this version, then empty folders (data\ is never touched; config\
  ; stays while it holds the user's configuration).
!ifdef RADIOS_HAVE_SYMBOLS
  !include "${RADIOS_NSIS_DIR}\files-symbols-uninstall.nsh"
!endif
  !include "${RADIOS_NSIS_DIR}\files-uninstall.nsh"

  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegValue HKCU "${RADIOS_REG_KEY}" "InstallerLanguage"
  DeleteRegKey /ifempty HKCU "${RADIOS_REG_KEY}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DetailPrint "$(UN_FINISH_TEXT)"
SectionEnd
