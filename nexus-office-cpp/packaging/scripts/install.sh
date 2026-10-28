#!/bin/sh
# Nexus-Office (edicion C++) - preparacion en Linux y macOS. No instala nada mas: el programa es un unico
# ejecutable (nexus-office) con la interfaz web y los motores de IA al lado. Funciona sin internet.
# Uso: ./install.sh [--oficina=<perfil>] [--servicio [--red]] [--modelos=auto|todos|no]
#   --oficina=ID    configura la oficina de una pyme (herreria, reformas...; lista: ./nexus-office oficina)
#   --servicio      arranque automatico con el equipo (systemd/launchd/cron); con sudo, del sistema; --red: accesible
#                   desde moviles y otros equipos de la red local. Se quita con: ./nexus-office servicio quitar
#   --modelos=MODO  auto (por defecto): evalua el hardware, elige los modelos de chat, documentos y voz y los descarga
#                   si hay internet; todos: tambien el de imagen; no: solo muestra la recomendacion
# Se aceptan (y no hacen nada) las opciones de la edicion Python: --offline --clean --no-clean --skip-extras --force
set -eu
cd "$(dirname "$0")"
OFICINA="${NEXUS_OFICINA:-}"; SERVICIO="${NEXUS_SERVICIO:-0}"; RED=0; MODELOS="${NEXUS_MODELOS:-auto}"
for a in "$@"; do case "$a" in --oficina=*) OFICINA="${a#--oficina=}";; --servicio) SERVICIO=1;; --red) RED=1;;
  --modelos=*) MODELOS="${a#--modelos=}";; esac; done
say() { printf '\033[36m[Nexus-Office]\033[0m %s\n' "$*"; }
# macOS: si se descomprimio con el Finder, Gatekeeper bloquearia los ejecutables (no firmados por Apple)
if [ "$(uname -s)" = Darwin ]; then xattr -dr com.apple.quarantine . 2>/dev/null || true; fi
chmod +x nexus-office start.sh 2>/dev/null || true
find bundled -type f \( -name llama-server -o -name sd-cli -o -name piper -o -name whisper-cli \) -exec chmod +x {} + 2>/dev/null || true
./nexus-office --version
if [ -n "$OFICINA" ]; then say "Oficina: $OFICINA"; ./nexus-office oficina "$OFICINA"; fi
case "$MODELOS" in
  no) ./nexus-office modelos || true;;
  todos) say "Evaluando el hardware y descargando los modelos de IA..."; ./nexus-office modelos --descargar --todos || say "Sin internet: descarga los modelos despues desde Modelos > Recomendados";;
  *) say "Evaluando el hardware y descargando el modelo de IA adecuado..."; ./nexus-office modelos --descargar || say "Sin internet: descarga el modelo despues desde Modelos > Recomendados";;
esac
if [ "$SERVICIO" = 1 ]; then
  if [ "$RED" = 1 ]; then ./nexus-office servicio instalar --red; else ./nexus-office servicio instalar; fi
fi
say "Listo. Arranca con: $(pwd)/start.sh"
