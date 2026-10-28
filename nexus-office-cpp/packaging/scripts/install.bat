@echo off
rem Nexus-Office (edicion C++) - preparacion en Windows. Uso:
rem   install.bat [--oficina=<perfil>] [--servicio [--red]] [--modelos=auto^|todos^|no]
setlocal EnableDelayedExpansion
cd /d "%~dp0"
set OFICINA=%NEXUS_OFICINA%
set SERVICIO=0
set RED=
set MODELOS=auto
:args
if "%~1"=="" goto run
set A=%~1
if /i "!A:~0,10!"=="--oficina=" set OFICINA=!A:~10!
if /i "!A!"=="--servicio" set SERVICIO=1
if /i "!A!"=="--red" set RED=--red
if /i "!A:~0,10!"=="--modelos=" set MODELOS=!A:~10!
shift
goto args
:run
nexus-office.exe --version || exit /b 1
if not "%OFICINA%"=="" nexus-office.exe oficina %OFICINA%
if /i "%MODELOS%"=="no" (nexus-office.exe modelos) else if /i "%MODELOS%"=="todos" (nexus-office.exe modelos --descargar --todos) else (nexus-office.exe modelos --descargar)
if "%SERVICIO%"=="1" nexus-office.exe servicio instalar %RED%
echo [Nexus-Office] Listo. Arranca con start.bat
exit /b 0
