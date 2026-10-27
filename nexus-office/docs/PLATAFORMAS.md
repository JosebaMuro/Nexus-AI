# Plataformas, instaladores y empaquetado

## Matriz de compatibilidad

Todos los instaladores son **100 % portables**: llevan Python, las dependencias (wheels) y llama-server/sd-cli, y se
instalan sin internet. Solo el modelo de IA se descarga, y solo si hay conexión. Cómo instalar cada uno, paso a paso:
[Guía de usuario → Instalación](USUARIO.md#instalación).

| Plataforma | Instalador | Ejecución local de IA | Notas |
|---|---|---|---|
| Windows 10/11 x64 | `Nexus-Office-Setup-2.2.0-windows-x64.exe` | ✅ CPU, CUDA (NVIDIA), Vulkan (NVIDIA, AMD, Intel), OpenVINO | Instalador gráfico NSIS, accesos directos, desinstalador en *Aplicaciones*, servicio opcional. **Probado en el equipo de desarrollo.** |
| Windows 11 ARM64 (Snapdragon X) | `Nexus-Office-Setup-2.2.0-windows-arm64.exe` | ✅ CPU ARM64 y Vulkan (Adreno); NPU Hexagon vía `onnxruntime-qnn` | llama-server y sd-cli nativos ARM64; Python x64 emulado por Windows 11. El instalador se niega a seguir en un Windows que no sea ARM64. |
| Linux x86_64 | `Nexus-Office-2.2.0-linux-x86_64.run`, `nexus-office_2.2.0_amd64.deb`, Docker | ✅ CPU, CUDA, Vulkan, OpenVINO | Ubuntu, Debian, Fedora, Arch... (glibc 2.28 o posterior). |
| **Steam Deck** (SteamOS) | `Nexus-Office-2.2.0-linux-x86_64.run` | ✅ CPU / Vulkan (APU AMD) | Instala en modo escritorio en tu carpeta de usuario (SteamOS es de solo lectura) y añade `start.sh` como *juego no‑Steam*. |
| Linux ARM64 (Raspberry Pi 4/5, Jetson, Orange Pi) | `Nexus-Office-2.2.0-linux-aarch64.run`, `nexus-office_2.2.0_arm64.deb` | ✅ CPU y Vulkan (Pi 5: Qwen3‑VL 2B Thinking); CUDA en Jetson Orin | Hace falta un sistema de **64 bits** (Raspberry Pi OS 64‑bit, Ubuntu). Los sistemas de 32 bits (`armv7l`) no están soportados. Usa disco SSD/USB3 para los modelos. |
| macOS Apple Silicon / Intel | `Nexus-Office-2.2.0-macos-arm64.tar.gz`, `Nexus-Office-2.2.0-macos-x86_64.tar.gz` | ✅ Metal (Apple Silicon), CPU | Incluye `Nexus-Office.command` y `Nexus-Office.app`. Primera apertura: clic derecho → Abrir (no está firmado). |
| **Android** (app Nexus-Office VPN) | `Nexus-Office-2.2.0-android-vpn.apk` | ⚙️ Cliente del PC | App nativa (Android 7+). Conecta con el Nexus-Office de tu PC (Windows, Linux o macOS) por **VPN WireGuard cifrada** desde cualquier sitio, por HTTPS con certificado anclado o por la Wi‑Fi de la oficina. Webui completa, voz del sistema, adjuntar y descargar ficheros. Ver [VPN.md](VPN.md). |
| **Android** (app Nexus-Office, recomendada) | `Nexus-Office-2.2.0-android-autonomo.apk` | ✅ CPU (3 variantes ARM), GPU (Adreno por OpenCL; Mali, Immortalis, Xclipse y PowerVR por Vulkan) y NPU Hexagon de los Snapdragon 8 Gen 2 o posteriores (llama.cpp Hexagon, experimental). Otras NPU (APU, TPU, Exynos) detectadas pero sin uso | Nexus-Office 100 % en el móvil, sin PC (Android 9+, ARM64). La APK lleva Python 3.12, Nexus-Office, sus dependencias y llama.cpp (CPU y GPU); detecta CPU, GPU y NPU, mide si el modelo va más rápido en GPU o en CPU y solo descarga el modelo de IA adecuado a la RAM del móvil (Qwen3‑VL 2B o 4B Thinking). Arranque al encender opcional. También se conecta al Nexus-Office del PC (*RED → Conectar a mi ordenador*). Ver [android-autonomo/README.md](../android-autonomo/README.md). |
| **Android ARM64** (Termux) | Desde el código fuente o desde la app Nexus-Office VPN | ✅ CPU vía Termux (`pkg llama-cpp`) | Ya no se entrega un paquete para Termux, pero `install-termux.sh` sigue en el código fuente, y la app Nexus-Office VPN da el comando que descarga Nexus-Office desde tu PC (`/api/package/termux`). Instala Termux desde F‑Droid. Para la mayoría de móviles es más sencilla la app Nexus-Office autónomo. |
| iPhone / iPad | PWA | ⚠️ Cliente | iOS no permite ejecutar este tipo de servidor; úsalo como app web conectada a tu PC o a tu red cooperativa. |
| **Xbox Series** | PWA (Microsoft Edge) | ⚠️ Cliente | Las consolas son plataformas cerradas: no permiten instalar Python ni runtimes. Abre la UI de tu PC desde Edge (puede instalarse como app). |
| **PlayStation 5** | Navegador | ⚠️ Cliente | Sin instalación posible; acceso por navegador a un Nexus-Office de la red. |
| **Nintendo Switch** | — | ❌ | No dispone de un navegador general utilizable. |
| Servidores / NAS / nube | `Dockerfile` (amd64/arm64) | ✅ | `docker run -p 8775:8775 -v nexus-data:/app/data nexus-office` |

> **Por qué las consolas son "cliente"**: PlayStation, Xbox y Switch solo ejecutan software firmado por el fabricante. Nexus-Office sí funciona en ellas como interfaz (PWA) mientras la IA corre en un PC, Steam Deck, servidor o en tu red cooperativa — que además permite que varios equipos compartan el modelo más potente.

## Qué hace cada instalador

| Instalador | Cómo instala | Opciones |
|---|---|---|
| `.exe` (Windows, NSIS) | Copia la carpeta portable a `%LOCALAPPDATA%\Nexus-Office` (o la elegida) sin pedir administrador y ejecuta `install.ps1 -Offline -Clean -Modelos no` (más `-Oficina`). Después, `python -m nexus modelos --descargar` y, si se pidió, `python -m nexus servicio instalar [--red]` con UAC solo para ese paso. Accesos directos con `pythonw.exe -m nexus run` (sin consola) y *Nexus-Office (consola)* con `start.bat`. | Página de opciones: perfil de oficina, descargar el modelo, servicio al encender, acceso desde la red local, acceso directo en el escritorio. Silencioso: `/S /OFICINA=<perfil> /SERVICIO /RED /MODELOS=auto\|todos\|no /D=<carpeta>` (`/D` siempre al final). |
| `.run` (Linux) | Script `sh` con el `.tar.gz` incrustado. Lo extrae en la carpeta indicada (por defecto `~/.local/share/nexus-office`) y ejecuta `install.sh` con el resto de opciones. | `sh Nexus-Office-2.2.0-linux-<arch>.run [carpeta] [--oficina=<perfil>] [--servicio [--red]] [--modelos=auto\|todos\|no]` |
| `.deb` (Debian, Ubuntu, Raspberry Pi OS 64 bits) | Programa en `/opt/nexus-office`; `postinst` prepara `venv/` sin internet (`install.sh --offline --no-clean --modelos=no`). Lanzador `/usr/bin/nexus-office` y entrada de menú. Los datos de cada usuario van a `~/.local/share/nexus-office` (`NEXUS_DATA_DIR`). La primera ejecución de `nexus-office` evalúa el hardware y descarga el modelo. `prerm` quita el servicio. | `nexus-office [servicio\|modelos\|oficina\|detect\|pull\|chat] …`; servicio con `sudo nexus-office servicio instalar [--red]`. |
| `.tar.gz` (macOS) | Carpeta portable. `Nexus-Office.command` ejecuta `install.sh` la primera vez (quita la cuarentena de Gatekeeper a los binarios incluidos) y después `start.sh`. | `./install.sh --oficina=<perfil> --servicio [--red] --modelos=…` |

Con el **instalador gráfico de Windows** y con `install.sh`/`install.ps1`, el último paso es `python -m nexus modelos`:
evalúa el hardware, elige el modelo de cada tipo y lo descarga si hay internet (ver
[Guía de usuario](USUARIO.md#modelo-de-ia-elegido-según-tu-equipo)). Sin internet solo muestra la recomendación.

### Opciones de los scripts de instalación

| `install.sh` (Linux, macOS) | `install.ps1` / `install.bat` (Windows) | Qué hace |
|---|---|---|
| `--oficina=<perfil>` | `-Oficina <perfil>` | Perfil de oficina (también `NEXUS_OFICINA`). Ver [OFICINA.md](OFICINA.md). |
| `--modelos=auto\|todos\|no` | `-Modelos auto\|todos\|no` | `auto` (por defecto): modelo de chat y visión, de documentos (embeddings) y de voz; `todos`: también el de imagen; `no`: solo muestra la recomendación. También `NEXUS_MODELOS`. |
| `--servicio` | `-Servicio` | Instala el arranque automático (también `NEXUS_SERVICIO=1`). Ver [SERVICIO.md](SERVICIO.md). |
| `--red` | `-Red` | Con el anterior: el servicio escucha en `0.0.0.0`. |
| `--offline` | `-Offline` | No usa internet: todo sale de `runtime/` y `wheelhouse/`. |
| `--fetch-wheels` | `-FetchWheels` | Descarga antes `wheelhouse/` y el Python a `runtime/`. |
| `--clean` / `--no-clean` | `-Clean` / `-NoClean` | Limpieza final sin preguntar / sin limpieza. |
| `--skip-extras`, `--force` | `-SkipExtras`, `-Force` | Sin extras por hardware / reinstala el runtime. |

`install-termux.sh` admite `--oficina=<perfil>`, `--modelos=auto|no` y `--servicio` (en Termux el servicio escucha
siempre en la red local).

## Instalar desde el código fuente (sin compilar)

Desde `Nexus-Office-2.2.0-codigo-fuente.zip`, `install.sh` (Linux, WSL, macOS) e `install.bat` / `install.ps1` (Windows)
descargan el Python portable y las dependencias. Si la carpeta tiene `runtime/` y `wheelhouse/` (como los
instaladores), no usan internet. Wheels comprobadas para CPython 3.11 (`nexus wheels --check`, simulando la
instalación sin red):

| Plataforma | Base (`requirements.txt`) | Extras sin wheel (no se instalan; Nexus-Office usa la alternativa) | wheelhouse |
|---|---|---|---|
| Linux x86_64 (glibc 2.28+) | ✅ | — | ~240 MB |
| Linux ARM64 (Raspberry Pi, Jetson) | ✅ | — | ~210 MB |
| Windows x64 (también el instalador ARM64, con Python x64 emulado) | ✅ | — | ~215 MB |
| Windows ARM64 nativo (solo instalando desde el código) | ✅ con `vendor/wheels/` (PyYAML puro) | piper-tts, faster-whisper, openvino | ~40 MB |
| macOS Apple Silicon | ✅ | — | ~135 MB |
| macOS Intel | ✅ | piper-tts, faster-whisper, onnxruntime | ~90 MB |
| Android (Termux) | sale de `pkg` (ya compilado) y pip solo pone Python puro | — | — |

PyYAML no publica wheel para Windows ARM64. `vendor/wheels/` trae la misma versión en puro Python, y pip solo la usa donde no hay binaria. Sin voz neuronal, Nexus-Office usa la voz del sistema (pyttsx3) o la del navegador. Termux no puede usar wheels de PyPI, y si a tu repositorio le falta algún paquete, pip lo compila. Sin compilar nada en el móvil está la APK **Nexus-Office autónomo**, que ya lleva Python y sus dependencias.

`install.sh` instala siempre **wheels binarias** para el Python portable 3.11 (`--only-binary=:all:`), así que nunca compila: no necesita `gcc`, `build-essential` ni `python3-dev`. Si falta la wheel de algún paquete, se para con un mensaje claro en vez de intentar compilarlo (`NEXUS_ALLOW_BUILD=1` lo permite si de verdad hace falta). Solo admite `x86_64` y `aarch64`: en una Raspberry Pi con sistema de 32 bits se para con «Arquitectura no soportada».

La carpeta `wheelhouse/` guarda las wheels para instalar sin internet: las de `requirements.txt` y las de los extras de voz y NPU (piper-tts, faster-whisper, pyttsx3, onnxruntime y openvino). Lleva las variantes para glibc 2.39 y 2.28, así que vale desde Ubuntu 20.04 o Debian 10 en adelante. Con `wheelhouse/`, `install.sh` y `nexus setup` (también desde *Sistema*) instalan primero desde ella sin red. Si le falta algo y hay internet, lo completan desde PyPI.

```bash
bash install.sh --fetch-wheels      # con internet: baja wheelhouse/ (~240 MB) y el Python a runtime/, e instala desde ellos
bash install.sh --offline           # sin internet: todo sale de runtime/ y wheelhouse/; si falta algo, lo dice
venv/bin/python3 -m nexus wheels --target linux-aarch64   # wheels para otra plataforma (linux, windows-x64, macos-arm64, all...)
venv/bin/python3 -m nexus wheels --check                  # comprueba que wheelhouse/ instala todo sin internet
```
`wheelhouse/SHA256SUMS` permite detectar wheels dañadas al copiarlas. Los binarios de llama.cpp y stable-diffusion.cpp no son wheels: los instaladores los traen ya compilados en `bundled/runtimes/` (llama-server y sd-cli), también para GPU, y `nexus setup` no los descarga.

**WSL**: instala Nexus-Office en tu carpeta de Linux (`~/Nexus-Office`), no en `/mnt/c/...`, que es mucho más lenta. Si la carpeta ya tiene el Nexus-Office de Windows (`venv\python.exe`), `install.sh` no lo borra: te pide copiarla a `~/`. Si descomprimes el zip desde Windows y se pierden los permisos, usa `bash install.sh`.

### Aceleración incluida en los instaladores
Cada instalador lleva todo lo necesario para acelerar con la GPU o la NPU de su plataforma, sin internet. Nexus-Office elige al arrancar: prueba primero la variante de GPU adecuada a tu hardware y, si el equipo no tiene el driver (por ejemplo, `libcuda` o `vulkan-1.dll`), pasa a la siguiente y, al final, a la CPU.

| Instalador | llama-server (chat, visión, embeddings) | sd-cli (imagen) | Wheels de aceleración |
|---|---|---|---|
| Windows x64 | CUDA 12.9 (NVIDIA), Vulkan (NVIDIA, AMD, Intel), CPU | Vulkan, CPU | OpenVINO (Intel CPU/GPU/NPU), onnxruntime-qnn (NPU Qualcomm), PyTorch con DirectML (cualquier GPU) |
| Windows ARM64 | Vulkan (Adreno y otras), CPU | Vulkan, CPU | Las de Windows x64 (Python x64 emulado) |
| Linux x86_64 | CUDA 12.9 (NVIDIA), Vulkan (NVIDIA, AMD, Intel), CPU | Vulkan, CPU | OpenVINO (Intel CPU/GPU/NPU) |
| Linux ARM64 | CUDA (NVIDIA Jetson Orin, JetPack 6), Vulkan (Mali, Adreno, Raspberry Pi 5...), CPU | Vulkan, CPU | OpenVINO |
| macOS Apple Silicon | Metal (GPU de Apple), CPU | CPU | PyTorch con Metal (MPS), onnxruntime con Core ML |
| macOS Intel | CPU | CPU | — |

- **Vulkan** acelera las GPU de cualquier fabricante con su driver normal. Lo compila `packaging/build_desktop_runtimes.sh` con zig desde las fuentes de PyPI; enlaza con el cargador de Vulkan del sistema (`libvulkan.so.1` en Linux, `vulkan-1.dll` en Windows), que instala cualquier driver de GPU.
- **CUDA** y **Metal** salen de los paquetes de llama.cpp de conda-forge (`packaging/build_gpu_runtimes.py`), con sus bibliotecas (cuBLAS en CUDA). Con NVIDIA hace falta el driver 525 o posterior.
- **No incluidos**: ROCm (AMD) y SYCL (Intel), que pesan varios GB; esas GPU van por Vulkan, y con internet se puede instalar la variante oficial desde *Sistema → Runtimes*. PyTorch con CUDA o ROCm (2-4 GB) tampoco: la música neuronal en esas GPU se instala con `nexus setup --all` y conexión. Sin PyTorch, Nexus-Office compone la música con el compositor procedural.
- **Selector de inicio (2.0)**: cada opción del selector de motor y CPU/GPU/NPU lleva su runtime dentro del paquete: llama-server en sus variantes, el motor integrado (llama-cpp-python) y llama-server con OpenVINO para la NPU Intel en Windows x64 y Linux x86_64. Ver [ACELERADOR.md](ACELERADOR.md).
- `nexus setup` instala solo los extras que usa tu hardware. PyTorch se instala sin preguntar si sus wheels vienen en el instalador y el equipo tiene la GPU adecuada.

### Limpieza al final de la instalación
Al terminar, `install.sh` / `install.bat` preguntan si quieres quitar lo que tu equipo no usa: las wheels que no se han instalado y las variantes de runtime de otras GPU. Después preguntan si guardas lo que sí se ha usado (las wheels instaladas y el Python de `runtime/`) para reinstalar sin internet con `--force --offline`. El runtime que usa tu equipo y su variante CPU de respaldo nunca se borran. El instalador de Windows limpia sin preguntar (`-Clean`) y el `.deb` no limpia.

```bash
./install.sh --clean          # sin preguntar: borra lo no usado y guarda lo usado (Windows: install.bat -Clean)
./install.sh --no-clean       # no limpia (Windows: install.bat -NoClean)
venv/bin/python3 -m nexus cleanup --dry-run   # en cualquier momento: muestra lo que se borraría
venv/bin/python3 -m nexus cleanup --no-keep   # borra también wheelhouse/ y runtime/ (Nexus-Office sigue funcionando)
```
Sin terminal (instalación desatendida), no se limpia nada salvo con `--clean` o `NEXUS_CLEAN=yes`. Tras limpiar, la carpeta ya no sirve para instalar en otro equipo con otro hardware: para eso, guarda el instalador original.

### AppImage y Flatpak

`packaging/build_linux_bundles.py` (lo llama `build.py` para cada Linux) genera `Nexus-Office-<ver>-linux-<arch>.AppImage`
y `.flatpak` para x86_64 y aarch64. Los dos son de solo lectura, así que Nexus-Office va **ya instalado** dentro: Python con
las dependencias base en su `site-packages` (instaladas con el pip del equipo de compilación y `--platform`, lo que
permite construir el de ARM64 desde x86_64), más `bundled/runtimes`. Los datos van a `~/.local/share/nexus-office`
(AppImage) o `~/.var/app/ai.nexus.NexusOffice` (Flatpak). Sin argumentos arrancan Nexus-Office; con argumentos son
`python -m nexus …` (`servicio`, `modelos`, `oficina`, `chat`).

- **AppImage**: `appimagetool` y el `runtime-<arch>` de AppImage/type2-runtime (se descargan a `.cache/appimage/`).
  El servicio apunta al fichero `.AppImage` (variable `APPIMAGE`), no a la carpeta temporal donde se monta.
- **Flatpak**: se arma sin `flatpak-builder` (`flatpak build-export` + `build-bundle --runtime-repo` de Flathub):
  al instalarlo, Flatpak descarga `org.freedesktop.Platform 24.08`. Permisos: red, X11/Wayland, audio, todos los
  dispositivos (GPU) y `xdg-config/systemd` para dejar la unidad del servicio, que se activa desde fuera del sandbox
  con `systemctl --user enable --now nexus-office`.
- Los extras que se instalan con pip al vuelo (Sistema → extras) no están disponibles en estos dos formatos, porque la
  carpeta es de solo lectura: usa el `.run` o el `.deb` si los necesitas.

## Servicio y descargas para la oficina

- **Servicio**: `python -m nexus servicio instalar|quitar|estado [--red] [--puerto P]` (`nexus/service.py`). Usa
  systemd (del sistema como root, de usuario con `loginctl enable-linger` si no), cron `@reboot` sin systemd, launchd
  (LaunchDaemon o LaunchAgent), el Programador de tareas de Windows (al iniciar el equipo como SYSTEM si es
  administrador, al iniciar sesión si no; reinicio ante fallos, sin límite de tiempo y regla de firewall con `--red`)
  o termux-services con Termux:Boot. La app Nexus-Office autónomo arranca su servicio en primer plano al encender
  (`BootReceiver`). Todo en [SERVICIO.md](SERVICIO.md).
- **Descargas**: `nexus/downloads.py` lista los ficheros de `<datos>/descargas/` (por ejemplo,
  `data/descargas/windows/Nexus-Office-Setup-2.2.0-windows-x64.exe`) y los clasifica por plataforma según su nombre. Para
  repartir los instaladores en la oficina, copia la carpeta de una entrega (`windows/`, `linux/`, `macos/`,
  `android/`…) ahí; nada la llena automáticamente. *Sistema → Descargas* marca el del sistema anfitrión y el del
  dispositivo que mira la página. Se sirven en `/descargas/<ruta>` con la misma protección de clave de red que el resto.

## Empaquetado

Todo se construye en Linux (también WSL); el `.exe` de Windows sale de `makensis`, que funciona en Linux.

**1. Prepara los runtimes en `.cache/`** (una vez, o al cambiar de versión):

```bash
python3 packaging/build_python_runtimes.py      # CPython 3.11 portable de conda-forge (Linux, Windows x64, macOS)
bash packaging/build_desktop_runtimes.sh        # llama-server y sd-cli (CPU y Vulkan) para todas las combinaciones, con zig
#   (requisitos: sudo apt install cmake ninja-build python3 curl g++ glslc libvulkan-dev spirv-headers)
python3 packaging/build_gpu_runtimes.py         # CUDA (Windows x64, Linux x64, Jetson) y Metal (Apple Silicon) de conda-forge
#   (requisitos: pip install macholib pyelftools pefile)
python3 -m nexus wheels --target all --dest .cache/wheelhouse-all   # opcional: wheels de todas las plataformas
```

Todo queda en `.cache/desktop-runtimes/<plataforma>/` (`cpython-*.tar.gz`, `llama/<variante>/`, `sd/<variante>/`).
Si falta el Python de `.cache/`, `build.py` lo baja de python-build-standalone (GitHub); si falta
`.cache/wheelhouse-all`, descarga las wheels de cada plataforma. Windows ARM64 lleva el Python x64 (conda-forge no
tiene CPython 3.11 para ARM64 y Windows 11 ARM lo emula) con sus wheels, y llama-server y sd-cli nativos ARM64.

**2. Compila las APK** (opcional; ver [Apps Android](#apps-android-versión-160)). Las dos escriben en `dist/`, que
`build.py` vacía al empezar: muévelas antes a otra carpeta, por ejemplo `dist-apk/`.

**3. Genera los instaladores**:

```bash
sudo apt install nsis                                     # makensis, para el .exe (o brew install makensis / choco install nsis)
python3 packaging/build.py                                # todas las plataformas
python3 packaging/build.py --targets windows-x64 linux-aarch64
python3 packaging/build.py --apk dist-apk/*.apk --docs Manual-de-usuario.pdf Documentacion-tecnica.pdf
python3 packaging/build.py --keep --targets macos-arm64   # añade una plataforma sin borrar dist/
```

| Opción | Qué hace |
|---|---|
| `--targets` | `windows-x64`, `windows-arm64`, `linux-x86_64`, `linux-aarch64`, `macos-arm64`, `macos-x86_64` (por defecto, todas). |
| `--apk` | APK ya compiladas, que se copian a `dist/android/`. |
| `--docs` | Ficheros extra para la raíz de `dist/` (los manuales en PDF, por ejemplo). |
| `--keep` | No borra `dist/` al empezar. |

Sin `makensis`, `build.py` sigue con las demás plataformas y avisa de que falta el `.exe`.

Resultado:

```
dist/
  windows/  Nexus-Office-Setup-2.2.0-windows-x64.exe, Nexus-Office-Setup-2.2.0-windows-arm64.exe
  linux/    Nexus-Office-2.2.0-linux-x86_64.run, Nexus-Office-2.2.0-linux-aarch64.run,
            nexus-office_2.2.0_amd64.deb, nexus-office_2.2.0_arm64.deb
  macos/    Nexus-Office-2.2.0-macos-arm64.tar.gz, Nexus-Office-2.2.0-macos-x86_64.tar.gz
  android/  Nexus-Office-2.2.0-android-autonomo.apk, Nexus-Office-2.2.0-android-vpn.apk   (de --apk)
  Nexus-Office-2.2.0-codigo-fuente.zip, LEEME.txt, SHA256SUMS.txt (+ los ficheros de --docs)
```

Cada instalador lleva la carpeta portable completa: código, `runtime/` (CPython de la plataforma), `wheelhouse/`
(dependencias y extras de voz, NPU y GPU, solo binarias) y `bundled/runtimes/` (llama-server y sd-cli para CPU y GPU).
Ya no se generan los paquetes *online* que descargaban Python y las dependencias al instalar, ni los `.zip` por
plataforma, ni el zip maestro, ni el paquete de Termux.

`Nexus-Office-2.2.0-codigo-fuente.zip` contiene solo código, tests, documentación, empaquetado y las apps Android, sin
`venv/`, datos, modelos, cachés ni compilados; `build.py` se para si encuentra ficheros sensibles (claves, `.env`,
`config.yaml`, registros). Las versiones de referencia de Python portable, llama.cpp, stable-diffusion.cpp y los
modelos por perfil están en `requirements.txt`; se regenera con `packaging/requirements_ref.py`.

Los modelos no se incluyen en los instaladores (son de varios GB y tienen sus propias licencias): los descarga el
propio instalador si hay internet, la aplicación desde *Modelos → Recomendados* o `nexus modelos --descargar`.

Para enviar los instaladores a un móvil o a otro canal con límite de tamaño (unos 30 MB por fichero), usa `venv/python.exe packaging/split_parts.py [--size 25] [ficheros]`. Deja el resultado en `dist/envio/`:

- Los ficheros pequeños se copian enteros.
- Los grandes se dividen en partes `.001`, `.002`… e incluyen `UNIR-<nombre>.bat` (Windows) y `unir-<nombre>.sh` (Linux, macOS o Termux) para reconstruirlos. Estos scripts comprueban la huella SHA-256 del fichero unido.
- `LEEME-DESCARGAS.txt` resume los ficheros con sus tamaños y huellas.

En la red de la oficina es más cómodo copiar `dist/` en `data/descargas/` del PC con Nexus-Office y descargarlos desde
*Sistema → Descargas*.

## Apps Android (versión 2.2.0)

Desde la 2.2 la app **autónoma** es la única que hace falta: lleva la IA en el móvil y, en *RED → Conectar a mi
ordenador*, el mismo cliente de la app VPN (red local, HTTPS anclado y VPN WireGuard). La app VPN se sigue
publicando para móviles que no pueden con la IA local. Las dos pueden instalarse a la vez:

| | Nexus-Office VPN | Nexus-Office autónomo |
|---|---|---|
| Fichero | `Nexus-Office-2.2.0-android-vpn.apk` | `Nexus-Office-2.2.0-android-autonomo.apk` |
| Paquete | `ai.nexus.office.client` (sustituye a la app 1.3) | `ai.nexus.office.autonomo` |
| Android | 7.0 o superior (API 24) | 9.0 o superior (API 28), ARM64 |
| Dónde corre la IA | En tu PC con Windows, Linux o macOS | En el propio móvil (modelo elegido según su RAM), en la GPU si llama.cpp puede usarla y va más rápida, si no en la CPU |
| Conexión | VPN WireGuard cifrada, HTTPS anclado o Wi‑Fi | Su IA en `127.0.0.1` y, si quieres, tu PC por VPN WireGuard, HTTPS anclado o Wi‑Fi |
| Arranque al encender | — | Opcional: *Sistema → Arranque automático* → **ARRANCAR AL ENCENDER** |
| Compilar | `venv/python packaging/build_apk.py` | `bash packaging/build_android_runtime.sh` y `venv/python packaging/build_apk_autonomo.py` |

La versión de las dos APK sale de `nexus/__init__.py` más `APK_REVISION` (`packaging/build_apk.py`): la 2.2.0 revisión 0 da versionCode 10700. Desde la 2.2.0 los identificadores son `ai.nexus.office.client` y `ai.nexus.office.autonomo`, así que Nexus-Office no se instala encima de NexusAI: conviven.

**Actualizar desde la app 1.3**: la app Nexus-Office VPN 1.4 está firmada con una clave nueva. Desinstala antes la app Nexus-Office anterior, porque Android no deja instalar encima una app con otra firma. Después vuelve a conectar con el código de tu PC.

### Compilar la app Nexus-Office VPN

Sirve en Linux, WSL o macOS.

1. Con el SDK de Android: copia `build-tools/34.0.0` a `.cache/android/build-tools` y `platforms/android-34/android.jar` a `.cache/android/platform/`. Necesitas también un JDK 17.
2. En Ubuntu o WSL, sin SDK:
   ```bash
   sudo apt install aapt zipalign apksigner dalvik-exchange android-sdk-platform-23 openjdk-17-jdk-headless
   ```
   Además necesitas el `android.jar` de la API 34 en `.cache/android/platform/` o en la variable `NEXUS_ANDROID_JAR`. Sirve el `android-all` de Robolectric de Maven Central.

Después:

```bash
venv/bin/python packaging/build_apk.py      # deja la APK en dist/Nexus-Office-<versión>-android-vpn.apk
```

`libwg-go.so` (WireGuard oficial, Apache 2.0) se descarga de Maven Central y se comprueba su huella SHA‑256.

Para probar el túnel sin móvil, `sudo bash packaging/test_vpn_e2e.sh` monta dos espacios de red Linux:

- en uno, el PC levanta el túnel con la conf de Nexus-Office;
- en el otro, WireGuard se configura con el texto que genera la app.

La prueba comprueba que Nexus-Office responde por la VPN.

### Compilar la app Nexus-Office autónomo

```bash
ANDROID_NDK_HOME=~/android-ndk bash packaging/build_android_runtime.sh   # Python y llama.cpp de CPU con zig; con NDK, también el de GPU
venv/bin/python packaging/build_apk_autonomo.py    # deja la APK en dist/Nexus-Office-<versión>-android-autonomo.apk (sin Gradle)
```

Detalles en [android-autonomo/README.md](../android-autonomo/README.md).

## Portabilidad
`venv/` contiene un **CPython completo** (python-build-standalone o conda-forge), no un entorno virtual que dependa del Python del sistema. Copiar la carpeta a otro equipo con el mismo sistema y arquitectura conserva modelos, memoria y configuración; Nexus-Office re-detecta el hardware al arrancar y adapta el perfil. Si tenías el servicio instalado, vuelve a instalarlo en la carpeta nueva.

## Acceso desde otros dispositivos (móvil, tablet, consola, TV)
En *Red COOP → Conectar dispositivos* (modo admin) activa **Permitir acceso desde la red** y reinicia Nexus-Office (o instala el servicio con `--red`). Los dispositivos que no sean el propio PC necesitan la **clave de red** (se muestra ahí; la app Android y la URL con `?key=` la recuerdan). `/api/ping` es lo único público, para que la app encuentre el servidor.
