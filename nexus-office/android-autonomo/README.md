# Nexus-Office Autónomo para Android

APK: `Nexus-Office-<versión>-android-autonomo.apk`. Paquete `ai.nexus.office.autonomo`, para **Android 9 o superior con ARM64**, que son prácticamente todos los móviles desde 2017.

Nexus-Office completo dentro del móvil, sin PC. **La APK lo lleva todo menos los modelos de IA**:

| Dentro de la APK | Qué es |
|---|---|
| Python 3.12 para ARM64 | Con ssl, sqlite3, ctypes, lzma y bz2 |
| Dependencias de Nexus-Office | FastAPI, uvicorn, numpy, pydantic, cryptography, Pillow, psutil… |
| El código de Nexus-Office | Memoria, RAG, importación de documentos y ZIP/TAR, agentes, rastreo web y webui |
| `llama-server` (llama.cpp) en tres variantes de CPU | ARMv8 básica, ARMv8.2 con *dotprod* y ARMv8.6 con *i8mm*. Nexus-Office usa la más rápida que admite la CPU. |
| `llama-server` con GPU | llama.cpp enlazado con la libc de Android, con los backends **OpenCL** (Adreno de Qualcomm) y **Vulkan** (Mali e Immortalis de ARM, Xclipse de Samsung, PowerVR…) y la CPU en siete variantes de ARM (hasta ARMv9.2 con SVE2 y SME). Cada backend se carga solo si el móvil tiene su driver. |

**Modelos**: al abrir la app por primera vez, Nexus-Office elige el modelo adecuado a la RAM del móvil y solo descarga ese.

| RAM del móvil | Modelo que descarga |
|---|---|
| Menos de 10 GB | Qwen3‑VL 2B Thinking (≈ 1,5 GB) |
| 10 GB o más | Qwen3‑VL 4B Thinking (≈ 2,8 GB) |

Si el modelo va lento en ese móvil, Nexus-Office propone cambiar al más pequeño (y al revés si sobra potencia).
En **Sistema > Arranque automático** se puede activar que Nexus-Office arranque solo al encender el móvil.

Con Wi‑Fi empieza a descargar solo. Con datos móviles, pregunta antes. En *Modelos* se puede cambiar.

**Oficina para pymes**: los perfiles por gremio (herrería, calderería, reformas…) también van en la APK. Se eligen en *Agentes → Perfil de oficina* (modo ADMIN) y funcionan sin internet. Ver [Oficina para pymes](../docs/OFICINA.md).

## CPU, GPU y NPU

Al arrancar, Nexus-Office mira qué tiene el móvil (`nexus/hardware/android_accel.py`) y lo muestra en *Sistema → Hardware*:

| Pieza | Cómo la detecta | Para qué la usa |
|---|---|---|
| **CPU** | El SoC (`Build.SOC_MODEL`, `ro.soc.model`, `ro.board.platform`) con su nombre comercial (p. ej. «Qualcomm SM8650 (Snapdragon 8 Gen 3)») y sus núcleos rápidos | La variante de llama.cpp y los hilos |
| **GPU** | El nombre por EGL (lo escribe la app en `device_info.json`) y, sobre todo, `libllama-server-gpu.so --list-devices`, que dice qué GPU puede usar llama.cpp de verdad | El modelo entero en la GPU: **Adreno por OpenCL**, el resto **por Vulkan**. En Adreno no se usa Vulkan porque en llama.cpp va más lento que la CPU. |
| **NPU** | Por el SoC y por las bibliotecas del fabricante: Hexagon (Qualcomm), APU (MediaTek), TPU (Google Tensor), NPU (Exynos). La Hexagon de los **Snapdragon 8 Gen 2 o posteriores** (8 Gen 3, 8 Elite, 8 Elite Gen 5, 8s Gen 3…) aparece además en `--list-devices` como `HTP0` | En esos Snapdragon, **el modelo entero en la NPU** con el backend Hexagon de llama.cpp, por delante de la GPU. Las demás NPU (MediaTek, Tensor, Exynos y Snapdragon anteriores) no tienen backend en llama.cpp: se informa de ellas y el modelo va en la GPU o en la CPU. |

El orden es **NPU → GPU → CPU**. Si hay NPU o GPU utilizable, el perfil pone todas las capas en ella (`-ngl 999 --device HTP0`, `GPUOpenCL` o `Vulkan0`). Después el autoajuste **mide** unas respuestas con ella y otras con la CPU y se queda con lo más rápido, porque hay GPU de gama baja que van peor que los núcleos grandes. Si la NPU falla al arrancar llama-server, Nexus-Office lo apunta (`android_accel.json`, campo `npu_broken`) y pasa a la GPU; si falla la GPU (`gpu_broken`), sigue en la CPU, sin cortar la conversación. No vuelve a probar lo que falló hasta que se actualiza la app o Android.

**Cómo usa la NPU**: la APK lleva `libggml-hexagon.so` (la parte que corre en Android) y los programas del DSP `libggml-htp-v73/v75/v79/v81.so`, uno por generación de Hexagon. `libggml-hexagon.so` abre `libcdsprpc.so` (FastRPC, del fabricante, en `/vendor/lib64`) y carga en el DSP el programa de su generación, que busca en `ADSP_LIBRARY_PATH` (la carpeta de bibliotecas de la app). En un móvil sin `libcdsprpc.so` el backend no encuentra la NPU y no pasa nada. Se compila con el Hexagon SDK de Qualcomm (`.github/workflows/android-npu.yml`). El backend Hexagon de llama.cpp aún es experimental, y la NPU no se ha podido probar en un móvil real al construir esta versión: si en el tuyo falla, la app sigue sola en la GPU o en la CPU.

La imagen (stable-diffusion.cpp) y la voz Piper no van en la APK: la voz es la del sistema.

## Se optimiza sola

Nexus-Office se ajusta al móvil al arrancar y mientras lo usas (`nexus/android_tune.py`):

| Qué ajusta | Cómo |
|---|---|
| NPU, GPU o CPU | Con NPU o GPU utilizable, compara la velocidad real con ella y con la CPU (3 respuestas cada una) y se queda con lo más rápido. Lo recuerda por modelo y versión. Si la NPU no arranca, compara la GPU. |
| Variante de llama.cpp | La más rápida que admite la CPU: i8mm, dotprod o ARMv8 básica |
| Hilos | Empieza con los núcleos rápidos. Después mide la velocidad real de tus respuestas con 2 o 3 valores y se queda con el mejor. Vuelve a medir al cambiar de modelo o de versión, y de vez en cuando. |
| Temperatura y batería | Móvil caliente, batería baja sin cargar o ahorro de energía: la mitad de hilos hasta que se normaliza |
| Contexto | 4096 de inicio. Si falta RAM libre, lo reduce; si sobra en sesiones estables, lo amplía hasta 8192 |
| Modelo | Si va lento propone uno más pequeño; si sobra velocidad y RAM, uno mayor. Lo descarga y lo activa al aceptar. |

Los cambios se aplican entre respuestas. Para ver lo que ha decidido: *Sistema → Autoajuste*, o `GET /api/android/autoajuste`.

## Cómo funciona

- **Actualizar**: instala la APK nueva encima de la anterior. Se conservan los datos y los modelos, y la app prepara el runtime nuevo.
- **Primer arranque**: la app comprueba que hay unos 350 MB libres. Después descomprime `assets/runtime.zip` en sus datos privados con varios hilos, mostrando el progreso, y verifica ficheros y bytes contra `assets/runtime.json`. Luego arranca en pocos segundos.
- **Sin cuelgues**: en cada arranque comprueba que el entorno sigue completo y, si no, lo repara solo. Si Nexus-Office no responde en 4 minutos, lo para y muestra las últimas líneas del registro. La pantalla de arranque ofrece **Reintentar**, **Reparar** y **Copiar registro**. El registro está en `nexus.log`, y el del arranque anterior en `nexus.prev.log`.
- **Tras cada inicio**: limpia temporales, optimiza la base de datos, aplica los hilos medidos y precarga el modelo si el móvil está en condiciones.
- **Servicio en primer plano**: mantiene Nexus-Office en `127.0.0.1:8775`, solo accesible desde el propio móvil. La notificación «Nexus-Office está activo» tiene el botón **Detener**.
- **Cómo se arranca Python**: Android no deja ejecutar ficheros de los datos de la app. La app ejecuta el cargador de musl (`libnexusld.so`), que va entre las bibliotecas nativas de la APK, y es el cargador quien carga el intérprete. Nexus-Office lanza `llama-server` directamente desde las bibliotecas nativas.
- **Por qué hay dos llama.cpp**: los de CPU son estáticos con musl, como Python, y no pueden cargar los drivers de la GPU, que son bibliotecas de Android. El de GPU está enlazado con la libc de Android y se lanza con `LD_LIBRARY_PATH` en sus bibliotecas, `/system/lib64` (Vulkan) y `/vendor/lib64` (el `libOpenCL.so` del fabricante).
- **Llamadas al sistema que Android prohíbe**: si un paquete usa una que el filtro seccomp de Android no permite (por ejemplo `mbind`, de OpenBLAS), Android mata el proceso con el «código 159». La app carga `libnexussys.so`, que convierte esas llamadas en «no disponible» (`ENOSYS`) para que Nexus-Office siga funcionando. Cada una queda anotada en `nexus.log`.
- **DNS**: el Python de la app no puede usar el DNS de Android. La app le pasa los servidores DNS de la red activa y Nexus-Office resuelve los nombres él mismo (`nexus/android_dns.py`). Si no responden, usa 1.1.1.1, 8.8.8.8 o 9.9.9.9.
- **Interfaz**: voz del sistema, adjuntar varios ficheros, y botones COPIAR y DESCARGAR.

## Conectar a mi ordenador (desde 2.2)

La misma APK también se conecta al Nexus-Office de tu PC (Windows, Linux o macOS): en la barra superior, **RED → Conectar a mi ordenador**. Ofrece lo mismo que la app VPN:

- **Red local**: busca tu PC en la Wi‑Fi o escribe su dirección.
- **Internet**: con el código de emparejamiento del PC, por HTTPS con el certificado anclado.
- **VPN cifrada (WireGuard)**: con el código NEXUSV1 del panel VPN del PC. Solo pasa por la VPN el tráfico de la app.

El botón **USAR NEXUS-OFFICE EN ESTE MÓVIL** vuelve a la IA del teléfono, que sigue funcionando sin conexión. La app VPN ligera (`ai.nexus.office.client`, carpeta `../android`) se sigue publicando para móviles que no son ARM64 o con poca memoria; las dos pueden estar instaladas a la vez.

## Compilar

En Linux o WSL, sin Gradle. Para la parte de CPU no hace falta el SDK ni el NDK de Google; para el llama-server con GPU hace falta el **NDK** (r26 o posterior) en `ANDROID_NDK_HOME` o en `.cache/android/ndk/`, y las cabeceras `opencl-c-headers`, `libvulkan-dev` y `spirv-headers` de Ubuntu. Sin NDK la APK sale igual, solo con CPU.

```bash
sudo apt install python3.12 cmake ninja-build autoconf automake libtool tcl curl binutils-aarch64-linux-gnu \
                 qemu-user-static aapt zipalign apksigner dalvik-exchange android-sdk-platform-23 openjdk-17-jdk-headless
bash packaging/build_android_runtime.sh --probar   # Python + llama.cpp para ARM64
venv/bin/python packaging/build_apk_autonomo.py    # deja dist/Nexus-Office-<versión>-android-autonomo.apk
```

`build_android_runtime.sh` lo hace todo desde el código fuente:

1. Compila Python y sus bibliotecas con **zig** (PyPI) para `aarch64-linux-musl`. Las fuentes salen de Ubuntu.
2. Instala las ruedas `musllinux_aarch64` de PyPI.
3. Compila `llama-server` estático desde el paquete fuente de `llama-cpp-python`.
4. Con el NDK, compila el mismo llama.cpp para Android con `GGML_BACKEND_DL`, Vulkan, OpenCL (kernels de Adreno) y todas las variantes de CPU de ARM. Para enlazar OpenCL genera un `libOpenCL.so` de solo nombres que **no** va en la APK: en el móvil se carga el del fabricante.
5. Con `--probar`, arranca Nexus-Office emulando ARM64 con qemu y comprueba que responde.

El resultado queda en `.cache/android-runtime/`. Solo hace falta repetirlo al cambiar dependencias.

`build_apk_autonomo.py` necesita el mismo `android.jar` que la app VPN (ver [PLATAFORMAS](../docs/PLATAFORMAS.md#compilar-la-app-nexus-vpn)) y firma con la misma clave.

La versión de la APK sale de `nexus/__init__.py` y de `APK_REVISION` en `packaging/build_apk.py`: 1.6.0 con la revisión 1 (GPU) da versionCode 10601, así que se instala encima de la 10600 conservando datos y modelos.

## Notas

- **RAR y 7z**: no están dentro del móvil, porque no hay `unrar` ni `7z`. ZIP y TAR sí. Para importar un RAR, conviértelo a ZIP o impórtalo en el Nexus-Office del PC.
- **Paralelismo**: el rastreo y la importación usan hilos, porque en Android no se lanzan procesos hijo de Python.
- **Herramientas que ejecutan código Python**: se lanzan a través del mismo cargador (`nexus/proc.py: python_cmd`).
- **Datos**: se guardan en el almacenamiento privado de la app y se borran al desinstalarla.
- **x86_64**: emuladores y Chromebooks Intel no están incluidos. La APK es solo ARM64.
