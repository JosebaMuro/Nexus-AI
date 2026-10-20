#!/data/data/com.termux/files/usr/bin/bash
# Nexus-Office en Android (ARM64) mediante Termux (instalalo desde F-Droid, no desde Play Store).
# PyPI no tiene wheels para Termux: lo que tiene codigo nativo (numpy, cryptography, pillow, psutil,
# pydantic-core, pyyaml) sale ya compilado de los paquetes de Termux (pkg), y pip solo instala Python puro.
# Si algun paquete no existe en tu repositorio de Termux, pip lo compila (por eso clang/rust).
# Sin compilar nada en el movil: la app Nexus-Office autonomo (APK) ya lleva todo dentro.
# El llama-server empaquetado por Termux (pkg llama-cpp) es el runtime LLM.
# Uso: bash install-termux.sh [--oficina=<perfil>] [--servicio [--red]] [--modelos=auto|todos|no]
#   --oficina   (o NEXUS_OFICINA=<perfil>): oficina de pyme, ver docs/OFICINA.md
#   --servicio  arranca Nexus-Office como servicio de Termux (termux-services; con la app Termux:Boot, al encender el movil)
#   --red       el servicio escucha en la red local (para entrar desde otros dispositivos)
set -euo pipefail
cd "$(dirname "$0")"
OFICINA="${NEXUS_OFICINA:-}"; SERVICIO=0; RED=0; MODELOS="${NEXUS_MODELOS:-auto}"
for a in "$@"; do case "$a" in --oficina=*) OFICINA="${a#--oficina=}";; --servicio) SERVICIO=1;; --red) RED=1;; --modelos=*) MODELOS="${a#--modelos=}";; esac; done
say() { printf '\033[36m[Nexus-Office]\033[0m %s\n' "$*"; }

say "Actualizando paquetes de Termux..."
pkg update -y
pkg install -y python clang make libyaml
EXTRA_PIP=()
for p in numpy cryptography pillow psutil pydantic-core pyyaml; do
  pkg install -y "python-$p" || { say "python-$p no disponible en pkg; se instalara con pip (compila, tarda)"; EXTRA_PIP+=("$p"); }
done
case " ${EXTRA_PIP[*]-} " in *" pydantic-core "*|*" cryptography "*) pkg install -y rust || true;; esac
pkg install -y llama-cpp || say "llama-cpp no disponible en tu repositorio; podras usar un nodo de la red cooperativa"
pkg install -y unrar p7zip || say "unrar/p7zip no disponibles: el RAG importara ZIP y TAR, pero no RAR ni 7z"

if [ ! -x venv/bin/python ]; then
  say "Creando entorno virtual (reutiliza los paquetes nativos de Termux)..."
  python -m venv --system-site-packages venv
fi
say "Instalando dependencias Python..."
# requirements.txt completo (antes faltaban fpdf2 y pypdf): lo que ya trae pkg cuenta como instalado
venv/bin/python -m pip install --disable-pip-version-check --prefer-binary -r requirements.txt ${EXTRA_PIP[@]+"${EXTRA_PIP[@]}"}

if [ -n "$OFICINA" ]; then
  say "Configurando la oficina: perfil $OFICINA"
  venv/bin/python -m nexus oficina "$OFICINA" || say "Perfil de oficina no aplicado (lista: venv/bin/python -m nexus oficina)"
fi
termux-setup-storage >/dev/null 2>&1 || true
say "Evaluando el hardware del movil y eligiendo el modelo de IA..."
case "$MODELOS" in
  no) venv/bin/python -m nexus modelos || true;;
  todos) venv/bin/python -m nexus modelos --descargar --todos || true;;
  *) venv/bin/python -m nexus modelos --descargar || true;;
esac
if [ "$SERVICIO" = 1 ]; then
  pkg install -y termux-services || true
  if [ "$RED" = 1 ]; then venv/bin/python -m nexus servicio instalar --red; else venv/bin/python -m nexus servicio instalar; fi || say "Servicio no instalado (ver docs/SERVICIO.md)"
fi
chmod +x start.sh
say "Listo. Arranca con: ./start.sh --host 0.0.0.0"
say "Abre http://127.0.0.1:8775 en el navegador del movil (o 'Instalar app' para usarlo como PWA)."
say "El modelo de IA lo elige Nexus-Office segun la memoria del movil (venv/bin/python -m nexus modelos); si va lento, usa la IA de otro nodo (Red COOP)."
