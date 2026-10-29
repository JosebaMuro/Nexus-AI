# Plataformas, instaladores y empaquetado

## Matriz de compatibilidad

Todos los instaladores son **100 % portables**: llevan el programa nativo (`nexus-office`, un único ejecutable con la
interfaz web), llama-server y sd-cli en sus variantes de CPU y GPU, Piper y Whisper, y se instalan sin internet. Solo
el modelo de IA se descarga, y solo si hay conexión. No llevan ni necesitan Python. Cómo instalar cada uno, paso a
paso: [Guía de usuario → Instalación](USUARIO.md#instalación).

| Plataforma | Instalador | Ejecución local de IA | Notas |
|---|---|---|---|
| Windows 10/11 x64 | `Nexus-Office-Setup-<ver>-windows-x64.exe` | ✅ CPU, CUDA (NVIDIA), Vulkan (NVIDIA, AMD, Intel), OpenVINO (NPU Intel) | Instalador gráfico NSIS, accesos directos, desinstalador en *Aplicaciones*, servicio opcional. |
| Windows 11 ARM64 (Snapdragon X) | `Nexus-Office-Setup-<ver>-windows-arm64.exe` | ✅ CPU ARM64 y Vulkan (Adreno) | Todo nativo ARM64: nexus-office, llama-server y sd-cli. |
| Linux x86_64 | `Nexus-Office-<ver>-linux-x86_64.run`, `.AppImage`, `.flatpak`, `nexus-office-c_<ver>_amd64.deb` | ✅ CPU, CUDA, Vulkan, OpenVINO | Ubuntu 22.04, Debian 12, Fedora, Arch... (glibc 2.35 o posterior). |
| **Steam Deck** (SteamOS) | `Nexus-Office-<ver>-linux-x86_64.run` o `.AppImage` | ✅ CPU / Vulkan (APU AMD) | Instala en modo escritorio en tu carpeta de usuario (SteamOS es de solo lectura) y añade `start.sh` como *juego no‑Steam*. |
| Linux ARM64 (Raspberry Pi 4/5, Jetson, Orange Pi) | `Nexus-Office-<ver>-linux-aarch64.run`, `.AppImage`, `.flatpak`, `nexus-office-c_<ver>_arm64.deb` | ✅ CPU y Vulkan (Pi 5: Qwen3‑VL 2B Thinking); CUDA en Jetson Orin | Hace falta un sistema de **64 bits** (Raspberry Pi OS 64‑bit, Ubuntu). Los de 32 bits (`armv7l`) no están soportados. Usa disco SSD/USB3 para los modelos. |
| macOS Apple Silicon / Intel | `Nexus-Office-<ver>-macos-arm64.tar.gz`, `Nexus-Office-<ver>-macos-x86_64.tar.gz` | ✅ Metal (Apple Silicon), CPU | Incluye `Nexus-Office.command` y `Nexus-Office.app`. Primera apertura: clic derecho → Abrir (no está firmado). |
| **Android** (app Nexus-Office, recomendada) | `Nexus-Office-<ver>-android-autonomo.apk` | ✅ CPU, GPU (Adreno por OpenCL; Mali, Immortalis, Xclipse y PowerVR por Vulkan) y NPU Hexagon de los Snapdragon 8 Gen 2 o posteriores (experimental) | Nexus-Office 100 % en el móvil, sin PC (Android 9+, ARM64). La APK lleva el mismo `nexus-office` compilado para Android y llama.cpp (CPU y GPU); detecta CPU, GPU y NPU, mide si el modelo va más rápido en GPU o en CPU y solo descarga el modelo adecuado a la RAM del móvil. Arranque al encender opcional. También se conecta al Nexus-Office del PC (*RED → Conectar a mi ordenador*). |
| **Android** (app Nexus-Office VPN) | `Nexus-Office-<ver>-android-vpn.apk` | ⚙️ Cliente del PC | App nativa (Android 7+). Conecta con el Nexus-Office de tu PC por **VPN WireGuard cifrada**, por HTTPS con certificado anclado o por la Wi‑Fi de la oficina. Ver [VPN.md](VPN.md). |
| iPhone / iPad | PWA | ⚠️ Cliente | iOS no permite ejecutar este tipo de servidor; úsalo como app web conectada a tu PC o a tu red cooperativa. |
| **Xbox Series** | PWA (Microsoft Edge) | ⚠️ Cliente | Las consolas son plataformas cerradas. Abre la interfaz de tu PC desde Edge (puede instalarse como app). |
| **PlayStation 5** | Navegador | ⚠️ Cliente | Sin instalación posible; acceso por navegador a un Nexus-Office de la red. |
| **Nintendo Switch** | — | ❌ | No dispone de un navegador general utilizable. |

> **Por qué las consolas son "cliente"**: PlayStation, Xbox y Switch solo ejecutan software firmado por el fabricante. Nexus-Office sí funciona en ellas como interfaz (PWA) mientras la IA corre en un PC, Steam Deck, servidor o en tu red cooperativa, que además permite que varios equipos compartan el modelo más potente.

La edición C++ se instala en carpetas propias (`Nexus-Office-C`, `nexus-office-c`), así que convive con la edición
Python sin pisarla. Ver [CPP.md](CPP.md#convivencia-con-la-edición-python).

## Qué hace cada instalador

| Instalador | Cómo instala | Opciones |
|---|---|---|
| `.exe` (Windows, NSIS) | Copia la carpeta portable a `%LOCALAPPDATA%\Nexus-Office-C` (o la elegida) sin pedir administrador y después ejecuta `nexus-office` con las opciones elegidas: `oficina <perfil>`, `nexus-office modelos --descargar` y, si se pidió, `nexus-office servicio instalar [--red]` con UAC solo para ese paso. Accesos directos sin consola (`nexus-office-w.exe`) y *Nexus-Office (consola)* con `start.bat`. | Página de opciones: perfil de oficina, descargar el modelo, servicio al encender, acceso desde la red local, acceso directo en el escritorio. Silencioso: `/S /OFICINA=<perfil> /SERVICIO /RED /MODELOS=auto\|todos\|no /D=<carpeta>` (`/D` siempre al final). |
| `.run` (Linux) | Script `sh` con el `.tar.gz` incrustado. Lo extrae en la carpeta indicada (por defecto `~/.local/share/nexus-office-c`, o `/opt/nexus-office-c` con `sudo`) y ejecuta `install.sh` con el resto de opciones. | `sh Nexus-Office-<ver>-linux-<arch>.run [carpeta] [--oficina=<perfil>] [--servicio [--red]] [--modelos=auto\|todos\|no]` |
| `.deb` (Debian, Ubuntu, Raspberry Pi OS 64 bits) | Programa en `/opt/nexus-office-c`, lanzador `/usr/bin/nexus-office-c` y entrada de menú. No hay nada que preparar al instalar: el programa ya está listo. Los datos de cada usuario van a `~/.local/share/nexus-office-c` (`NEXUS_DATA_DIR`). La primera ejecución evalúa el hardware y descarga el modelo. `prerm` quita el servicio. | `nexus-office-c [servicio\|modelos\|oficina\|detect\|pull\|chat\|comprobar\|firmware\|grafo] …`; servicio con `sudo nexus-office-c servicio instalar [--red]`. |
| `.AppImage` y `.flatpak` (Linux) | Nexus-Office ya instalado dentro, de solo lectura. Ver [AppImage y Flatpak](#appimage-y-flatpak). | Sin argumentos arrancan Nexus-Office; con argumentos son órdenes de `nexus-office`. |
| `.tar.gz` (macOS) | Carpeta portable. `Nexus-Office.command` ejecuta `install.sh` la primera vez (quita la cuarentena de Gatekeeper a los binarios incluidos) y después `start.sh`. | `./install.sh --oficina=<perfil> --servicio [--red] --modelos=…` |

Con el **instalador gráfico de Windows** y con `install.sh`/`install.bat`, el último paso es `nexus-office modelos`:
evalúa el hardware, elige el modelo de cada tipo y lo descarga si hay internet (ver
[Guía de usuario](USUARIO.md#modelo-de-ia-elegido-según-tu-equipo)). Sin internet solo muestra la recomendación.

### Opciones de los scripts de instalación

`install.sh` (Linux, macOS) e `install.bat` (Windows) admiten las mismas opciones:

| Opción | Qué hace |
|---|---|
| `--oficina=<perfil>` | Perfil de oficina (también `NEXUS_OFICINA`). Ver [OFICINA.md](OFICINA.md). |
| `--modelos=auto\|todos\|no` | `auto` (por defecto): modelo de chat y visión, de documentos (embeddings) y de voz; `todos`: también el de imagen; `no`: solo muestra la recomendación. También `NEXUS_MODELOS`. |
| `--servicio` | Instala el arranque automático (también `NEXUS_SERVICIO=1`). Ver [SERVICIO.md](SERVICIO.md). |
| `--red` | Con el anterior: el servicio escucha en `0.0.0.0`. |

Las opciones de la edición Python (`--offline`, `--clean`, `--no-clean`, `--skip-extras`, `--force`) se aceptan y no
hacen nada: aquí no hay Python ni dependencias que instalar.

### Aceleración incluida en los instaladores
Cada instalador lleva todo lo necesario para acelerar con la GPU o la NPU de su plataforma, sin internet. Nexus-Office
elige al arrancar: prueba primero la variante de GPU adecuada a tu hardware y, si el equipo no tiene el driver (por
ejemplo, `libcuda` o `vulkan-1.dll`), pasa a la siguiente y, al final, a la CPU. La comprobación del arranque
(*Sistema → Comprobación y autorreparación*) también pasa de la GPU a la CPU si la GPU no arranca.

| Instalador | llama-server (chat, visión, embeddings) | sd-cli (imagen) | Voz |
|---|---|---|---|
| Windows x64 | CUDA 12.9 (NVIDIA), Vulkan (NVIDIA, AMD, Intel), OpenVINO (NPU Intel), CPU | Vulkan, CPU | Piper, Whisper |
| Windows ARM64 | Vulkan (Adreno y otras), CPU | Vulkan, CPU | Piper, Whisper |
| Linux x86_64 | CUDA 12.9 (NVIDIA), Vulkan (NVIDIA, AMD, Intel), OpenVINO (NPU Intel), CPU | Vulkan, CPU | Piper, Whisper |
| Linux ARM64 | CUDA (NVIDIA Jetson Orin, JetPack 6), Vulkan (Mali, Adreno, Raspberry Pi 5...), CPU | Vulkan, CPU | Piper, Whisper |
| macOS Apple Silicon | Metal (GPU de Apple), CPU | CPU | Piper, Whisper |
| macOS Intel | CPU | CPU | Piper, Whisper |

- **Vulkan** acelera las GPU de cualquier fabricante con su driver normal; enlaza con el cargador de Vulkan del
  sistema (`libvulkan.so.1` en Linux, `vulkan-1.dll` en Windows), que instala cualquier driver de GPU.
- **CUDA** necesita el driver de NVIDIA 525 o posterior.
- **No incluidos**: ROCm (AMD) y SYCL (Intel), que pesan varios GB; esas GPU van por Vulkan, y con internet se puede
  instalar la variante oficial desde *Sistema → Runtimes*.
- Selector de motor y CPU/GPU/NPU: ver [ACELERADOR.md](ACELERADOR.md).

### Limpieza

`nexus-office cleanup --dry-run` muestra las variantes de llama-server y sd-cli que tu equipo no usa, y
`nexus-office cleanup` las borra. La variante en uso y la de CPU de respaldo nunca se borran. Tras limpiar, la carpeta
ya no sirve para llevarla a otro equipo con otro hardware: para eso, guarda el instalador original.

### AppImage y Flatpak

`nexus-construir paquetes` genera `Nexus-Office-<ver>-linux-<arch>.AppImage` y `.flatpak` para x86_64 y aarch64. Los dos
son de solo lectura, así que Nexus-Office va **ya instalado** dentro, con `bundled/runtimes`. Los datos van a
`~/.local/share/nexus-office-c` (AppImage) o a la carpeta de la app en `~/.var/app/ai.nexus.NexusOfficeCpp` (Flatpak).
Sin argumentos arrancan Nexus-Office (y la primera vez eligen y descargan el modelo); con argumentos son órdenes de
`nexus-office` (`servicio`, `modelos`, `oficina`, `chat`...).

- **AppImage**: `appimagetool` y el `runtime-<arch>` de AppImage/type2-runtime (se descargan a `.cache/appimage/`).
  El servicio apunta al fichero `.AppImage` (variable `APPIMAGE`), no a la carpeta temporal donde se monta.
- **Flatpak**: se arma sin `flatpak-builder` (`flatpak build-export` + `build-bundle --runtime-repo` de Flathub): al
  instalarlo, Flatpak descarga `org.freedesktop.Platform 24.08`. Permisos: red, X11/Wayland, audio, todos los
  dispositivos (GPU y puertos serie de las placas) y `xdg-config/systemd` para dejar la unidad del servicio.

## Instalar desde el código fuente

`Nexus-Office-<ver>-codigo-fuente.zip` trae `nexus-office-cpp/` (programa, recursos, app Android, pruebas, documentación
y la herramienta de construcción), el firmware de las placas y los flujos de GitHub Actions. Se compila con CMake:

```bash
cmake -S nexus-office-cpp -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix ~/Nexus-Office      # carpeta portable: nexus-office y nexus/ (web y recursos)
```

Hace falta un compilador C++20 (GCC 12, Clang 15, MSVC 2022 o Apple Clang 15) y CMake 3.20. No hace falta Python. Para
usar la GPU, copia en `bundled/runtimes/llama/<variante>/` el llama-server de esa variante (o instálalo desde
*Sistema → Runtimes* con internet).

**WSL**: instala Nexus-Office en tu carpeta de Linux (`~/Nexus-Office`), no en `/mnt/c/...`, que es mucho más lenta.

## Servicio y descargas para la oficina

- **Servicio**: `nexus-office servicio instalar|quitar|estado [--red] [--puerto P]` (`src/core/service.cpp`). Usa
  systemd (del sistema como root, de usuario con `loginctl enable-linger` si no), cron `@reboot` sin systemd, launchd
  (LaunchDaemon o LaunchAgent) o el Programador de tareas de Windows (al iniciar el equipo como SYSTEM si es
  administrador, al iniciar sesión si no; reinicio ante fallos, sin límite de tiempo y regla de firewall con `--red`).
  La app Nexus-Office autónomo arranca su servicio en primer plano al encender. Todo en [SERVICIO.md](SERVICIO.md).
- **Descargas**: Nexus-Office lista los ficheros de `<datos>/descargas/` (por ejemplo,
  `data/descargas/windows/Nexus-Office-Setup-<ver>-windows-x64.exe`) y los clasifica por plataforma según su nombre.
  Para repartir los instaladores en la oficina, copia ahí la carpeta de una entrega (`windows/`, `linux/`, `macos/`,
  `android/`…). *Sistema → Descargas* marca el del sistema anfitrión y el del dispositivo que mira la página. Se
  sirven en `/descargas/<ruta>` con la misma protección de clave de red que el resto.

## Empaquetado

Lo hace `nexus-construir` (`tools/construir/`, en C++), que se compila junto a Nexus-Office. En GitHub Actions,
`.github/workflows/cpp-entrega.yml` compila en cada sistema, construye los instaladores y los prueba instalados con un
modelo real; `cpp-publicar.yml` firma la APK, genera los manuales en PDF y publica la Release.

**1. Entradas en `.cache/`**:

- `.cache/cpp/<plataforma>/Nexus-Office/`: la carpeta de `cmake --install` en cada sistema (artefacto `cpp-<plataforma>`
  de `cpp.yml`).
- `.cache/desktop-runtimes/<plataforma>/{llama,sd,piper,whisper}/`: los motores de IA.
- `.cache/drivers/<plataforma>/`: drivers USB-serie para Windows y macOS (opcional).

**2. Instaladores**:

```bash
sudo apt install nsis                                        # makensis, para el .exe
build/nexus-construir paquetes                               # todas las plataformas
build/nexus-construir paquetes --targets windows-x64 linux-aarch64
build/nexus-construir paquetes --targets --keep --apk dist-apk/*.apk --docs *.pdf   # solo código fuente, LEEME y sumas
build/nexus-construir pdf manuales                           # Manual-de-usuario.pdf y Documentacion-tecnica.pdf (Chromium)
```

| Opción | Qué hace |
|---|---|
| `--targets` | `windows-x64`, `windows-arm64`, `linux-x86_64`, `linux-aarch64`, `macos-arm64`, `macos-x86_64` (por defecto, todas; vacío, ninguna). |
| `--apk` | APK ya compiladas, que se copian a `dist/android/`. |
| `--docs` | Ficheros extra para la raíz de `dist/` (los manuales en PDF, por ejemplo). |
| `--keep` | No borra `dist/` al empezar. |

Sin `makensis`, sigue con las demás plataformas y avisa de que falta el `.exe`. Resultado:

```
dist/
  windows/  Nexus-Office-Setup-<ver>-windows-x64.exe, Nexus-Office-Setup-<ver>-windows-arm64.exe
  linux/    Nexus-Office-<ver>-linux-{x86_64,aarch64}.run / .AppImage / .flatpak,
            nexus-office-c_<ver>_amd64.deb, nexus-office-c_<ver>_arm64.deb
  macos/    Nexus-Office-<ver>-macos-arm64.tar.gz, Nexus-Office-<ver>-macos-x86_64.tar.gz
  android/  Nexus-Office-<ver>-android-autonomo.apk, Nexus-Office-<ver>-android-vpn.apk   (de --apk)
  Nexus-Office-<ver>-codigo-fuente.zip, LEEME.txt, SHA256SUMS.txt (+ los ficheros de --docs y los ejemplos)
```

El zip de código fuente contiene solo código, pruebas, documentación, empaquetado, firmware y la app Android, sin
compilados, datos, modelos ni cachés; `nexus-construir` se para si encuentra ficheros sensibles (claves, `.env`,
`config.yaml`, registros).

Los modelos no se incluyen en los instaladores (son de varios GB y tienen sus propias licencias): los descarga el
propio instalador si hay internet, la aplicación desde *Modelos → Recomendados* o `nexus-office modelos --descargar`.

En la red de la oficina es cómodo copiar `dist/` en `data/descargas/` del PC con Nexus-Office y descargarlos desde
*Sistema → Descargas*.

## Apps Android

La app **autónoma** es la única que hace falta: lleva la IA en el móvil y, en *RED → Conectar a mi ordenador*, el mismo
cliente de la app VPN (red local, HTTPS anclado y VPN WireGuard). La app VPN se sigue publicando para móviles que no
pueden con la IA local. Las dos pueden instalarse a la vez:

| | Nexus-Office VPN | Nexus-Office autónomo |
|---|---|---|
| Fichero | `Nexus-Office-<ver>-android-vpn.apk` | `Nexus-Office-<ver>-android-autonomo.apk` |
| Android | 7.0 o superior (API 24) | 9.0 o superior (API 28), ARM64 |
| Dónde corre la IA | En tu PC con Windows, Linux o macOS | En el propio móvil (modelo elegido según su RAM), en la GPU si va más rápida, si no en la CPU |
| Arranque al encender | — | Opcional: *Sistema → Arranque automático* → **ARRANCAR AL ENCENDER** |

La APK autónoma se compila con `nexus-construir apk --exe <nexus-office para Android> --libs <llama.cpp> --recursos <nexus/>`
(aapt2, d8, zipalign y apksigner del SDK de Android, sin Gradle). La app Java está en `android/`. En Actions lo hace el
trabajo `android` de `cpp-entrega.yml`, que la prueba en un emulador de Android 11.

## Portabilidad
La carpeta instalada es autosuficiente: copiarla a otro equipo con el mismo sistema y arquitectura conserva modelos,
memoria y configuración; Nexus-Office re-detecta el hardware al arrancar y adapta el perfil. Si tenías el servicio
instalado, vuelve a instalarlo en la carpeta nueva.

## Acceso desde otros dispositivos (móvil, tablet, consola, TV)
En *Red COOP → Conectar dispositivos* (modo admin) activa **Permitir acceso desde la red** y reinicia Nexus-Office (o
instala el servicio con `--red`). Los dispositivos que no sean el propio PC necesitan la **clave de red** (se muestra
ahí; la app Android y la URL con `?key=` la recuerdan). `/api/ping` es lo único público, para que la app encuentre el
servidor.
