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
- Desde la terminal: `python -m nexus run --motor llamaserver --acelerador gpu` (cualquiera de las dos opciones fija
  la elección y no pregunta).

## Motores

| Motor | Qué es | CPU | GPU | NPU | Visión |
|---|---|---|---|---|---|
| **Automático** | El mejor disponible (normalmente llama-server) | ✓ | ✓ | ✓ | ✓ |
| **llama-server** | llama.cpp oficial como proceso aparte | ✓ | CUDA, Vulkan, Metal | OpenVINO (Intel), Hexagon (Android) | ✓ |
| **llama.cpp integrado** | llama-cpp-python dentro de Nexus-Office | ✓ | Metal en Mac | — | — |

Los dos motores van **incluidos en los instaladores offline**, con su runtime: no se descarga nada al elegirlos.
El integrado se instala desde el propio paquete la primera vez que lo eliges (unos segundos, sin internet).

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

| Instalador | llama-server | Motor integrado | NPU |
|---|---|---|---|
| Windows x64 | CPU, Vulkan, CUDA 12 | llama-cpp-python (CPU) | OpenVINO (Intel AI Boost) |
| Windows ARM64 | CPU, Vulkan | llama-cpp-python x64 (emulado) | — |
| Linux x86_64 (.deb, .run, AppImage, Flatpak) | CPU, Vulkan, CUDA 12 | llama-cpp-python (CPU) | OpenVINO (Intel AI Boost) |
| Linux ARM64 y Raspberry Pi | CPU, Vulkan, CUDA (Jetson) | llama-cpp-python (CPU, NEON) | — |
| macOS Apple Silicon | CPU, Metal | llama-cpp-python (Metal) | — |
| macOS Intel | CPU | llama-cpp-python (CPU) | — |
| Android autónoma | CPU, OpenCL/Vulkan | — | Hexagon HTP |

Los runtimes del selector (llama-cpp-python y llama-server con OpenVINO) se compilan y prueban con
`.github/workflows/selector-runtimes.yml` y la construcción de los instaladores los toma de ahí.

## Configuración
- `llm.engine`: `auto`, `llamaserver` o `llamacpp`.
- `hardware.accel`: `auto`, `cpu`, `gpu` o `npu`.
- `hardware.llama_variant`: variante de llama-server elegida (`cpu`, `vulkan`, `cuda`, `metal`, `openvino`).
- `startup.ask_accel`: `true` para preguntar al arrancar.

API: `GET /api/accel` (opciones, elección y dispositivo efectivo), `POST /api/accel` `{engine, accel, remember}`,
`POST /api/accel/test` (genera 16 tokens y mide).
