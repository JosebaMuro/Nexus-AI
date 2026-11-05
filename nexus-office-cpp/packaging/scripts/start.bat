@echo off
rem Arranca Agente IA (edicion C++) y abre el navegador. Argumentos extra: --host 0.0.0.0 --port 8775 --no-browser
cd /d "%~dp0"
nexus-office.exe run %*
