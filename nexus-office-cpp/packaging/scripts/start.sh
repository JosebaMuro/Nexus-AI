#!/bin/sh
# Arranca Agente IA (edicion C++) y abre el navegador. Argumentos extra: --host 0.0.0.0 --port 8775 --no-browser
cd "$(dirname "$0")"
exec ./nexus-office run "$@"
