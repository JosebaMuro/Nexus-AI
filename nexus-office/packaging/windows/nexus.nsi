; Nexus-Office - instalador grafico de Windows (NSIS 3). Lo genera packaging/build.py (makensis funciona tambien
; en Linux): el paquete portable completo (Python, dependencias, llama-server/sd-cli) va dentro del .exe.
; Defines que pasa build.py: VERSION, ARCH (x64|arm64), PAYLOAD (carpeta Nexus-Office preparada), OUTFILE, ICON,
; LICENSE, PROFILES (fichero .nsh con los perfiles de oficina).
; Opciones de linea de comandos (tambien en modo silencioso /S):
;   /OFICINA=herreria  /SERVICIO  /RED  /MODELOS=auto|todos|no  /D=C:\ruta  (la ultima, siempre al final)

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma
SetCompressorDictSize 64
RequestExecutionLevel user

!include MUI2.nsh
!include nsDialogs.nsh
!include LogicLib.nsh
!include FileFunc.nsh
!include x64.nsh

Name "Nexus-Office ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Nexus-Office"
InstallDirRegKey HKCU "Software\Nexus-Office" "InstallDir"
BrandingText "Nexus-Office ${VERSION} (${ARCH})"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "Nexus-Office"
VIAddVersionKey "FileDescription" "Instalador de Nexus-Office (oficina de IA local)"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Nexus-Office"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Instalar Nexus-Office ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "Nexus-Office es una oficina de inteligencia artificial que funciona entera en este equipo, sin enviar nada a internet.$\r$\n$\r$\nEl instalador incluye todo lo necesario (no descarga programas). Solo el modelo de IA, elegido segun tu hardware, se descarga si hay internet.$\r$\n$\r$\nLa carpeta de instalacion es portable: puedes elegir un disco USB."
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Abrir Nexus-Office"
!define MUI_FINISHPAGE_RUN_FUNCTION OpenNexus
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\docs\USUARIO.md"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Ver el manual de usuario"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LICENSE}"
!insertmacro MUI_PAGE_DIRECTORY
Page custom OptionsPage OptionsLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Spanish"

Var Oficina
Var Servicio
Var Red
Var Modelos
Var Escritorio
Var hCombo
Var hServ
Var hRed
Var hMod
Var hDesk

Function .onInit
  ${If} "${ARCH}" == "arm64"
    ${IfNot} ${IsNativeARM64}
      MessageBox MB_ICONEXCLAMATION "Este instalador es para Windows en ARM (Snapdragon X). Usa el instalador x64 en este equipo." /SD IDOK
      Abort
    ${EndIf}
  ${Else}
    ${IfNot} ${RunningX64}
      MessageBox MB_ICONEXCLAMATION "Nexus-Office necesita Windows de 64 bits." /SD IDOK
      Abort
    ${EndIf}
  ${EndIf}
  StrCpy $Oficina ""
  StrCpy $Servicio "0"
  StrCpy $Red "0"
  StrCpy $Modelos "auto"
  StrCpy $Escritorio "1"
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/OFICINA=" $R1
  ${IfNot} ${Errors}
    StrCpy $Oficina $R1
  ${EndIf}
  ClearErrors
  ${GetOptions} $R0 "/MODELOS=" $R1
  ${IfNot} ${Errors}
    StrCpy $Modelos $R1
  ${EndIf}
  ClearErrors
  ${GetOptions} $R0 "/SERVICIO" $R1
  ${IfNot} ${Errors}
    StrCpy $Servicio "1"
  ${EndIf}
  ClearErrors
  ${GetOptions} $R0 "/RED" $R1
  ${IfNot} ${Errors}
    StrCpy $Red "1"
  ${EndIf}
FunctionEnd

; texto del combo "id: Nombre" -> id
Function IdFromItem
  Exch $0
  Push $1
  Push $2
  StrCpy $1 0
  loop:
    StrCpy $2 $0 1 $1
    StrCmp $2 "" done
    StrCmp $2 ":" cut
    IntOp $1 $1 + 1
    Goto loop
  cut:
    StrCpy $0 $0 $1
  done:
  Pop $2
  Pop $1
  Exch $0
FunctionEnd

Function OptionsPage
  !insertmacro MUI_HEADER_TEXT "Opciones" "Oficina, modelo de IA y arranque automatico"
  nsDialogs::Create 1018
  Pop $0
  ${NSD_CreateLabel} 0 0 100% 12u "Perfil de oficina (agentes de tu gremio; se puede cambiar despues en Agentes):"
  Pop $0
  ${NSD_CreateDropList} 0 13u 100% 120u ""
  Pop $hCombo
  ${NSD_CB_AddString} $hCombo "(ninguno): agentes generales de Nexus-Office"
  !include "${PROFILES}"
  ${NSD_CB_SelectString} $hCombo "(ninguno): agentes generales de Nexus-Office"
  ${If} $Oficina != ""
    ${NSD_CB_SelectString} $hCombo "$Oficina:"
  ${EndIf}
  ${NSD_CreateCheckbox} 0 34u 100% 12u "Elegir y descargar el modelo de IA adecuado a este equipo (necesita internet)"
  Pop $hMod
  ${If} $Modelos != "no"
    ${NSD_Check} $hMod
  ${EndIf}
  ${NSD_CreateCheckbox} 0 50u 100% 12u "Arrancar Nexus-Office como servicio al encender el equipo (pide permiso de administrador)"
  Pop $hServ
  ${If} $Servicio == "1"
    ${NSD_Check} $hServ
  ${EndIf}
  ${NSD_CreateCheckbox} 12u 64u 95% 12u "Accesible desde la red local (moviles y otros PC de la oficina)"
  Pop $hRed
  ${If} $Red == "1"
    ${NSD_Check} $hRed
  ${EndIf}
  ${NSD_CreateCheckbox} 0 80u 100% 12u "Crear acceso directo en el escritorio"
  Pop $hDesk
  ${NSD_Check} $hDesk
  ${NSD_CreateLabel} 0 100u 100% 30u "El modelo se elige segun la memoria, la grafica y el procesador detectados. Sin internet, Nexus-Office lo recomienda y lo podras descargar despues desde Modelos > Recomendados."
  Pop $0
  nsDialogs::Show
FunctionEnd

Function OptionsLeave
  ${NSD_GetText} $hCombo $0
  Push $0
  Call IdFromItem
  Pop $0
  ${If} $0 == "(ninguno)"
    StrCpy $Oficina ""
  ${Else}
    StrCpy $Oficina $0
  ${EndIf}
  ${NSD_GetState} $hMod $0
  ${If} $0 == ${BST_CHECKED}
    ${If} $Modelos != "todos"
      StrCpy $Modelos "auto"
    ${EndIf}
  ${Else}
    StrCpy $Modelos "no"
  ${EndIf}
  ${NSD_GetState} $hServ $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $Servicio "1"
  ${Else}
    StrCpy $Servicio "0"
  ${EndIf}
  ${NSD_GetState} $hRed $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $Red "1"
  ${Else}
    StrCpy $Red "0"
  ${EndIf}
  ${NSD_GetState} $hDesk $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $Escritorio "1"
  ${Else}
    StrCpy $Escritorio "0"
  ${EndIf}
FunctionEnd

Section "Nexus-Office" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  DetailPrint "Copiando Nexus-Office (Python, dependencias y motores de IA incluidos)..."
  File /r "${PAYLOAD}\*.*"
  File "/oname=$INSTDIR\nexus.ico" "${ICON}"

  ; instalacion portable sin internet: runtime\ y wheelhouse\ incluidos; limpia lo que este equipo no usa
  StrCpy $0 "-Offline -Clean -Modelos no"
  ${If} $Oficina != ""
    StrCpy $0 "$0 -Oficina $Oficina"
  ${EndIf}
  DetailPrint "Preparando el entorno portable (puede tardar unos minutos)..."
  ExecWait 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\install.ps1" $0' $1
  ${If} $1 != 0
    MessageBox MB_ICONEXCLAMATION "La preparacion de Nexus-Office no termino bien (codigo $1). Puedes repetirla ejecutando install.bat en $INSTDIR." /SD IDOK
  ${EndIf}

  ${If} $Modelos != "no"
    DetailPrint "Evaluando el hardware y descargando el modelo de IA adecuado..."
    ${If} $Modelos == "todos"
      ExecWait '"$INSTDIR\venv\python.exe" -m nexus modelos --descargar --todos'
    ${Else}
      ExecWait '"$INSTDIR\venv\python.exe" -m nexus modelos --descargar'
    ${EndIf}
  ${EndIf}

  ${If} $Servicio == "1"
    DetailPrint "Instalando el servicio (arranque con el equipo)..."
    StrCpy $2 "-m nexus servicio instalar"
    ${If} $Red == "1"
      StrCpy $2 "$2 --red"
    ${EndIf}
    ; solo este paso pide permisos de administrador (UAC); sin ellos, arranca al iniciar sesion
    ExecWait `powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "try { $$p = Start-Process -FilePath '$INSTDIR\venv\python.exe' -ArgumentList '$2' -WorkingDirectory '$INSTDIR' -Verb RunAs -Wait -PassThru -WindowStyle Hidden; exit $$p.ExitCode } catch { exit 5 }"` $1
    ${If} $1 != 0
      DetailPrint "Sin permisos de administrador: el servicio arrancara al iniciar sesion"
      nsExec::ExecToLog '"$INSTDIR\venv\python.exe" $2'
    ${EndIf}
  ${EndIf}

  CreateDirectory "$SMPROGRAMS\Nexus-Office"
  CreateShortcut "$SMPROGRAMS\Nexus-Office\Nexus-Office.lnk" "$INSTDIR\venv\pythonw.exe" "-m nexus run" "$INSTDIR\nexus.ico" 0 SW_SHOWNORMAL "" "Abrir Nexus-Office"
  CreateShortcut "$SMPROGRAMS\Nexus-Office\Nexus-Office (consola).lnk" "$INSTDIR\start.bat" "" "$INSTDIR\nexus.ico"
  CreateShortcut "$SMPROGRAMS\Nexus-Office\Manual de usuario.lnk" "$INSTDIR\docs\USUARIO.md"
  CreateShortcut "$SMPROGRAMS\Nexus-Office\Desinstalar Nexus-Office.lnk" "$INSTDIR\uninstall.exe"
  ${If} $Escritorio == "1"
    CreateShortcut "$DESKTOP\Nexus-Office.lnk" "$INSTDIR\venv\pythonw.exe" "-m nexus run" "$INSTDIR\nexus.ico" 0 SW_SHOWNORMAL "" "Abrir Nexus-Office"
  ${EndIf}

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKCU "Software\Nexus-Office" "InstallDir" "$INSTDIR"
  !define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\Nexus-Office"
  WriteRegStr HKCU "${UNINST}" "DisplayName" "Nexus-Office"
  WriteRegStr HKCU "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST}" "Publisher" "Nexus-Office"
  WriteRegStr HKCU "${UNINST}" "DisplayIcon" "$INSTDIR\nexus.ico"
  WriteRegStr HKCU "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKCU "${UNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST}" "NoRepair" 1
SectionEnd

Function OpenNexus
  Exec '"$INSTDIR\venv\pythonw.exe" -m nexus run'
FunctionEnd

Section "Uninstall"
  ; el servicio: primero sin permisos (tarea de usuario) y, si era del sistema, con UAC
  nsExec::ExecToLog '"$INSTDIR\venv\python.exe" -m nexus servicio quitar'
  Pop $0
  ${If} $0 != 0
    ExecWait `powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '$INSTDIR\venv\python.exe' -ArgumentList '-m nexus servicio quitar' -WorkingDirectory '$INSTDIR' -Verb RunAs -Wait -WindowStyle Hidden"`
  ${EndIf}
  nsExec::Exec 'taskkill /F /IM llama-server.exe'
  Delete "$DESKTOP\Nexus-Office.lnk"
  RMDir /r "$SMPROGRAMS\Nexus-Office"
  MessageBox MB_YESNO|MB_ICONQUESTION "¿Borrar tambien tus datos (conversaciones, documentos, modelos de IA y configuracion)?$\r$\nSi eliges No, quedan en $INSTDIR\data" /SD IDNO IDYES all
    ; se conservan los datos
    Push $0
    FindFirst $0 $1 "$INSTDIR\*.*"
    loop:
      StrCmp $1 "" done
      StrCmp $1 "." next
      StrCmp $1 ".." next
      StrCmp $1 "data" next
      RMDir /r "$INSTDIR\$1"
      Delete "$INSTDIR\$1"
    next:
      FindNext $0 $1
      Goto loop
    done:
    FindClose $0
    Pop $0
    Goto regs
  all:
    RMDir /r "$INSTDIR"
  regs:
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Nexus-Office"
  DeleteRegKey HKCU "Software\Nexus-Office"
SectionEnd
