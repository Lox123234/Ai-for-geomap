; Installer of GeoMap AI (NSIS 3, Unicode). Built by scripts/build-windows.ps1 -Installer:
;   makensis /DVERSION=0.4.0 /DSOURCE_DIR=<dist\GeoMapAI> /DOUTFILE=<setup.exe> /DROOT=<repo> GeoMapAI.nsi
; Installs for the current user without administrator rights into
; %LOCALAPPDATA%\Programs\GeoMapAI (the whole deployment folder, with the built-in OpenDroneMap),
; adds Start menu and optional desktop shortcuts and an entry in "Apps" for uninstalling.
; Projects and the data folder (%LOCALAPPDATA%\GeoMapAI: logs, work links) are never touched.

Unicode true
ManifestDPIAware true
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

!ifndef VERSION
  !error "Define VERSION (e.g. /DVERSION=0.4.0)"
!endif
!ifndef SOURCE_DIR
  !error "Define SOURCE_DIR (the deployment folder with GeoMapAI.exe)"
!endif
!ifndef OUTFILE
  !error "Define OUTFILE"
!endif
!ifndef ROOT
  !error "Define ROOT (the repository)"
!endif
!ifndef LICENSE_FILE
  !error "Define LICENSE_FILE (installer\license.txt as UTF-16LE)"
!endif

!define APP "GeoMap AI"
!define EXE "GeoMapAI.exe"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\GeoMapAI"

Name "${APP}"
Caption "Установка ${APP} ${VERSION}"
UninstallCaption "Удаление ${APP}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\GeoMapAI"
InstallDirRegKey HKCU "${UNINST_KEY}" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 64
BrandingText "${APP} ${VERSION}"
ShowInstDetails nevershow
ShowUninstDetails nevershow

!define MUI_ICON "${ROOT}\app\resources\geomap.ico"
!define MUI_UNICON "${ROOT}\app\resources\geomap.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${ROOT}\installer\welcome.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "${ROOT}\installer\welcome.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "${ROOT}\installer\header.bmp"
!define MUI_HEADERIMAGE_UNBITMAP "${ROOT}\installer\header.bmp"
!define MUI_ABORTWARNING
!define MUI_ABORTWARNING_TEXT "Прервать установку ${APP}?"
!define MUI_COMPONENTSPAGE_NODESC

!define MUI_WELCOMEPAGE_TITLE "Установка ${APP} ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "GeoMap AI — рабочее место для аэрофотосъёмки с БПЛА: проекты и снимки, фотограмметрическая обработка (встроенный OpenDroneMap), ГИС-карта, 3D-просмотр облаков точек, моделей и рельефа.$\r$\n$\r$\nПрограмма ставится только для вас, права администратора не нужны. Нужно около 1,2 ГБ на диске.$\r$\n$\r$\nНажмите «Далее», чтобы продолжить."
!define MUI_LICENSEPAGE_TEXT_TOP "GeoMap AI — MIT; в комплекте сторонние программы со своими лицензиями."
!define MUI_FINISHPAGE_TITLE "GeoMap AI установлен"
!define MUI_FINISHPAGE_TEXT "Программа установлена в $INSTDIR.$\r$\n$\r$\nЯрлык — в меню «Пуск». Удалить программу можно в «Параметры → Приложения»; ваши проекты при этом не удаляются."
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Запустить GeoMap AI"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!define MUI_UNCONFIRMPAGE_TEXT_TOP "GeoMap AI будет удалён из этой папки. Проекты, снимки и результаты обработки хранятся в папках проектов и не удаляются."
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "Russian"

VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=${LANG_RUSSIAN} "ProductName" "${APP}"
VIAddVersionKey /LANG=${LANG_RUSSIAN} "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=${LANG_RUSSIAN} "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=${LANG_RUSSIAN} "FileDescription" "Установка ${APP} ${VERSION}"
VIAddVersionKey /LANG=${LANG_RUSSIAN} "LegalCopyright" "© 2026 GeoMap AI contributors, MIT"
VIAddVersionKey /LANG=${LANG_RUSSIAN} "CompanyName" "GeoMap AI contributors"

; GeoMapAI.exe cannot be opened for writing while the program runs.
!macro CheckNotRunning UN
Function ${UN}CheckNotRunning
  ${If} ${FileExists} "$INSTDIR\${EXE}"
    retry:
    ClearErrors
    FileOpen $0 "$INSTDIR\${EXE}" a
    ${If} ${Errors}
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "GeoMap AI сейчас запущен. Закройте программу и нажмите «Повтор»." IDRETRY retry
      Abort
    ${EndIf}
    FileClose $0
  ${EndIf}
FunctionEnd
!macroend
!insertmacro CheckNotRunning ""
!insertmacro CheckNotRunning "un."

; The folder must be empty or hold GeoMap AI: the program is removed with its whole folder.
Function .onVerifyInstDir
  ${If} ${FileExists} "$INSTDIR\${EXE}"
    Return
  ${EndIf}
  FindFirst $0 $1 "$INSTDIR\*.*"
  ${DoWhile} $1 != ""
    ${If} $1 != "."
    ${AndIf} $1 != ".."
      FindClose $0
      Abort
    ${EndIf}
    FindNext $0 $1
  ${Loop}
  FindClose $0
FunctionEnd

Section "GeoMap AI (программа и OpenDroneMap)" SecMain
  SectionIn RO
  Call CheckNotRunning
  ; An update replaces the previous version completely (no stale libraries).
  ${If} ${FileExists} "$INSTDIR\${EXE}"
    DetailPrint "Удаление предыдущей версии…"
    RMDir /r "$INSTDIR"
  ${EndIf}
  SetOutPath "$INSTDIR"
  DetailPrint "Копирование файлов программы…"
  File /r "${SOURCE_DIR}\*.*"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0 SW_SHOWNORMAL "" "Аэрофотосъёмка, фотограмметрия, ГИС и 3D"

  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "GeoMap AI contributors"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${EXE},0"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "Ярлык на рабочем столе" SecDesktop
  CreateShortcut "$DESKTOP\${APP}.lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0 SW_SHOWNORMAL "" "Аэрофотосъёмка, фотограмметрия, ГИС и 3D"
SectionEnd

Section "Uninstall"
  Call un.CheckNotRunning
  Delete "$SMPROGRAMS\${APP}.lnk"
  Delete "$DESKTOP\${APP}.lnk"
  DeleteRegKey HKCU "${UNINST_KEY}"
  ; The folder was empty or held only GeoMap AI when it was installed (.onVerifyInstDir).
  ${If} ${FileExists} "$INSTDIR\${EXE}"
    RMDir /r "$INSTDIR"
  ${EndIf}
SectionEnd
