@echo off
cd /d "%~dp0"
if not exist "%~dp0venv\python.exe" (
    echo [Nexus-Office] Runtime portable no encontrado. Ejecutando instalador...
    call "%~dp0install.bat"
)
"%~dp0venv\python.exe" -m nexus run %*
