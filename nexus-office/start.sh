#!/usr/bin/env bash
# Arranca Nexus-Office (Linux / macOS / Android-Termux). Argumentos extra: --host 0.0.0.0 --port 8775 --no-browser
cd "$(dirname "$0")"
for PY in ./venv/bin/python3 ./venv/bin/python; do
  if [ -x "$PY" ]; then exec "$PY" -m nexus run "$@"; fi
done
echo "[Nexus-Office] Runtime no encontrado: ejecutando el instalador..."
bash ./install.sh && exec ./venv/bin/python3 -m nexus run "$@"
