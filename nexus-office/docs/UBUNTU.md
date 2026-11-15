# Ubuntu mínima con un proyecto Python preinstalado

`packaging/ubuntu/rehacer-ubuntu.sh` (desde la 4.2; también suelto en cada Release) rehace la ISO oficial de
**Ubuntu Server** para que la **instalación mínima** («Ubuntu Server (minimized)») lleve un proyecto Python
preinstalado como **demonio**: un servicio systemd activado y preconfigurado que arranca solo desde el primer inicio.

Por defecto integra **Agente IA** desde su paquete `.deb` (100 % offline: Python, dependencias y motores dentro). Sirve
igual para cualquier otro proyecto Python.

## Qué hace

1. Lee la ISO y saca solo la **capa mínima** (`casper/ubuntu-server-minimal.squashfs` en 22.04, 24.04 y 26.04).
2. La descomprime y, con `chroot`, instala el proyecto:
   - **Agente IA** (`--deb nexus-office_<versión>_amd64.deb`): instala el paquete (que prepara su Python y sus
     dependencias sin internet) y las bibliotecas que no trae la mínima (`libgomp1`, `libvulkan1`, `ca-certificates`).
     El servicio arranca `python -m nexus run` en el puerto 8775, escuchando en la red y protegido con su **clave de
     red** (`--solo-local` lo deja en 127.0.0.1). Al entrar por consola o SSH se ve la dirección con la clave, y la
     orden `agente-ia` la vuelve a mostrar.
   - **Otro proyecto** (`--proyecto carpeta --orden "python -m paquete"`): lo copia a `/opt/<nombre>/app`, crea su
     entorno virtual en `/opt/<nombre>/venv` e instala sus dependencias (`requirements.txt`, `pyproject.toml` o
     `setup.py`). Con `--sin-venv` usa el `python3` de Ubuntu.
3. Crea el usuario del sistema `<nombre>` (con acceso a la GPU y la NPU: grupos `video` y `render`), su carpeta de
   datos `/var/lib/<nombre>`, sus variables en `/etc/<nombre>/<nombre>.env` y el servicio
   `/etc/systemd/system/<nombre>.service`, **activado**. El servicio no arranca en el instalador en vivo de la ISO
   (`ConditionKernelCommandLine=!boot=casper`), solo en el sistema instalado.
4. Con `--probar` arranca el demonio dentro de la imagen con el mismo usuario, orden y carpeta que el servicio, pide su
   página y lo para (los datos de la prueba no quedan en la imagen).
5. Vuelve a comprimir la capa (xz), actualiza su tamaño, su manifiesto de paquetes, `install-sources.yaml` (la mínima
   queda **marcada por defecto**), las sumas `casper/SHA256SUMS` y `md5sum.txt`, y escribe la ISO nueva copiando el
   **arranque BIOS y UEFI** de la original (`xorriso -boot_image any replay`).

Lo que se detectó del equipo donde se construye (hardware, configuración) no se guarda en la imagen: Agente IA lo
detecta en el primer arranque del equipo final.

## Uso

```bash
sudo apt install xorriso squashfs-tools
# Agente IA (el .deb está en cada Release)
sudo packaging/ubuntu/rehacer-ubuntu.sh --descargar 24.04 --deb nexus-office_4.2.0_amd64.deb --probar
# con una ISO ya descargada e instalación desatendida
sudo packaging/ubuntu/rehacer-ubuntu.sh --iso ubuntu-24.04.4-live-server-amd64.iso --deb nexus-office_4.2.0_amd64.deb \
     --desatendida --usuario joseba --clave-hash "$(openssl passwd -6)" --salida agente-ia-ubuntu.iso
# cualquier proyecto Python
sudo packaging/ubuntu/rehacer-ubuntu.sh --iso ubuntu.iso --proyecto ~/mi-api --orden "python -m mi_api" \
     --nombre mi-api --entorno PUERTO=8000 --puerto 8000 --probar
```

| Opción | Para qué |
|---|---|
| `--iso FICHERO` / `--descargar VERSIÓN` | ISO original, o descargarla de releases.ubuntu.com comprobando su SHA256 (`ARQ_ISO=arm64` para ARM) |
| `--salida FICHERO` | ISO nueva (por defecto `<iso>-<nombre>.iso`) y su `.sha256` |
| `--deb` / `--proyecto` | El proyecto: paquete `.deb` o carpeta |
| `--orden "ORDEN"` | Orden del servicio (`python ...` usa el Python del venv); con el `.deb` de Agente IA no hace falta |
| `--nombre`, `--descripcion` | Servicio, usuario y carpetas (por defecto `agente-ia`) |
| `--entorno CLAVE=VALOR` | Variables del servicio (se puede repetir) |
| `--paquetes "a b"` | Paquetes de Ubuntu que se instalan dentro (necesita red al construir) |
| `--puerto N`, `--solo-local` | Puerto que comprueba `--probar`; Agente IA solo en 127.0.0.1 |
| `--capa NOMBRE` | Otra capa de `casper/` (por defecto la mínima) |
| `--desatendida --usuario U --clave-hash H [--equipo N]` | Instalación automática (`autoinstall.yaml` en la raíz de la ISO, con la capa mínima, LVM y SSH) |
| `--serie` | El instalador también escribe en el puerto serie (servidores sin pantalla, máquinas virtuales) |
| `--probar` | Arranca el demonio dentro de la imagen y comprueba que responde |
| `--sin-venv`, `--trabajo`, `--conservar` | Proyecto sin dependencias; carpeta de trabajo; no borrarla al terminar |

Necesita **root** (usa `chroot`), unos 20 GB libres para Agente IA y, para una ISO de otra arquitectura (arm64 en un
PC x86_64), `qemu-user-static`. Si el equipo usa un proxy, `apt` y `pip` lo usan también dentro de la imagen.

## Después de instalar

```bash
systemctl status agente-ia          # activo desde el primer arranque
agente-ia                           # estado, dirección y clave de red
agente-ia modelos --descargar       # orden de nexus como el usuario del servicio (aquí: bajar el modelo)
journalctl -u agente-ia -f          # registro
sudo nano /etc/agente-ia/agente-ia.env && sudo systemctl restart agente-ia
```

## Pruebas

Probado con Ubuntu Server 24.04.4 (amd64): la ISO nueva conserva el arranque BIOS y UEFI, la capa mínima queda por
defecto con el servicio activado, y el demonio arranca y responde dentro de la imagen. En GitHub Actions
(`entrega.yml`, trabajo `ubuntu`) se rehace con el `.deb` de Agente IA de esa ejecución, se **instala sola en una
máquina virtual** (QEMU con KVM, instalación desatendida) y se comprueba que Agente IA responde al arrancar el sistema
instalado. Falta probarla en un equipo físico.

La ISO resultante pasa de 2 GB, el límite de GitHub para un fichero de una Release: por eso se publica el script y
no la ISO.
