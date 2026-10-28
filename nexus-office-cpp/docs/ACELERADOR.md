# Motor de IA y acelerador (CPU, GPU o NPU)

Desde la versión 2.0, al arrancar Nexus-Office puedes elegir a mano **con qué motor** se ejecuta el modelo de IA y
**en qué chip**: el procesador (CPU), la tarjeta gráfica (GPU) o la unidad de IA (NPU). Si no eliges nada, lo decide
Nexus-Office según tu equipo, como hasta ahora.

## Dónde se elige
- **Al arrancar** aparece la ventana **MOTOR Y ACELERADOR**. Muestra tu procesador, memoria, GPU y NPU detectadas, y
  qué opciones están disponibles en este equipo (las que no, dicen por qué).
- **PROBAR** genera unas palabras con la opción marcada y dice cuánto ha tardado, los tokens por segundo y **dónde ha
  quedado el modelo de verdad** (por ejemplo, «GPU · 37/37 capas · Vulkan0 (NVIDIA GeForce RTX 4060)»).
- **ARRANCAR** la aplica. Con **Recordar y no volver a preguntar al abrir** (marcado por defecto) no vuelve a preguntar.
- Para cambiarla después: *Sistema → MOTOR Y CPU/GPU/NPU* (junto a *Redetectar*).
- Desde la terminal: `nexus-office run --motor llamaserver --acelerador gpu` (cualquiera de las dos opciones fija
  la elección y no pregunta).

## Perfiles de hardware (2.2)
Un **perfil** guarda de una vez cómo usar tu equipo: motor, acelerador, GPU, modelo, contexto, capas en la GPU y
dónde va la memoria de la conversación (caché KV: en la **VRAM**, más rápida, o en la **RAM**, para que quepa un
modelo más grande en una gráfica pequeña), y si se bloquea el modelo en RAM (`--mlock`).

| Perfil de serie | Para qué |
|---|---|
| **Automático** | Lo decide Nexus-Office según el equipo (como siempre) |
| **Rendimiento** | GPU, modelo entero en la VRAM y contexto máximo |
| **Equilibrado** | GPU si la hay, contexto moderado (8192) |
| **Modelo grande** | Parte del modelo en la VRAM y el resto y el contexto en la RAM (bloqueada) |
| **Ahorro** | CPU, contexto corto (2048) y la mitad de los hilos: menos consumo |
| **Solo CPU** | Sin GPU ni NPU |

Se elige en el menú **PERFIL** de la barra superior, que muestra solo lo que tu hardware permite. **Editar perfiles**
crea, copia o cambia los tuyos (si cambias uno de serie y lo borras, vuelve a ser el original). Al activar uno, Nexus-Office reinicia el motor con esos
valores. API: `GET /api/hw/perfiles`, `PUT` y `DELETE /api/hw/perfiles/{id}`, `POST /api/hw/perfiles/{id}/activar`.
Configuración: `hardware.perfil` y `hardware.perfiles`.

## Equipos con dos o más GPU y Linux (2.2)
- llama-server se arranca con `--device` para la GPU elegida (la lista sale de `llama-server --list-devices`). Antes
  podía caer en la integrada aunque hubiera una dedicada, o repartir capas entre las dos.
- En el menú **IA** eliges la GPU cuando hay más de una (`POST /api/hw/gpu`, `hardware.gpu`). En *Automático* se
  prefiere la dedicada con más VRAM.
- Los dispositivos **de software** (llvmpipe, lavapipe, SwiftShader) no se usan nunca: en Linux sin driver de
  Vulkan aparecían como «GPU» e iban más lentos que la CPU. Si es lo único que hay, se queda en CPU y lo dice.
- El nombre de la GPU se reconoce aunque llama.cpp le añada sufijos (por ejemplo «Intel(R) UHD Graphics 770 (ADL-S GT1)»).
- Si la GPU elegida falla al arrancar, Nexus-Office vuelve a la CPU y lo muestra en el menú **IA** (`gpu_error`).

No se ha podido probar con dos GPU físicas; la elección se ha probado con la salida real de `--list-devices` de varios equipos.

## Motores

| Motor | Qué es | CPU | GPU | NPU | Visión |
|---|---|---|---|---|---|
| **Automático** | El mejor disponible (normalmente llama-server) | ✓ | ✓ | ✓ | ✓ |
| **llama-server** | llama.cpp oficial como proceso aparte | ✓ | CUDA, Vulkan, Metal | OpenVINO (Intel), Hexagon (Android) | ✓ |

llama-server va **incluido en los instaladores offline** con todas sus variantes: no se descarga nada al elegirlo.
La opción **llama.cpp integrado** de la edición Python (llama-cpp-python dentro del programa) no existe en la edición
C++: el selector la muestra como no disponible y llama-server, que es el mismo llama.cpp, hace su trabajo.

Ollama ya no aparece en el selector porque es otra aplicación que hay que instalar aparte y no viene en el paquete.
Si lo tienes instalado, Nexus-Office lo sigue usando cuando no hay otro motor.

## Aceleradores

- **CPU**: siempre disponible. El modelo no usa la GPU aunque la haya.
- **GPU**: NVIDIA con CUDA; AMD, Intel, NVIDIA, Adreno, Mali o Raspberry Pi 5 con Vulkan; Mac con Metal; en Android,
  la GPU del móvil (OpenCL o Vulkan). Con una GPU integrada se avisa de que puede ir más lenta que la CPU con modelos
  grandes.
- **NPU**:
  - **Intel Core Ultra (AI Boost)**, Windows x64 y Linux x86_64: llama-server con **OpenVINO** en la NPU
    (`GGML_OPENVINO_DEVICE=NPU`). La NPU trabaja con grafos fijos, así que se usa una sola conversación a la vez y
    un contexto de hasta 4096 tokens.
  - **Snapdragon con Hexagon v73 o posterior** (app Android autónoma): llama.cpp en la NPU (HTP). Si el driver del
    móvil no lo permite, pasa solo a la GPU y luego a la CPU.
  - **AMD Ryzen AI, Apple Neural Engine y Qualcomm en Windows**: llama.cpp no puede ejecutar ahí el modelo de texto.
    El selector lo dice y usa la GPU (o la CPU). Esas NPU se siguen usando para embeddings y voz cuando su runtime
    está instalado.

## Cómo se comprueba que acelera de verdad
Tras arrancar llama-server, Nexus-Office lee su registro (`logs/llama-server.log`) y anota el dispositivo real: capas
cargadas en la GPU, dispositivo CUDA, Vulkan o Metal, la NPU Hexagon u OpenVINO. Si lo pedido no se ha cumplido
(por ejemplo, falta el driver de la GPU), lo dice en el selector y en el registro, y el modelo sigue en lo que sí
funciona.

## Runtimes incluidos por instalador

| Instalador | llama-server | NPU |
|---|---|---|
| Windows x64 | CPU, Vulkan, CUDA 12 | OpenVINO (Intel AI Boost) |
| Windows ARM64 | CPU, Vulkan | — |
| Linux x86_64 (.deb, .run, AppImage, Flatpak) | CPU, Vulkan, CUDA 12 | OpenVINO (Intel AI Boost) |
| Linux ARM64 y Raspberry Pi | CPU, Vulkan, CUDA (Jetson) | — |
| macOS Apple Silicon | CPU, Metal | — |
| macOS Intel | CPU | — |
| Android autónoma | CPU, OpenCL/Vulkan | Hexagon HTP |

## Configuración
- `llm.engine`: `auto` o `llamaserver` (`llamacpp` se acepta por compatibilidad con la edición Python, pero no está disponible).
- `hardware.accel`: `auto`, `cpu`, `gpu` o `npu`.
- `hardware.llama_variant`: variante de llama-server elegida (`cpu`, `vulkan`, `cuda`, `metal`, `openvino`).
- `startup.ask_accel`: `true` para preguntar al arrancar.

API: `GET /api/accel` (opciones, elección y dispositivo efectivo), `POST /api/accel` `{engine, accel, remember}`,
`POST /api/accel/test` (genera 16 tokens y mide).
