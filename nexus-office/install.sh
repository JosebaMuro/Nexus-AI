#!/usr/bin/env bash
# Nexus-Office - instalador portable para Linux (x86_64 / aarch64: PC, Steam Deck, Raspberry Pi 5, Jetson...)
# y macOS (Apple Silicon / Intel). Descarga un CPython completo y autonomo en ./venv.
# Uso: ./install.sh [--skip-extras] [--force] [--fetch-wheels] [--offline] [--clean | --no-clean] [--oficina=<perfil>]
#                   [--servicio [--red]] [--modelos=auto|todos|no]
#   --servicio      instala Nexus-Office como servicio (systemd/launchd/cron): arranca solo con el equipo. Con sudo,
#                   servicio del sistema; sin sudo, de usuario. Se quita con: venv/bin/python3 -m nexus servicio quitar
#   --red           el servicio escucha en la red local (0.0.0.0) para entrar desde moviles y otros equipos
#   --modelos=MODO  auto (por defecto): evalua el hardware, elige el modelo de chat, documentos y voz adecuados y los
#                   descarga si hay internet; todos: tambien el de imagen; no: solo muestra la recomendacion
#   --oficina=ID    configura la instalacion como la oficina de una pyme (herreria, caldereria, reformas...;
#                   lista: venv/bin/python3 -m nexus oficina). Tambien con NEXUS_OFICINA=ID
#   --fetch-wheels  descarga antes a ./wheelhouse (y el Python a ./runtime) todo lo que se instala, para
#                   reinstalar o copiar la carpeta a otro equipo y usar --offline
#   --offline       no usa internet: todo sale de ./runtime y ./wheelhouse (falla si falta algo)
#   --clean         al final, sin preguntar: borra lo que este equipo no usa y guarda lo usado para reinstalar
#   --no-clean      al final no limpia (por defecto pregunta si hay terminal; ver python -m nexus cleanup --help)
# Las dependencias se instalan siempre como wheels binarias: nada se compila (NEXUS_ALLOW_BUILD=1 lo permite).
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(pwd)"
VENV="$ROOT/venv"
PY="$VENV/bin/python3"
PYVER="${NEXUS_PYTHON_VERSION:-3.11}"
WHEELS="$ROOT/wheelhouse"
SERVICIO="${NEXUS_SERVICIO:-0}"; RED=0; MODELOS="${NEXUS_MODELOS:-auto}"
SKIP_EXTRAS=0; FORCE=0; FETCH=0; OFFLINE="${NEXUS_OFFLINE:-0}"; CLEAN="${NEXUS_CLEAN:-ask}"; OFICINA="${NEXUS_OFICINA:-}"
for a in "$@"; do case "$a" in --oficina=*) OFICINA="${a#--oficina=}";; --skip-extras) SKIP_EXTRAS=1;; --force) FORCE=1;; --fetch-wheels) FETCH=1;; --offline) OFFLINE=1;;
  --clean) CLEAN=yes;; --no-clean) CLEAN=no;; --servicio) SERVICIO=1;; --red) RED=1;; --modelos=*) MODELOS="${a#--modelos=}";; esac; done
say() { printf '\033[36m[Nexus-Office]\033[0m %s\n' "$*"; }
die() { printf '\033[31m[Nexus-Office]\033[0m %s\n' "$*" >&2; exit 1; }

if [ -n "${TERMUX_VERSION:-}" ] || [ -d /data/data/com.termux ]; then
  say "Android/Termux detectado: usando install-termux.sh"; exec bash "$ROOT/install-termux.sh" "$@"
fi

OS="$(uname -s)"; ARCH="$(uname -m)"
case "$ARCH" in x86_64|amd64) ARCH=x86_64;; aarch64|arm64) ARCH=aarch64;; *) say "Arquitectura no soportada: $ARCH"; exit 1;; esac
case "$OS" in
  Linux) TRIPLE="$ARCH-unknown-linux-gnu";;
  Darwin) TRIPLE="$ARCH-apple-darwin";;
  *) say "Sistema no soportado: $OS"; exit 1;;
esac
mkdir -p "$ROOT/.cache"
# macOS: si el paquete se descomprimio con el Finder, Gatekeeper bloquearia los binarios incluidos (no firmados por Apple)
if [ "$OS" = Darwin ] && [ -d "$ROOT/bundled" ]; then
  xattr -dr com.apple.quarantine "$ROOT/bundled" "$ROOT/runtime" 2>/dev/null || true
  chmod +x "$ROOT"/bundled/runtimes/*/*/llama-server "$ROOT"/bundled/runtimes/*/*/bin/llama-server \
    "$ROOT"/bundled/runtimes/*/*/sd-cli 2>/dev/null || true
fi

# La misma carpeta instalada antes desde Windows (venv\python.exe): no se borra ese runtime
if [ -f "$VENV/python.exe" ] && [ ! -x "$PY" ]; then
  die "Esta carpeta ya tiene el Nexus-Office de Windows (venv\python.exe). Para WSL/Linux copia Nexus-Office a tu carpeta de Linux:
       cp -r \"$ROOT\" ~/Nexus-Office && cd ~/Nexus-Office && rm -rf venv && ./install.sh"
fi
case "$ROOT" in /mnt/[a-z]/*) [ -n "${WSL_DISTRO_NAME:-}" ] && say "Aviso: estas en un disco de Windows ($ROOT). Funciona, pero en ~/ (disco de Linux) la instalacion y Nexus-Office van mucho mas rapido.";; esac

if [ -x "$PY" ] && [ "$FORCE" = 0 ]; then
  say "Runtime portable ya presente (usa --force para reinstalar)"
else
  rm -rf "$VENV"; mkdir -p "$ROOT/.cache"
  ARCHIVE=""
  if ls "$ROOT"/runtime/cpython-*"$TRIPLE"*.tar.gz >/dev/null 2>&1; then
    ARCHIVE="$(ls "$ROOT"/runtime/cpython-*"$TRIPLE"*.tar.gz | head -n1)"; say "Usando runtime incluido (offline): $(basename "$ARCHIVE")"
  else
    [ "$OFFLINE" = 1 ] && die "Modo offline: falta runtime/cpython-*-$TRIPLE-*.tar.gz (preparalo con ./install.sh --fetch-wheels en un equipo con internet)"
    command -v curl >/dev/null || die "Falta curl: sudo apt install curl  (o usa un paquete offline con runtime/)"
    say "Buscando python-build-standalone $PYVER para $TRIPLE..."
    URL="$(curl -fsSL -H 'User-Agent: nexus-office' https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest \
      | grep -o '"browser_download_url": *"[^"]*"' | cut -d'"' -f4 \
      | grep -E "cpython-${PYVER//./\\.}\.[0-9]+\+[0-9]+-${TRIPLE}-install_only(_stripped)?\.tar\.gz$" | sort | tail -n1 || true)"
    [ -z "$URL" ] && URL="https://github.com/astral-sh/python-build-standalone/releases/download/20261009/cpython-3.11.17+20261009-${TRIPLE}-install_only_stripped.tar.gz"
    ARCHIVE="$ROOT/.cache/python-standalone.tar.gz"
    [ "$FETCH" = 1 ] && { mkdir -p "$ROOT/runtime"; ARCHIVE="$ROOT/runtime/$(basename "${URL//%2B/+}")"; }
    say "Descargando $URL"; curl -fL --retry 3 -o "$ARCHIVE" "$URL"
  fi
  rm -rf "$ROOT/.cache/extract"; mkdir -p "$ROOT/.cache/extract"
  tar -xzf "$ARCHIVE" -C "$ROOT/.cache/extract"
  mv "$ROOT/.cache/extract/python" "$VENV"; rm -rf "$ROOT/.cache/extract"
  echo "Runtime CPython autonomo de Nexus-Office." > "$VENV/NEXUS_PORTABLE.txt"
  say "Runtime instalado: $("$PY" --version)"
fi

export NEXUS_OFFLINE="$OFFLINE"     # tambien lo respetan `nexus setup` y los extras (nexus/wheelhouse.py)
if [ "$FETCH" = 1 ]; then
  [ "$OFFLINE" = 1 ] && die "--fetch-wheels necesita internet (quita --offline)"
  say "Descargando wheels (base + voz + NPU) y el runtime para instalar sin internet..."
  "$PY" -m nexus wheels --runtime || say "Aviso: el wheelhouse no quedo completo (lo que falte se bajara de internet)"
fi

say "Instalando dependencias base..."
PIPARGS=(--disable-pip-version-check --no-warn-script-location)
[ "${NEXUS_ALLOW_BUILD:-0}" = 1 ] && PIPARGS+=(--prefer-binary) || PIPARGS+=(--only-binary=:all:)
[ -d "$ROOT/vendor/wheels" ] && PIPARGS+=(--find-links "$ROOT/vendor/wheels")   # wheels puras sin binaria en PyPI
installed=0
if ls "$WHEELS"/*.whl >/dev/null 2>&1; then
  say "Usando wheels locales de wheelhouse/ (sin internet)"
  if "$PY" -m pip install "${PIPARGS[@]}" --no-index --find-links "$WHEELS" -r "$ROOT/requirements.txt"; then
    installed=1
  elif [ "$OFFLINE" = 1 ]; then
    die "Faltan wheels en wheelhouse/ para esta plataforma ($TRIPLE). Preparalas con: ./install.sh --fetch-wheels"
  else
    say "wheelhouse/ incompleto para $TRIPLE: se completa desde internet"; PIPARGS+=(--find-links "$WHEELS")
  fi
elif [ "$OFFLINE" = 1 ]; then
  die "Modo offline sin wheelhouse/. Preparalo con: ./install.sh --fetch-wheels"
fi
if [ "$installed" = 0 ]; then
  "$PY" -m pip install --disable-pip-version-check --no-warn-script-location --upgrade pip || true
  "$PY" -m pip install "${PIPARGS[@]}" -r "$ROOT/requirements.txt" \
    || die "No se pudieron instalar las dependencias. Sin compilar no hay wheel para algun paquete en $TRIPLE; reintenta con NEXUS_ALLOW_BUILD=1 ./install.sh (necesita sudo apt install build-essential python3-dev)"
fi

say "Detectando hardware y generando el perfil optimo..."
"$PY" -m nexus detect > "$ROOT/.cache/hardware-report.json" || true
if [ -n "$OFICINA" ]; then
  say "Configurando la oficina: perfil $OFICINA"
  "$PY" -m nexus oficina "$OFICINA" || say "Perfil de oficina no aplicado (lista: $PY -m nexus oficina)"
fi
if [ "$SKIP_EXTRAS" = 0 ]; then
  say "Instalando extras recomendados para este hardware..."
  "$PY" -m nexus setup --auto || say "Algunos extras fallaron (se puede reintentar desde Sistema)"
fi

if [ "$OS" = Linux ] && [ -d "$HOME/.local/share/applications" -o -n "${XDG_DATA_HOME:-}" ]; then
  APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"; mkdir -p "$APPS"
  cat > "$APPS/nexus-office.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Nexus-Office
Comment=Agente de IA multimodal portable
Exec="$ROOT/start.sh"
Icon=$ROOT/nexus/web/icon.svg
Terminal=true
Categories=Utility;Education;Science;
EOF
  say "Acceso directo creado en el menu de aplicaciones (en Steam Deck: anadelo como juego no-Steam)"
fi
chmod +x "$ROOT/start.sh" 2>/dev/null || true
case "$MODELOS" in
  no) say "Modelo de IA recomendado para este equipo:"; "$PY" -m nexus modelos || true;;
  todos) say "Evaluando el hardware y eligiendo los modelos de IA (incluida imagen)..."; "$PY" -m nexus modelos --descargar --todos || say "Descarga de modelos incompleta: reintenta con venv/bin/python3 -m nexus modelos --descargar";;
  *) say "Evaluando el hardware y eligiendo el modelo de IA..."; "$PY" -m nexus modelos --descargar || say "Descarga de modelos incompleta: reintenta con venv/bin/python3 -m nexus modelos --descargar";;
esac
# Limpieza: el paquete offline trae wheels y runtimes para cualquier GPU/NPU; aqui se quita lo que no usa este equipo
if [ "$CLEAN" != no ] && { ls "$WHEELS"/*.whl >/dev/null 2>&1 || [ -d "$ROOT/bundled/runtimes" ]; }; then
  if [ "$CLEAN" = yes ]; then "$PY" -m nexus cleanup --yes || true
  elif [ -t 0 ]; then "$PY" -m nexus cleanup || true
  else say "Para quitar lo que este equipo no usa: venv/bin/python3 -m nexus cleanup"; fi
fi
if [ "$SERVICIO" = 1 ]; then
  say "Instalando Nexus-Office como servicio..."
  SVC=(); [ "$RED" = 1 ] && SVC+=(--red)
  "$PY" -m nexus servicio instalar ${SVC[@]+"${SVC[@]}"} || say "No se pudo instalar el servicio (ver docs/SERVICIO.md)"
fi
say "Instalacion completada. Ejecuta ./start.sh"
