@echo off
rem Arranca Nexus-Office (edicion C++) y abre el navegador. Argumentos extra: --host 0.0.0.0 --port 8775 --no-browser
cd /d "%~dp0"
nexus-office.exe run %*
