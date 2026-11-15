#!/usr/bin/env bash
# rehacer-ubuntu.sh - rehace una ISO de Ubuntu Server con un proyecto Python preinstalado como demonio (servicio
# systemd activo y preconfigurado) en la instalacion MINIMA de Ubuntu, que queda como opcion por defecto.
#
# Por defecto integra Agente IA a partir de su paquete .deb (100 % offline: Python, dependencias y motores dentro).
# Sirve para cualquier proyecto Python: con --proyecto <carpeta> y --orden "<orden>" crea su entorno virtual con las
# dependencias (requirements.txt o pyproject.toml) y su servicio.
#
# Uso:
#   sudo ./rehacer-ubuntu.sh --iso ubuntu-24.04.4-live-server-amd64.iso --deb nexus-office_4.2.0_amd64.deb
#   sudo ./rehacer-ubuntu.sh --iso ubuntu.iso --proyecto ~/mi-api --orden "python -m mi_api --puerto 8000" \
#        --nombre mi-api --paquetes "python3-venv" --puerto 8000
#
# Opciones:
#   --iso FICHERO          ISO original de Ubuntu Server (22.04 o posterior; amd64 o arm64). Obligatoria.
#   --descargar VERSION    descarga antes la ISO de releases.ubuntu.com (p. ej. 24.04 o 26.04) y comprueba su SHA256
#                          (ARQ_ISO=arm64 para la de ARM de cdimage.ubuntu.com)
#   --salida FICHERO       ISO nueva (por defecto: <iso>-<nombre>.iso junto a la original)
#   --deb FICHERO          paquete .deb del proyecto (Agente IA: nexus-office_<version>_<arq>.deb)
#   --proyecto CARPETA     proyecto Python que se copia a /opt/<nombre>/app con su venv en /opt/<nombre>/venv
#   --orden "ORDEN"        orden del servicio. «python ...» usa el Python del venv del proyecto
#   --nombre NOMBRE        servicio, usuario del sistema y carpetas (por defecto: agente-ia)
#   --descripcion TEXTO    descripcion del servicio
#   --entorno CLAVE=VALOR  variable de entorno del servicio (se puede repetir)
#   --paquetes "a b"       paquetes de Ubuntu que se instalan dentro (apt; necesita red al construir)
#   --puerto N             puerto que se comprueba con --probar (Agente IA: 8775)
#   --solo-local           Agente IA escucha solo en 127.0.0.1 (por defecto en la red, protegido con su clave de red)
#   --capa NOMBRE          capa de casper que se modifica (por defecto la minima: ubuntu-server-minimal.squashfs)
#   --desatendida          instalacion automatica (autoinstall) con la capa minima; pide --usuario y --clave-hash
#   --usuario U            usuario del sistema instalado (con --desatendida)
#   --clave-hash HASH      clave cifrada (openssl passwd -6) del usuario (con --desatendida)
#   --equipo NOMBRE        nombre del equipo instalado (con --desatendida; por defecto el --nombre)
#   --probar               arranca el demonio dentro de la imagen, pide su pagina y comprueba que queda activo
#   --trabajo CARPETA      carpeta de trabajo (por defecto una temporal junto a la salida)
#   --conservar            no borra la carpeta de trabajo al terminar
#   --sin-venv             proyecto sin dependencias: usa el python3 de Ubuntu, sin entorno virtual ni apt
#   --serie                el instalador tambien escribe en el puerto serie (servidores sin pantalla, maquinas virtuales)
#
# Necesita root (chroot), xorriso, squashfs-tools y, para --deb, dpkg. Con una ISO arm64 en un PC x86_64 hace falta
# qemu-user-static (binfmt) para el chroot. Solo modifica la capa minima y unos ficheros de la ISO: el arranque
# (BIOS y UEFI) se copia tal cual de la original con «xorriso -boot_image any replay».
set -euo pipefail

say() { printf '\033[36m[ubuntu]\033[0m %s\n' "$*"; }
die() { printf '\033[31m[ubuntu]\033[0m %s\n' "$*" >&2; exit 1; }

ISO=""; DESCARGAR=""; SALIDA=""; DEB=""; PROYECTO=""; ORDEN=""; NOMBRE="agente-ia"; DESCRIPCION=""; PAQUETES=""
PAQUETES_DADOS=0; PUERTO=""; SOLO_LOCAL=0; CAPA=""; DESATENDIDA=0; USUARIO=""; CLAVE_HASH=""; EQUIPO=""; PROBAR=0
TRABAJO=""; CONSERVAR=0; ENTORNO=(); SIN_VENV=0; SERIE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --iso) ISO="$2"; shift;;
    --descargar) DESCARGAR="$2"; shift;;
    --salida) SALIDA="$2"; shift;;
    --deb) DEB="$2"; shift;;
    --proyecto) PROYECTO="$2"; shift;;
    --orden) ORDEN="$2"; shift;;
    --nombre) NOMBRE="$2"; shift;;
    --descripcion) DESCRIPCION="$2"; shift;;
    --entorno) ENTORNO+=("$2"); shift;;
    --paquetes) PAQUETES="$2"; PAQUETES_DADOS=1; shift;;
    --puerto) PUERTO="$2"; shift;;
    --solo-local) SOLO_LOCAL=1;;
    --capa) CAPA="$2"; shift;;
    --desatendida) DESATENDIDA=1;;
    --usuario) USUARIO="$2"; shift;;
    --clave-hash) CLAVE_HASH="$2"; shift;;
    --equipo) EQUIPO="$2"; shift;;
    --probar) PROBAR=1;;
    --trabajo) TRABAJO="$2"; shift;;
    --conservar) CONSERVAR=1;;
    --sin-venv) SIN_VENV=1;;
    --serie) SERIE=1;;
    -h|--help|--ayuda) sed -n '2,42p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) die "Opcion desconocida: $1 (ver --ayuda)";;
  esac
  shift
done

[ "$(id -u)" = 0 ] || die "Hace falta root (sudo): se monta y se usa chroot dentro de la imagen"
for t in xorriso unsquashfs mksquashfs md5sum chroot mount; do
  command -v "$t" >/dev/null || die "Falta $t (sudo apt install xorriso squashfs-tools)"
done
[[ "$NOMBRE" =~ ^[a-z][a-z0-9-]{0,30}$ ]] || die "--nombre: minusculas, numeros y guiones (empieza por letra)"
[ -n "$DEB" ] || [ -n "$PROYECTO" ] || die "Indica el proyecto: --deb paquete.deb o --proyecto carpeta"
[ -z "$DEB" ] || [ -f "$DEB" ] || die "No existe el paquete $DEB"
[ -z "$PROYECTO" ] || [ -d "$PROYECTO" ] || die "No existe la carpeta $PROYECTO"
if [ "$DESATENDIDA" = 1 ]; then
  [ -n "$USUARIO" ] && [ -n "$CLAVE_HASH" ] || die "--desatendida necesita --usuario y --clave-hash (openssl passwd -6)"
fi

# ------------------------------------------------------------------ ISO original
if [ -n "$DESCARGAR" ]; then
  command -v curl >/dev/null || die "Falta curl para --descargar"
  ARQ_ISO="${ARQ_ISO:-amd64}"
  BASE="https://releases.ubuntu.com/$DESCARGAR"
  [ "$ARQ_ISO" = arm64 ] && BASE="https://cdimage.ubuntu.com/releases/$DESCARGAR/release"
  SUMAS="$(curl -fsSL "$BASE/SHA256SUMS")" || die "No se pudo leer $BASE/SHA256SUMS"
  NOMBRE_ISO="$(printf '%s\n' "$SUMAS" | grep -o "ubuntu-[0-9.]*-live-server-$ARQ_ISO\.iso" | sort -V | tail -1)"
  [ -n "$NOMBRE_ISO" ] || die "No hay ISO live-server-$ARQ_ISO en $BASE"
  ISO="${ISO:-$PWD/$NOMBRE_ISO}"
  if [ ! -f "$ISO" ]; then say "Descargando $NOMBRE_ISO"; curl -fL --retry 4 -o "$ISO" "$BASE/$NOMBRE_ISO"; fi
  ESPERADA="$(printf '%s\n' "$SUMAS" | grep " \*\{0,1\}$NOMBRE_ISO\$" | cut -d' ' -f1)"
  [ "$(sha256sum "$ISO" | cut -d' ' -f1)" = "$ESPERADA" ] || die "La suma SHA256 de $ISO no coincide"
fi
[ -f "$ISO" ] || die "Indica la ISO original con --iso (o --descargar 24.04)"
ISO="$(readlink -f "$ISO")"
SALIDA="${SALIDA:-${ISO%.iso}-$NOMBRE.iso}"
SALIDA="$(readlink -f "$(dirname "$SALIDA")")/$(basename "$SALIDA")"
[ "$SALIDA" != "$ISO" ] || die "La salida no puede ser la ISO original"
TRABAJO="${TRABAJO:-$(mktemp -d "$(dirname "$SALIDA")/.rehacer-ubuntu.XXXXXX")}"
mkdir -p "$TRABAJO"
TRABAJO="$(readlink -f "$TRABAJO")"
RAIZ="$TRABAJO/raiz"; MEDIO="$TRABAJO/medio"
MONTADOS=()

limpiar() {
  local i
  if [ -n "${PID_PRUEBA:-}" ]; then kill "$PID_PRUEBA" 2>/dev/null || true; fi
  [ -d "${RAIZ:-}" ] && matar_dentro
  for ((i=${#MONTADOS[@]}-1; i>=0; i--)); do umount -lf "${MONTADOS[$i]}" 2>/dev/null || true; done
  if [ "$CONSERVAR" = 0 ]; then rm -rf "$TRABAJO"; else say "Carpeta de trabajo conservada: $TRABAJO"; fi
}
trap limpiar EXIT

# ------------------------------------------------------------------ capa minima
say "Leyendo $(basename "$ISO")"
LISTA="$(xorriso -indev "$ISO" -find /casper -name '*.squashfs' 2>/dev/null | tr -d "'" | sed 's|^/casper/||')"
if [ -z "$CAPA" ]; then
  for c in ubuntu-server-minimal.squashfs minimal.squashfs filesystem.squashfs; do
    if printf '%s\n' "$LISTA" | grep -qx "$c"; then CAPA="$c"; break; fi
  done
fi
[ -n "$CAPA" ] && printf '%s\n' "$LISTA" | grep -qx "$CAPA" || die "No encuentro la capa en casper/ (hay: $(echo $LISTA))"
ID_CAPA="${CAPA%.squashfs}"
say "Capa que se modifica: casper/$CAPA (instalacion minima)"
mkdir -p "$MEDIO/casper"
extraer() { xorriso -osirrox on -indev "$ISO" -extract "$1" "$2" >/dev/null 2>&1; }
extraer "/casper/$CAPA" "$TRABAJO/capa.squashfs" || die "No se pudo extraer casper/$CAPA"
for f in "$ID_CAPA.size" "$ID_CAPA.manifest" install-sources.yaml SHA256SUMS; do
  extraer "/casper/$f" "$MEDIO/casper/$f" || rm -f "$MEDIO/casper/$f"
done
extraer /md5sum.txt "$MEDIO/md5sum.txt" || rm -f "$MEDIO/md5sum.txt"
mkdir -p "$MEDIO/boot/grub"
extraer /boot/grub/grub.cfg "$MEDIO/boot/grub/grub.cfg" || rm -f "$MEDIO/boot/grub/grub.cfg"
chmod -R u+w "$MEDIO"

say "Descomprimiendo la capa (unos minutos)"
unsquashfs -q -d "$RAIZ" "$TRABAJO/capa.squashfs" >/dev/null
rm -f "$TRABAJO/capa.squashfs"
[ -x "$RAIZ/bin/sh" ] || [ -L "$RAIZ/bin" ] || die "La capa no parece un sistema completo"
ARQ_RAIZ="$(chroot "$RAIZ" dpkg --print-architecture 2>/dev/null)" || \
  die "No se puede ejecutar nada dentro de la imagen: con una ISO de otra arquitectura instala qemu-user-static"
say "Sistema de la imagen: $(. "$RAIZ/etc/os-release"; echo "$PRETTY_NAME") ($ARQ_RAIZ)"

montar() {
  mount --bind /dev "$RAIZ/dev"; MONTADOS+=("$RAIZ/dev")
  mount --bind /dev/pts "$RAIZ/dev/pts"; MONTADOS+=("$RAIZ/dev/pts")
  mount -t proc proc "$RAIZ/proc"; MONTADOS+=("$RAIZ/proc")
  mount -t sysfs sys "$RAIZ/sys"; MONTADOS+=("$RAIZ/sys")
  mount -t tmpfs tmpfs "$RAIZ/run"; MONTADOS+=("$RAIZ/run")
  mkdir -p "$RAIZ/run/systemd/resolve"
  cp -L /etc/resolv.conf "$RAIZ/run/systemd/resolve/stub-resolv.conf" 2>/dev/null || true
  RESOLV_ORIG=""
  if [ ! -L "$RAIZ/etc/resolv.conf" ]; then   # enlace a /run/systemd/resolve: ya vale con lo copiado arriba
    RESOLV_ORIG="$(mktemp)"
    if [ -e "$RAIZ/etc/resolv.conf" ]; then cp -a "$RAIZ/etc/resolv.conf" "$RESOLV_ORIG"; else echo "__ninguno__" > "$RESOLV_ORIG"; fi
    cp -L /etc/resolv.conf "$RAIZ/etc/resolv.conf" 2>/dev/null || true
  fi
  printf '#!/bin/sh\nexit 101\n' > "$RAIZ/usr/sbin/policy-rc.d"; chmod +x "$RAIZ/usr/sbin/policy-rc.d"
}
matar_dentro() {   # procesos que siguen vivos con la raiz de la imagen (prueba, demonios de los paquetes)
  local p r
  for p in /proc/[0-9]*; do
    r="$(readlink "$p/root" 2>/dev/null)" || continue
    if [ "$r" = "$RAIZ" ]; then kill -9 "${p#/proc/}" 2>/dev/null || true; fi
  done
}
desmontar() {
  matar_dentro
  rm -f "$RAIZ/usr/sbin/policy-rc.d"
  if [ -n "${RESOLV_ORIG:-}" ]; then
    if [ "$(cat "$RESOLV_ORIG")" = "__ninguno__" ]; then rm -f "$RAIZ/etc/resolv.conf"; else cp -a "$RESOLV_ORIG" "$RAIZ/etc/resolv.conf"; fi
    rm -f "$RESOLV_ORIG"; RESOLV_ORIG=""
  fi
  local i
  for ((i=${#MONTADOS[@]}-1; i>=0; i--)); do umount -lf "${MONTADOS[$i]}" 2>/dev/null || true; done
  MONTADOS=()
}
PROXY=()   # el proxy del equipo que construye vale tambien para apt y pip dentro de la imagen
for v in http_proxy https_proxy no_proxy HTTP_PROXY HTTPS_PROXY NO_PROXY; do
  [ -n "${!v:-}" ] && PROXY+=("$v=${!v}")
done
dentro() { chroot "$RAIZ" /usr/bin/env -i HOME=/root LANG=C.UTF-8 DEBIAN_FRONTEND=noninteractive \
             PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin ${PROXY[@]+"${PROXY[@]}"} "$@"; }

montar

# ------------------------------------------------------------------ proyecto
PAQ_DEB=""
if [ -n "$DEB" ]; then
  PAQ_DEB="$(dpkg-deb -f "$DEB" Package 2>/dev/null || true)"
  ARQ_DEB="$(dpkg-deb -f "$DEB" Architecture 2>/dev/null || true)"
  [ -z "$ARQ_DEB" ] || [ "$ARQ_DEB" = all ] || [ "$ARQ_DEB" = "$ARQ_RAIZ" ] || \
    die "El paquete es para $ARQ_DEB y la ISO para $ARQ_RAIZ"
fi
AGENTE_IA=0; [ "$PAQ_DEB" = nexus-office ] && AGENTE_IA=1
if [ "$AGENTE_IA" = 1 ]; then
  # Agente IA: el paquete trae Python, dependencias y motores; solo faltan bibliotecas que no estan en la minima
  [ "$PAQUETES_DADOS" = 1 ] || PAQUETES="libgomp1 libvulkan1 ca-certificates"
  DESCRIPCION="${DESCRIPCION:-Agente IA (oficina de IA local)}"
  PUERTO="${PUERTO:-8775}"
  HOST=0.0.0.0; [ "$SOLO_LOCAL" = 1 ] && HOST=127.0.0.1
  ORDEN="${ORDEN:-/opt/nexus-office/venv/bin/python3 -m nexus run --no-browser --host $HOST --port $PUERTO}"
  DIR_TRABAJO=/opt/nexus-office
  ENTORNO=("NEXUS_DATA_DIR=/var/lib/$NOMBRE" "PYTHONUNBUFFERED=1" "${ENTORNO[@]+"${ENTORNO[@]}"}")
else
  [ -n "$ORDEN" ] || die "Indica la orden del servicio con --orden (p. ej. \"python -m mi_paquete\")"
  DESCRIPCION="${DESCRIPCION:-$NOMBRE}"
  DIR_TRABAJO="/opt/$NOMBRE/app"
  [ -n "$DEB" ] && DIR_TRABAJO="/opt/$NOMBRE"
  ENTORNO=("PYTHONUNBUFFERED=1" "${ENTORNO[@]+"${ENTORNO[@]}"}")
  if [ -n "$PROYECTO" ] && [ "$SIN_VENV" = 0 ] && ! dentro python3 -c 'import ensurepip' 2>/dev/null; then
    case " $PAQUETES " in *" python3-venv "*) ;; *) PAQUETES="$PAQUETES python3-venv";; esac
  fi
fi

if [ -n "${PAQUETES// /}" ]; then
  say "Instalando paquetes de Ubuntu dentro de la imagen: $PAQUETES"
  dentro apt-get update -qq || die "apt-get update fallo (hace falta red al construir para --paquetes)"
  # shellcheck disable=SC2086
  dentro apt-get install -y -qq --no-install-recommends $PAQUETES || die "No se pudieron instalar: $PAQUETES"
  dentro apt-get clean
  rm -rf "$RAIZ"/var/lib/apt/lists/*_Packages "$RAIZ"/var/lib/apt/lists/*_Sources "$RAIZ"/var/lib/apt/lists/*Translation*
fi

if [ -n "$DEB" ]; then
  say "Instalando $(basename "$DEB") (puede tardar: prepara su Python y dependencias sin internet)"
  cp "$DEB" "$RAIZ/tmp/proyecto.deb"
  dentro env NEXUS_OFFLINE=1 dpkg -i /tmp/proyecto.deb || die "dpkg -i fallo"
  rm -f "$RAIZ/tmp/proyecto.deb"
  # lo que el instalador detecto del PC donde se construye no vale para el equipo final
  rm -rf "$RAIZ/var/lib/nexus-office" "$RAIZ/opt/nexus-office/.cache/hardware-report.json"
fi

if [ -n "$PROYECTO" ]; then
  say "Copiando el proyecto a /opt/$NOMBRE/app"
  mkdir -p "$RAIZ/opt/$NOMBRE"
  rm -rf "$RAIZ/opt/$NOMBRE/app"
  cp -a "$PROYECTO" "$RAIZ/opt/$NOMBRE/app"
  rm -rf "$RAIZ/opt/$NOMBRE/app/.git" "$RAIZ/opt/$NOMBRE/app/venv" "$RAIZ/opt/$NOMBRE/app/.venv"
  find "$RAIZ/opt/$NOMBRE/app" -name __pycache__ -type d -prune -exec rm -rf {} +
fi
if [ -n "$PROYECTO" ] && [ "$SIN_VENV" = 1 ]; then
  dentro python3 -m compileall -q "/opt/$NOMBRE/app" >/dev/null || true
  case "$ORDEN" in python\ *) ORDEN="/usr/bin/python3 ${ORDEN#python }";; python3\ *) ORDEN="/usr/bin/$ORDEN";; esac
elif [ -n "$PROYECTO" ]; then
  say "Creando su entorno virtual con las dependencias"
  dentro python3 -m venv "/opt/$NOMBRE/venv"
  dentro "/opt/$NOMBRE/venv/bin/python" -m pip install -q --disable-pip-version-check --upgrade pip || true
  if [ -f "$RAIZ/opt/$NOMBRE/app/requirements.txt" ]; then
    dentro "/opt/$NOMBRE/venv/bin/python" -m pip install -q --disable-pip-version-check \
      -r "/opt/$NOMBRE/app/requirements.txt" || die "pip install -r requirements.txt fallo"
  fi
  if [ -f "$RAIZ/opt/$NOMBRE/app/pyproject.toml" ] || [ -f "$RAIZ/opt/$NOMBRE/app/setup.py" ]; then
    dentro "/opt/$NOMBRE/venv/bin/python" -m pip install -q --disable-pip-version-check "/opt/$NOMBRE/app" \
      || die "pip install del proyecto fallo"
  fi
  dentro "/opt/$NOMBRE/venv/bin/python" -m compileall -q "/opt/$NOMBRE/app" >/dev/null || true
  case "$ORDEN" in python\ *|python3\ *) ORDEN="/opt/$NOMBRE/venv/bin/$ORDEN";; esac
fi

# ------------------------------------------------------------------ usuario, carpetas y servicio
say "Creando el usuario del sistema y el servicio $NOMBRE"
if ! dentro getent passwd "$NOMBRE" >/dev/null; then
  dentro useradd --system --home-dir "/var/lib/$NOMBRE" --shell /usr/sbin/nologin --user-group "$NOMBRE"
fi
for g in video render; do   # GPU y NPU: /dev/dri, /dev/accel
  dentro getent group "$g" >/dev/null && dentro usermod -aG "$g" "$NOMBRE"
done
mkdir -p "$RAIZ/var/lib/$NOMBRE" "$RAIZ/etc/$NOMBRE"
dentro chown "$NOMBRE:$NOMBRE" "/var/lib/$NOMBRE"
chmod 750 "$RAIZ/var/lib/$NOMBRE"
{
  echo "# Variables del servicio $NOMBRE (systemctl restart $NOMBRE tras cambiarlas)"
  for e in "${ENTORNO[@]}"; do echo "$e"; done
} > "$RAIZ/etc/$NOMBRE/$NOMBRE.env"
chmod 640 "$RAIZ/etc/$NOMBRE/$NOMBRE.env"; dentro chgrp "$NOMBRE" "/etc/$NOMBRE/$NOMBRE.env"

cat > "$RAIZ/etc/systemd/system/$NOMBRE.service" <<EOF
[Unit]
Description=$DESCRIPCION
After=network-online.target
Wants=network-online.target
# no arranca en el instalador en vivo de la ISO, solo en el sistema instalado
ConditionKernelCommandLine=!boot=casper

[Service]
Type=simple
User=$NOMBRE
Group=$NOMBRE
WorkingDirectory=$DIR_TRABAJO
EnvironmentFile=-/etc/$NOMBRE/$NOMBRE.env
ExecStart=$ORDEN
Restart=on-failure
RestartSec=5
TimeoutStopSec=30
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
EOF
dentro systemctl enable "$NOMBRE.service" >/dev/null 2>&1 || \
  mkdir -p "$RAIZ/etc/systemd/system/multi-user.target.wants" && ln -sf "/etc/systemd/system/$NOMBRE.service" "$RAIZ/etc/systemd/system/multi-user.target.wants/$NOMBRE.service"
[ -L "$RAIZ/etc/systemd/system/multi-user.target.wants/$NOMBRE.service" ] || die "No se pudo activar el servicio"

if [ "$AGENTE_IA" = 1 ]; then
  # orden corta y aviso al entrar por consola o SSH con la direccion y la clave de red
  cat > "$RAIZ/usr/local/bin/$NOMBRE" <<EOF
#!/bin/sh
# Agente IA preinstalado: estado del servicio, direccion y clave de red. Con argumentos, orden de nexus (sudo).
if [ \$# -gt 0 ]; then
  cd /opt/nexus-office && exec sudo -u $NOMBRE env NEXUS_DATA_DIR=/var/lib/$NOMBRE /opt/nexus-office/venv/bin/python3 -m nexus "\$@"
fi
systemctl --no-pager status $NOMBRE | head -3
CLAVE=\$(sudo grep -m1 'lan_key:' /var/lib/$NOMBRE/config.yaml 2>/dev/null | sed 's/.*lan_key: *//; s/["'"'"']//g')
for ip in \$(hostname -I 2>/dev/null); do echo "Abre http://\$ip:$PUERTO/\${CLAVE:+?key=\$CLAVE}"; done
EOF
  chmod 755 "$RAIZ/usr/local/bin/$NOMBRE"
  if [ -d "$RAIZ/etc/update-motd.d" ]; then
    cat > "$RAIZ/etc/update-motd.d/90-$NOMBRE" <<EOF
#!/bin/sh
ESTADO=\$(systemctl is-active $NOMBRE 2>/dev/null)
CLAVE=\$(grep -m1 'lan_key:' /var/lib/$NOMBRE/config.yaml 2>/dev/null | sed 's/.*lan_key: *//; s/["'"'"']//g')
echo ""
echo " Agente IA: \$ESTADO. Desde otro equipo de la red:"
for ip in \$(hostname -I 2>/dev/null); do echo "   http://\$ip:$PUERTO/\${CLAVE:+?key=\$CLAVE}"; done
echo " Estado y clave: $NOMBRE   |   registro: journalctl -u $NOMBRE"
EOF
    chmod 755 "$RAIZ/etc/update-motd.d/90-$NOMBRE"
  fi
fi

# ------------------------------------------------------------------ prueba dentro de la imagen
if [ "$PROBAR" = 1 ]; then
  say "Probando el demonio dentro de la imagen"
  [ "$(dentro systemctl is-enabled "$NOMBRE.service" 2>/dev/null || true)" = enabled ] || \
    [ -L "$RAIZ/etc/systemd/system/multi-user.target.wants/$NOMBRE.service" ] || die "El servicio no queda activado"
  PRUEBA_DATOS="/tmp/prueba-$NOMBRE"
  mkdir -p "$RAIZ$PRUEBA_DATOS"; dentro chown "$NOMBRE:$NOMBRE" "$PRUEBA_DATOS"
  ENV_PRUEBA=()
  for e in "${ENTORNO[@]}"; do
    case "$e" in NEXUS_DATA_DIR=*) ENV_PRUEBA+=("NEXUS_DATA_DIR=$PRUEBA_DATOS");; *) ENV_PRUEBA+=("$e");; esac
  done
  # misma orden, usuario y carpeta que el servicio; los datos de la prueba no quedan en la imagen
  chroot "$RAIZ" /usr/bin/env -i HOME="/var/lib/$NOMBRE" LANG=C.UTF-8 PATH=/usr/local/bin:/usr/bin:/bin \
    "${ENV_PRUEBA[@]}" /usr/sbin/runuser -u "$NOMBRE" -- /bin/sh -c "cd '$DIR_TRABAJO' && exec $ORDEN" \
    > "$TRABAJO/prueba.log" 2>&1 &
  PID_PRUEBA=$!
  OK=0
  for _ in $(seq 1 120); do
    if ! kill -0 "$PID_PRUEBA" 2>/dev/null; then break; fi
    if [ -n "$PUERTO" ] && command -v curl >/dev/null && \
       curl -fsS -m 5 -o "$TRABAJO/respuesta.txt" "http://127.0.0.1:$PUERTO/" 2>/dev/null; then OK=1; break; fi
    if [ -z "$PUERTO" ] && [ "$_" -ge 10 ]; then OK=1; break; fi   # sin puerto: basta con que siga vivo 10 s
    sleep 1
  done
  kill "$PID_PRUEBA" 2>/dev/null || true; wait "$PID_PRUEBA" 2>/dev/null || true; PID_PRUEBA=""
  matar_dentro
  if [ "$OK" != 1 ]; then tail -40 "$TRABAJO/prueba.log" >&2; die "El demonio no respondio dentro de la imagen"; fi
  say "Prueba superada: el demonio arranca y responde${PUERTO:+ en el puerto $PUERTO}"
  [ -s "$TRABAJO/respuesta.txt" ] && say "Respuesta: $(head -c 160 "$TRABAJO/respuesta.txt" | tr -s '\n\t ' ' ')"
  rm -rf "$RAIZ$PRUEBA_DATOS"
fi

desmontar
rm -rf "$RAIZ"/tmp/* "$RAIZ"/var/tmp/* "$RAIZ"/root/.cache 2>/dev/null || true

# ------------------------------------------------------------------ capa nueva y ficheros de casper
say "Comprimiendo la capa nueva (xz, tarda)"
mksquashfs "$RAIZ" "$MEDIO/casper/$CAPA" -comp xz -b 1M -noappend -quiet -wildcards -e 'proc/*' 'sys/*' 'dev/*' 'run/*' >/dev/null
TAM="$(du -sx --block-size=1 "$RAIZ" | cut -f1)"
[ -f "$MEDIO/casper/$ID_CAPA.size" ] && echo "$TAM" > "$MEDIO/casper/$ID_CAPA.size"
if [ -f "$MEDIO/casper/$ID_CAPA.manifest" ]; then
  chroot "$RAIZ" dpkg-query -W --showformat='${Package}\t${Version}\n' > "$MEDIO/casper/$ID_CAPA.manifest"
fi
rm -rf "$RAIZ"

if [ -f "$MEDIO/casper/install-sources.yaml" ]; then
  # la instalacion minima pasa a ser la opcion por defecto; los tamanos crecen con lo anadido
  python3 - "$MEDIO/casper/install-sources.yaml" "$ID_CAPA" "$TAM" "$CAPA" <<'PY'
import re, sys
ruta, ident, tam, capa = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
texto = open(ruta, encoding="utf-8").read()
items = re.split(r"(?m)^(?=- )", texto)
cab, items = (items[0], items[1:]) if not items[0].startswith("- ") else ("", items)
def campo(it, k):
    m = re.search(rf"(?m)^[- ] {k}: *(.*)$", it)
    return m.group(1).strip() if m else ""
base = next((it for it in items if campo(it, "id") == ident), None)
delta = 0
if base is not None and campo(base, "size").isdigit():
    delta = tam - int(campo(base, "size"))
out = []
for it in items:
    es = campo(it, "id") == ident
    if re.search(r"(?m)^[- ] default:", it):
        it = re.sub(r"(?m)^([- ] )default: *\w+", lambda m: f"{m.group(1)}default: {'true' if es else 'false'}", it)
    elif es:
        it = "- default: true\n  " + it[2:]
    camino = campo(it, "path")
    if delta and camino.startswith(capa.rsplit(".", 1)[0]) and campo(it, "size").isdigit():
        it = re.sub(r"(?m)^([- ] )size: *\d+", lambda m: f"{m.group(1)}size: {int(campo(it, 'size')) + delta}", it)
    out.append(it)
open(ruta, "w", encoding="utf-8").write(cab + "".join(out))
PY
fi

if [ -f "$MEDIO/casper/SHA256SUMS" ]; then   # sumas de las capas: la de la capa nueva
  SUMA="$(sha256sum "$MEDIO/casper/$CAPA" | cut -d' ' -f1)"
  sed -i -E "s|^[0-9a-f]+ (\*?)$CAPA\$|$SUMA \1$CAPA|" "$MEDIO/casper/SHA256SUMS"
fi
MAPAS=(-map "$MEDIO/casper/$CAPA" "/casper/$CAPA")
for f in "$ID_CAPA.size" "$ID_CAPA.manifest" install-sources.yaml SHA256SUMS; do
  [ -f "$MEDIO/casper/$f" ] && MAPAS+=(-map "$MEDIO/casper/$f" "/casper/$f")
done

if [ "$DESATENDIDA" = 1 ]; then
  say "Instalacion desatendida: autoinstall.yaml con la capa minima"
  cat > "$MEDIO/autoinstall.yaml" <<EOF
autoinstall:
  version: 1
  source:
    id: $ID_CAPA
    search_drivers: false
  identity:
    hostname: ${EQUIPO:-$NOMBRE}
    username: $USUARIO
    password: '$CLAVE_HASH'
  ssh:
    install-server: true
    allow-pw: true
  storage:
    layout:
      name: lvm
  late-commands:
    - curtin in-target -- systemctl enable $NOMBRE.service
EOF
  MAPAS+=(-map "$MEDIO/autoinstall.yaml" /autoinstall.yaml)
fi
if [ -f "$MEDIO/boot/grub/grub.cfg" ] && { [ "$DESATENDIDA" = 1 ] || [ "$SERIE" = 1 ]; }; then
  EXTRA=""
  [ "$DESATENDIDA" = 1 ] && EXTRA="$EXTRA autoinstall"
  [ "$SERIE" = 1 ] && EXTRA="$EXTRA console=tty0 console=ttyS0,115200n8"
  sed -i -E "/^\s*linux\s/ { / autoinstall| console=ttyS0/! s/(\s---|\$)/$EXTRA\1/ }" "$MEDIO/boot/grub/grub.cfg"
  sed -i -E 's/set timeout=[0-9]+/set timeout=5/' "$MEDIO/boot/grub/grub.cfg"
  MAPAS+=(-map "$MEDIO/boot/grub/grub.cfg" /boot/grub/grub.cfg)
fi

if [ -f "$MEDIO/md5sum.txt" ]; then
  for ((i=1; i<${#MAPAS[@]}; i+=3)); do
    local_f="${MAPAS[$i]}"; en_iso="${MAPAS[$((i+1))]}"
    suma="$(md5sum "$local_f" | cut -d' ' -f1)"
    if grep -q "  \.$en_iso\$" "$MEDIO/md5sum.txt"; then
      sed -i "s|^[0-9a-f]*  \.$en_iso\$|$suma  .$en_iso|" "$MEDIO/md5sum.txt"
    else
      echo "$suma  .$en_iso" >> "$MEDIO/md5sum.txt"
    fi
  done
  MAPAS+=(-map "$MEDIO/md5sum.txt" /md5sum.txt)
fi

# ------------------------------------------------------------------ ISO nueva
say "Escribiendo $(basename "$SALIDA") con el arranque de la original (BIOS y UEFI)"
rm -f "$SALIDA"
xorriso -indev "$ISO" -outdev "$SALIDA" "${MAPAS[@]}" -boot_image any replay -padding included >/dev/null 2>"$TRABAJO/xorriso.log" \
  || { tail -20 "$TRABAJO/xorriso.log" >&2; die "xorriso no pudo escribir la ISO"; }
xorriso -indev "$SALIDA" -report_el_torito plain 2>/dev/null | grep -qi "boot" || die "La ISO nueva no tiene arranque"
xorriso -indev "$SALIDA" -find "/casper/$CAPA" 2>/dev/null | grep -q "$CAPA" || die "La ISO nueva no tiene la capa"
(cd "$(dirname "$SALIDA")" && sha256sum "$(basename "$SALIDA")" > "$(basename "$SALIDA").sha256")
say "Lista: $SALIDA ($(du -h "$SALIDA" | cut -f1))"
say "Al instalar elige «Ubuntu Server (minimized)» (ya marcada por defecto): $NOMBRE arranca solo en el primer inicio."
[ "$AGENTE_IA" = 1 ] && say "En el equipo instalado, la orden «$NOMBRE» muestra la direccion y la clave de red."
exit 0
