# Motor de IA y acelerador (CPU, GPU o NPU)

Desde la versión 2.0, al arrancar Agente IA puedes elegir a mano **con qué motor** se ejecuta el modelo de IA y
**en qué chip**: el procesador (CPU), la tarjeta gráfica (GPU) o la unidad de IA (NPU). Si no eliges nada, lo decide
Agente IA según tu equipo, como hasta ahora.

## Dónde se elige
- **Al arrancar** aparece la ventana **MOTOR Y ACELERADOR**. Muestra tu procesador, memoria, GPU y NPU detectadas, y
  qué opciones están disponibles en este equipo (las que no, dicen por qué).
- **PROBAR** genera unas palabras con la opción marcada y dice cuánto ha tardado, los tokens por segundo y **dónde ha
  quedado el modelo de verdad** (por ejemplo, «GPU · 37/37 capas · Vulkan0 (NVIDIA GeForce RTX 4060)»).
- **ARRANCAR** la aplica. Con **Recordar y no volver a preguntar al abrir** (marcado por defecto) no vuelve a preguntar.
- **Capas del modelo en la GPU / NPU** (2.6): *Automático* (las que quepan en la VRAM con el contexto pedido),
  *Todas*, *Ninguna* (todo en la CPU) o un **número de capas**; el resto va en la CPU. Con el modelo ya cargado la
  ventana dice cuántas capas tiene. Menos capas deja VRAM para el contexto; más capas va más rápido si caben.
- Para cambiarla después: *Sistema → MOTOR Y CPU/GPU/NPU* (junto a *Redetectar*) o *Ajustes → Administración*.
- Desde la terminal: `nexus-office run --motor llamaserver --acelerador gpu` (cualquiera de las dos opciones fija
  la elección y no pregunta).

## Perfiles de hardware (2.2)
Un **perfil** guarda de una vez cómo usar tu equipo: motor, acelerador, GPU, modelo, contexto, capas en la GPU y
dónde va la memoria de la conversación (caché KV: en la **VRAM**, más rápida, o en la **RAM**, para que quepa un
modelo más grande en una gráfica pequeña), y si se bloquea el modelo en RAM (`--mlock`).

| Perfil de serie | Para qué |
|---|---|
| **Automático** | Lo decide Agente IA según el equipo (como siempre) |
| **Rendimiento** | GPU, modelo entero en la VRAM y contexto máximo |
| **Equilibrado** | GPU si la hay, contexto moderado (8192) |
| **Modelo grande** | Parte del modelo en la VRAM y el resto y el contexto en la RAM (bloqueada) |
| **Ahorro** | CPU, contexto corto (2048) y la mitad de los hilos: menos consumo |
| **Solo CPU** | Sin GPU ni NPU |

Se elige en el menú **PERFIL** de la barra superior, que muestra solo lo que tu hardware permite. **Editar perfiles**
crea, copia o cambia los tuyos (si cambias uno de serie y lo borras, vuelve a ser el original). Al activar uno, Agente IA reinicia el motor con esos
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
- Si la GPU elegida falla al arrancar, Agente IA vuelve a la CPU y lo muestra en el menú **IA** (`gpu_error`).

No se ha podido probar con dos GPU físicas; la elección se ha probado con la salida real de `--list-devices` de varios equipos.

## Motores

| Motor | Qué es | CPU | GPU | NPU | Visión |
|---|---|---|---|---|---|
| **Automático** | El mejor disponible (normalmente llama-server) | ✓ | ✓ | ✓ | ✓ |
| **llama-server** | llama.cpp oficial como proceso aparte | ✓ | CUDA 12/13, ROCm, SYCL, Vulkan, Metal, OpenCL (Adreno) | OpenVINO (Intel), Hexagon (Android y Linux Snapdragon) | ✓ |
| **LiteRT-LM** (2.5) | Motor de Google para modelos `.litertlm` | ✓ | OpenCL/Vulkan/Metal/WebGPU | Google Tensor, Snapdragon, Dimensity, Intel (con el *dispatch* del fabricante) | — |
| **ONNX Runtime GenAI** (2.5) | Motor de Microsoft para carpetas ONNX con `genai_config.json` | ✓ | DirectML (cualquier GPU en Windows), CUDA | QNN (Snapdragon X en Windows ARM), OpenVINO, VitisAI | — |

| **Difusión ONNX** (3.2) | Motor de imagen propio sobre ONNX Runtime para carpetas diffusers/optimum | ✓ | TensorRT, CUDA, ROCm, DirectML, CoreML, WebGPU | QNN (Hexagon), OpenVINO, NNAPI | CLIP/SigLIP |

Desde la 3.2, LiteRT-LM, ONNX Runtime GenAI y Genie se cargan **dentro del proceso** de Agente IA; si uno se cae, se
marca y la siguiente vez va en su proceso aparte (`motores.en_proceso: false` lo fuerza siempre). La **matriz por chip**
(`GET /api/hal`, campo `matriz`) dice qué motor y proveedor se prueba primero en cada equipo. La difusión ONNX toma el
proveedor de `media.difusion_proveedor` (`auto` = la matriz).

llama-server va **incluido en los instaladores offline** con todas sus variantes: no se descarga nada al elegirlo.
La opción **llama.cpp integrado** de la edición Python (llama-cpp-python dentro del programa) no existe en la edición
C++: el selector la muestra como no disponible y llama-server, que es el mismo llama.cpp, hace su trabajo.

### Elegir motor y runtime a mano (2.8)
En **MOTOR Y ACELERADOR** hay tres grupos: el **motor** (Automático, llama-server, LiteRT-LM, ONNX Runtime GenAI,
Qualcomm Genie), **CPU/GPU/NPU** y el **runtime concreto** de ese motor. Automático elige según el hardware; a mano
se fuerza uno. Lo que no va en el paquete de ese sistema sale desactivado con el motivo.

| Motor | Runtimes que se pueden elegir | Formato del modelo |
|---|---|---|
| llama-server | CPU, CPU sin AVX2, CUDA 12, CUDA 13, ROCm, Vulkan, SYCL, OpenVINO, Metal, OpenCL Adreno, Snapdragon/Hexagon, CANN, WebGPU (los que traiga el paquete) | `.gguf` |
| LiteRT-LM | CPU, GPU, NPU (Google Tensor, Hexagon, MediaTek APU, Intel) | `.litertlm` |
| ONNX Runtime GenAI | el del modelo, CPU, CUDA, DirectML, QNN, OpenVINO, VitisAI | carpeta con `genai_config.json` |
| Qualcomm Genie | NPU Hexagon (HTP) | carpeta con `genie_config.json` (Qualcomm AI Hub) |

Con un motor elegido a mano se usa su modelo activo; si no hay ninguno de su formato, el selector lo dice. En la
vista **Modelos**, el filtro **Formato** enseña solo los modelos de un motor. Terminal:
`nexus-office run --motor litert --acelerador npu`, `nexus-office run --motor onnx --runtime cuda`,
`nexus-office run --motor llamaserver --runtime vulkan`. Configuración: `llm.engine`, `llm.motor_acelerador`,
`hardware.llama_variant_manual`.

La lista completa de motores por hardware, con lo que no se puede incluir y por qué, está en el CHANGELOG (2.8.0).

### LiteRT-LM y ONNX Runtime GenAI (2.5)
Son motores para los equipos y móviles donde llama.cpp no aprovecha el acelerador: los **Pixel** (Tensor), los
**Snapdragon 8 Elite** y **Dimensity** con modelos `.litertlm` compilados para su NPU, y los portátiles **Snapdragon X
Elite/Plus** con modelos ONNX para QNN. Se eligen solos por el formato del modelo: un `.litertlm` va a LiteRT-LM y
una carpeta con `genai_config.json` a ONNX Runtime GenAI; los GGUF siguen en llama-server.

- Cada motor corre en un proceso aparte (`nexus-office motor litert|onnx ...`) con la misma API que llama-server. Si
  el driver de la NPU o de la GPU falla, cae ese proceso y no Agente IA, y se prueba el siguiente acelerador:
  **NPU → GPU → CPU**.
- LiteRT-LM solo intenta la NPU si el modelo está compilado para ese SoC (lleva `sm8750`, `mt6991`, `tensor_g4`... en
  el nombre) y el equipo tiene NPU; si no, GPU y CPU. ONNX Runtime GenAI usa el proveedor para el que se exportó el
  modelo y, si falla, la CPU. `llm.motor_acelerador` fuerza uno (`cpu`, `gpu`, `npu`, `dml`, `qnn`...).
- Sus bibliotecas (`liblitert-lm.so`, `onnxruntime-genai.dll`...) no se enlazan al compilar: van en
  `bundled/runtimes/litert|onnx/` y se cargan al ejecutar. En *Sistema → Runtimes* se ve si están (`motores`).
- Aún no admiten imágenes ni herramientas: el chat se manda como texto.

Ollama ya no aparece en el selector porque es otra aplicación que hay que instalar aparte y no viene en el paquete.
Si lo tienes instalado, Agente IA lo sigue usando cuando no hay otro motor.

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
  - **Snapdragon X Elite/Plus en Windows ARM**: ONNX Runtime GenAI con **QNN** en la NPU Hexagon (modelos ONNX
    exportados para QNN); los GGUF van a la GPU Adreno por OpenCL.
  - **Google Tensor, Snapdragon 8 Gen 2 o posterior y Dimensity** (Android, 2.8): LiteRT-LM con los modelos
    `.litertlm` de su SoC. La APK trae los *dispatch* de NPU de Google Tensor, Qualcomm y MediaTek y las bibliotecas
    QNN HTP; la NPU del Pixel usa además la biblioteca del sistema (`libedgetpu_litert.so`) y la de MediaTek el
    NeuroPilot del móvil. Si faltan, la GPU del móvil.
  - **Qualcomm Genie** (2.8): modelos de Qualcomm AI Hub en la NPU Hexagon (Android, Windows ARM, Linux ARM).
  - **AMD Ryzen AI y Apple Neural Engine**: llama.cpp no puede ejecutar ahí el modelo de texto. El selector lo dice y
    usa la GPU (o la CPU).

### Qué variante se usa en cada GPU (2.5)
- **NVIDIA**: CUDA 12; las RTX 50 (Blackwell) y posteriores, CUDA 13 si está.
- **AMD dedicada**: ROCm en Linux, Vulkan en Windows (y la otra si falla).
- **AMD integrada** (Radeon 680M/780M/890M, Strix Halo 8050S/8060S) con 16 GB o más: Vulkan con la memoria
  compartida; con menos, la CPU.
- **Intel Arc**: Vulkan y SYCL; **Intel integrada**: CPU, OpenVINO y Vulkan.
- **Qualcomm Adreno**: OpenCL en Windows ARM y el paquete Snapdragon (CPU, Adreno y Hexagon) en Linux ARM.
- **Apple Silicon**: Metal.

### Ajustes automáticos de rendimiento (2.5)
- Con poca memoria para el modelo, la caché KV va cuantizada a **q8_0** (la mitad de memoria); si esa build de
  llama-server no lo admite, se vuelve a arrancar sin ello.
- Hilos para leer el prompt (`-tb`) aparte de los de generar.
- Modelos de imagen: atención rápida, codificadores en la CPU y pesos por partes cuando no caben en la VRAM, VAE por
  teselas con poca VRAM o imágenes grandes, y **sd-server** con el modelo cargado entre imagen e imagen (se libera
  tras `media.sd_servidor_minutos`, 10 por defecto).

### LLM e imagen en la misma GPU (2.7)
- Desde la 2.6 el LLM carga con `-ngl auto`, que llena la VRAM. Al pedir una imagen, sd.cpp se quedaba sin memoria,
  repetía en la CPU (minutos, a veces pasado el tiempo de la herramienta) o acababa en la imagen compuesta sin IA.
- Ahora el prompt se mejora y traduce con el LLM **antes**; después, si el modelo de imagen (difusión, VAE,
  codificadores y memoria de trabajo) no cabe junto al LLM, el LLM **presta la GPU**: se para llama-server, se genera
  la imagen, el sd-server suelta la VRAM y el LLM vuelve a cargarse en la siguiente respuesta. Las peticiones al LLM
  local que lleguen mientras tanto esperan (máximo `media.gpu_espera_max`, 1800 s). Si caben los dos, siguen los dos
  y sd.cpp reparte con la VRAM que deja el LLM. Si sd.cpp aún así se queda sin memoria, se presta la GPU y se repite
  en la GPU antes de pasar a la CPU.
- Cuando llama-server vuelve a la GPU, el sd-server que esté parado sin uso se cierra para dejarle la VRAM.
- El codificador de texto LLM de Z-Image (Qwen3-4B), Qwen-Image (Qwen2.5-VL-7B) y FLUX.2 (klein: Qwen3-4B/8B;
  dev: Mistral Small 3.2) se comprueba en la cabecera GGUF (familia y tamaño oculto). Un LLM del chat que no encaja
  (p. ej. Qwen3-8B para Z-Image) ya no se pasa a sd.cpp; el aviso dice cuál falta. Un Qwen3-4B del chat con otro
  nombre sí se aprovecha.

### Correcciones de velocidad (2.6)
- **GPU dedicada**: llama-server arranca con `-ngl auto` en lugar de 999 capas fijas. llama.cpp (`--fit`) mete en
  la VRAM todas las capas que caben con el contexto pedido. Antes, si el modelo y la caché no cabían, CUDA en Linux
  fallaba y **todo** pasaba a la CPU, y en Windows el driver desbordaba la VRAM a la RAM compartida (muchas veces más
  lento). Si el llama-server es anterior a `auto`, se vuelve a 999 solo.
- El autoajuste ya no baja a 28 o 24 capas tras un pico de VRAM (se quedaba así para siempre) y descarta ese número
  si lo guardó una versión anterior. Tampoco agranda el contexto mirando solo la RAM cuando la caché va en la VRAM.
- Si la GPU elegida no arranca, el paso a la CPU se queda apuntado: antes cada respuesta volvía a probar la GPU y
  cargaba el modelo dos veces.
- **Móvil (GPU y NPU)**: un solo slot de llama-server, así la GPU o la NPU trabajan con todo el contexto y no con la
  mitad. Si la sonda solo ve la NPU Hexagon (sin nombre de GPU), se usa igualmente.
- **ARM** (Snapdragon X, Raspberry Pi, móviles): se cuentan todos los núcleos para los hilos. Antes un Snapdragon X
  de 12 núcleos se contaba como de 4.

### Rendimiento y autoajuste adaptativo (3.2.2)
- **Lote físico de la GPU** (`-ub`, `hardware` → perfil `llm.n_ubatch`): con una gráfica dedicada de 12 GB o más el
  prompt se procesa en trozos de 1024 tokens (antes 512). El prompt de los agentes es lo que más tarda en la GPU.
- **Caché de prompts en la RAM** (`--cache-ram`, perfil `llm.cache_ram_mb`): llama-server guarda por defecto hasta
  8 GB de prompts en la RAM; ahora se limita al 10 % de la RAM (5 % en el móvil, la mitad si la memoria va justa),
  entre 256 MB y 8 GB. En móviles y PC de 8-16 GB llenaba la memoria y el sistema tiraba del disco o mataba la app.
- **Atención rápida**: los llama-server antiguos (opción `-fa` sin valor) la reciben cuando la caché KV va en 8 bits o
  en CUDA/Metal; los recientes ya la ponen en `auto`.
- **Embeddings**: el servidor de embeddings atiende 4 secuencias a la vez (2 en el móvil). Antes, con un solo slot,
  cada fragmento de la Biblioteca RAG se calculaba de uno en uno.
- **Monitor**: con AMD en Linux la VRAM se lee de sysfs (antes el autoajuste no veía la VRAM de AMD). nvidia-smi se
  lanza en cada muestra solo mientras el modelo trabaja; en reposo, cada 10 s (deja a la gráfica bajar de consumo).
- **Autoajuste adaptativo**, en cada arranque y sobre lo medido en la sesión anterior:
  - VRAM por encima del 92 %: primero la caché KV en 8 bits, después el lote físico a la mitad, y solo entonces menos
    capas. Así el modelo sigue entero en la GPU.
  - RAM por encima del 92 % con el modelo en la CPU: caché KV en 8 bits antes de recortar el contexto.
  - RAM por encima del 90 %: caché de prompts a la mitad; por debajo del 75 %, se recupera.
  - Primera palabra lenta (más de 2,5 s de media) con la VRAM por debajo del 70 %: lote físico al doble (hasta 2048).
  - Si llama-server metió el modelo entero en la GPU o la NPU (`offloaded N/N`), no se prueban hilos de la CPU (no
    cambian la velocidad y cada prueba cambiaba la configuración).
  - Con CPU/GPU/NPU elegido a mano (`hardware.accel`) el autoajuste también se aplica: antes se calculaba sobre el
    perfil automático y se descartaba. Al cambiar el acelerador o la GPU sin reiniciar se conserva lo aprendido.

## Cómo se comprueba que acelera de verdad
Tras arrancar llama-server, Agente IA lee su registro (`logs/llama-server.log`) y anota el dispositivo real: capas
cargadas en la GPU, dispositivo CUDA, Vulkan o Metal, la NPU Hexagon u OpenVINO. Si lo pedido no se ha cumplido
(por ejemplo, falta el driver de la GPU), lo dice en el selector y en el registro, y el modelo sigue en lo que sí
funciona.

## Runtimes incluidos por instalador

| Instalador | llama-server | Otros motores | NPU |
|---|---|---|---|
| Windows x64 | CPU, Vulkan, CUDA 12 | LiteRT-LM, ONNX Runtime GenAI (DirectML) | OpenVINO (Intel AI Boost) |
| Windows ARM64 | CPU, Vulkan, OpenCL (Adreno) | ONNX Runtime GenAI (QNN) | Hexagon (QNN) |
| Linux x86_64 (.deb, .run, AppImage, Flatpak) | CPU, Vulkan, CUDA 12 | LiteRT-LM, ONNX Runtime GenAI | OpenVINO (Intel AI Boost) |
| Linux ARM64 y Raspberry Pi | CPU, Vulkan, CUDA (Jetson), Snapdragon | LiteRT-LM, ONNX Runtime GenAI | Hexagon (Snapdragon) |
| macOS Apple Silicon | CPU, Metal | LiteRT-LM, ONNX Runtime GenAI | — |
| macOS Intel | CPU | — | — |
| Android autónoma | CPU, OpenCL/Vulkan | LiteRT-LM | Hexagon HTP |

Las variantes que pesan demasiado para ir dentro del instalador (**CUDA 13**, **ROCm** y **SYCL**) se publican en la
Release como paquetes aparte, `Nexus-Office-runtime-<variante>-<versión>-<plataforma>.zip`. Para usarlos sin
internet, deja el zip junto al programa (o en su carpeta superior, o en `data/paquetes/`) y se importa solo al
arrancar; también con `nexus-office importar-runtime <zip>` o `POST /api/runtimes/importar {"ruta": ...}`. Con
internet, *Sistema → Runtimes* los descarga de las Releases oficiales de llama.cpp.

## Configuración
- `llm.engine`: `auto` o `llamaserver` (`llamacpp` se acepta por compatibilidad con la edición Python, pero no está disponible).
- `hardware.accel`: `auto`, `cpu`, `gpu` o `npu`.
- `hardware.capas`: capas del modelo en la GPU o la NPU: `auto`, `todas`, `ninguna` o un número (2.6). También en
  `POST /api/accel {"capas": 24}`.
- `hardware.llama_variant`: variante de llama-server elegida (`cpu`, `vulkan`, `cuda`, `cuda13`, `rocm`, `sycl`,
  `metal`, `openvino`, `opencl-adreno`, `snapdragon`).
- `llm.motor_acelerador`: acelerador de LiteRT-LM u ONNX Runtime GenAI que se prueba primero.
- `media.sd_servidor` (`no` para lanzar sd-cli en cada imagen) y `media.sd_servidor_minutos`.
- `media.gpu_compartida`: `auto` (el LLM presta la GPU solo si no caben los dos), `siempre` o `no`; y
  `media.gpu_espera_max` (segundos que espera el LLM a que vuelva la GPU).
- `startup.ask_accel`: `true` para preguntar al arrancar.

API: `GET /api/accel` (opciones, elección y dispositivo efectivo), `POST /api/accel` `{engine, accel, remember}`,
`POST /api/accel/test` (genera 16 tokens y mide).
