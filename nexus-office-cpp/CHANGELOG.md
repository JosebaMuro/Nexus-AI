# Cambios

## Agente IA 3.2.1-cpp

Arreglos de velocidad con NPU y con graficas dedicadas (NVIDIA, AMD, Intel Arc). Se revisaron la eleccion del motor,
las capas en la GPU, el autoajuste, las tareas de fondo y los runtimes que van en los instaladores.

### Movil (NPU Hexagon y GPU)
- El autoajuste ya no da **tres respuestas en la CPU a proposito** para comparar: si la NPU o la GPU van a 8 tokens/s
  o mas (contando el prompt), se quedan sin probar la CPU. Antes cada comparacion recargaba el modelo dos veces.
- La comparacion GPU/CPU mide la **respuesta entera** (prompt incluido) y la CPU solo gana si es un 15 % mas rapida.
  Midiendo solo la generacion, la CPU podia ganar y el modelo se quedaba en la CPU para siempre, aunque la NPU
  procese el prompt (lo que mas pesa con los agentes) muchas veces mas rapido.
- Con el modelo en la NPU o la GPU **no se exploran los hilos**: cada cambio de hilos reiniciaba llama-server
  (recarga del modelo y cache del prompt perdida) para una diferencia que con el modelo en el acelerador no existe.

### Escritorio (NVIDIA, AMD, Intel)
- Si la GPU falla al arrancar o llama-server se cae, se usa la CPU **solo 10 minutos** y despues se vuelve a probar
  la GPU. Antes un fallo pasajero (VRAM ocupada por la imagen, un modelo grande que tardaba en cargar) dejaba todo
  en la CPU hasta reiniciar Agente IA, y una caida con dos GPU marcaba la grafica como rota con la logica del movil.
- Con una sola GPU, un fallo al arrancar en la GPU tambien pasa a la CPU (antes la peticion acababa en error).
- La carga del modelo espera hasta 15 minutos mientras llama-server siga avanzando (antes cortaba a los 5 y pasaba a
  la CPU con modelos grandes en discos lentos o al compilar por primera vez los kernels de la GPU).

### Tareas de fondo
- La **reunion automatica** de agentes (cada 8 tareas) espera a que el modelo este libre y cede el turno si llega una
  peticion del usuario. Antes sus frases ocupaban la GPU o la NPU mientras el usuario esperaba.
- El aprendizaje en segundo plano no razona (hasta 768 tokens menos por llamada).

### Generador de video
- **Mejorar imagenes**: niveles automaticos, color, nitidez y ampliacion hasta x2 de las imagenes mas pequenas que el
  video (✨ en cada una, o todas a la vez; se puede deshacer y activar al anadir).
- **Hojas con varias diapositivas**: al anadir una imagen con varias diapositivas en cuadricula se recortan y se ordenan
  en orden de lectura (con Deshacer); ✂ en una imagen pide filas x columnas si no ve las separaciones.
- El movimiento lento ya no deja una banda negra en el borde al empezar cada imagen.

### Runtimes de la entrega
- Windows x64: llama.cpp **CUDA 12** y **Vulkan** de la misma build oficial que el resto (los de la entrega anterior
  eran una build de llama.cpp miles de versiones por detras, con un Vulkan mucho mas lento en AMD e Intel).
- Linux x86_64: llama.cpp **Vulkan** oficial reciente (AMD sin el paquete ROCm, Intel Arc).
- Paquete ONNX CUDA de Windows: faltaba `cublas64_12.dll` (la ultima nvidia-cublas-cu12 ya no publica wheel de
  Windows) y el proveedor CUDA no cargaba: los modelos ONNX iban en la CPU. Se fija la 12.9.2.10.

## Agente IA 3.2.0-cpp

Motores del dossier de arquitectura y todo lo que mejora el rendimiento, en una sola version (incluye la 3.1.0, que
no se publico aparte). Tambien MCP y un codificador de video en el navegador.

### Motor de difusion propio sobre ONNX Runtime
- Si el modelo de imagen es una carpeta ONNX (diffusers/optimum, como SD-Turbo), Agente IA ya no usa
  stable-diffusion.cpp: genera **dentro del proceso** con ONNX Runtime (tokenizador CLIP, codificador de texto, bucle
  UNet con el planificador Euler y VAE). Cada paso publica su vista previa directamente en el buffer de pantalla.
- Cada red va en el proveedor del chip: **QNN** (NPU Hexagon de Snapdragon, con cache del contexto compilado),
  **TensorRT/CUDA** (NVIDIA), **ROCm** o **DirectML** (AMD), **OpenVINO** (Intel NPU/GPU), **DirectML** (cualquier GPU
  en Windows), **CoreML** (Apple), **NNAPI** o **XNNPACK** (Android sin QNN) y CPU. Si uno falla se prueba el siguiente.
- Modelo nuevo en el catalogo: **SD-Turbo ONNX** (una imagen en 1 a 4 pasos).

### Memoria del acelerador y nucleos grandes
- El asignador zero-copy pide la memoria que mejor ve el acelerador: **dma-buf** (rpcmem de Qualcomm o
  `/dev/dma_heap`), **CUDA pinned** o **memoria registrada de HIP**; si no hay, memoria compartida y, al final, normal.
- Los hilos de calculo (difusion, percepcion, llama-server con `--cpu-mask`) se fijan a los **nucleos grandes** de
  CPUs big.LITTLE, Intel con nucleos P/E y Apple (cpufreq, CPU sets de Windows y QoS de macOS).

### Motores LLM dentro del proceso
- LiteRT-LM, ONNX Runtime GenAI y Qualcomm Genie corren **dentro de Agente IA** (sin HTTP ni copias entre procesos).
  Si uno se cae, queda marcado y la siguiente vez se usa en su proceso aparte, como antes (`motores.en_proceso`;
  `POST /api/hal/caidas/olvidar` lo reintenta).
- **Matriz de motores por chip** (Snapdragon, Tensor, Exynos, Dimensity, Apple, NVIDIA, AMD, Intel...): que motor y
  proveedor va primero para el LLM, la difusion y la vision. En Modelos, la etiqueta «para tu chip».

### Percepcion con CLIP y SigLIP
- Codificador de vision en ONNX Runtime (**CLIP** o **SigLIP**, modelos nuevos en el catalogo): cada frame se guarda
  tambien como vector de imagen y, con CLIP, la busqueda en lo percibido compara el texto directamente con las imagenes.

### MCP (Model Context Protocol)
- **Servidor**: las herramientas de Agente IA para otros programas (asistentes de escritorio, editores, otros agentes)
  por `POST /mcp` o por stdio con `nexus-office mcp`. Cada llamada pasa por el supervisor: misma politica por riesgo,
  aprobaciones y auditoria. Fuera del equipo, clave de red o token `mcp.token`.
- **Cliente**: los agentes usan herramientas de servidores MCP externos (`mcp.servidores`, por stdio o HTTP), que
  aparecen como `mcp_<servidor>_<nombre>` con su riesgo. Panel **MCP** en Herramientas.

### Voces de los agentes
- **Editor de voces** en Ajustes (la voz general) y en la configuracion de cada agente (su voz propia): motor (Piper,
  voz del sistema o del navegador), modelo Piper y hablante, voz del sistema (y variante en espeak), voz del navegador,
  velocidad, tono y volumen, con boton para probarla.
- Para **reconocer quien habla**, cada agente sin voz propia usa la general con un tono distinto (y otro hablante si el
  modelo Piper tiene varios); se puede desactivar. Funciona igual con Piper, la voz del sistema, el navegador y la app
  de Android. `POST /api/voice/tts` acepta `agente` o `voz`; `GET /api/voice/voces` lista las voces del equipo.

### Video con imagenes
- En el Estudio, **Video con imagenes**: una serie de imagenes (o de la galeria) a un video WebM con fundidos, Ken
  Burns o cortes, codificado en el navegador con WebCodecs (VP9/VP8 con aceleracion por hardware) y sin conexion.
  Se descarga o se guarda en Agente IA (`POST /api/media/video/subir`).

### Notas
- La APK va firmada con la clave de siempre (Nexus): si tienes instalada la 3.0.0, desinstalala antes de instalar esta.

## Agente IA 3.1.0-cpp

Adapta el runtime al **dossier de arquitectura** (agente de IA unificado para dispositivos moviles): tres capas,
compilacion cruzada con CMake 3.28, memoria zero-copy con colas lock-free y pipelines desacoplados de percepcion,
razonamiento y difusion.

### Tres capas
- **Orquestacion multiagente** (`agents/`, `tools/`, `office/`), **motor core multiplataforma** (`llm/`, `memory/`,
  `rag/`, `media/`, `pipeline/`, `server/`) y **abstraccion de hardware** (`hal/` nueva y `hardware/`). Las capas de
  arriba piden memoria y colas a `hal/` y el dispositivo al perfil; ningun modulo de arriba conoce un SDK de acelerador.

### Capa de abstraccion de hardware (`src/hal/`)
- **Asignador zero-copy**: bloques alineados a pagina (lo que piden Vulkan, OpenCL, CUDA y el DMA de las NPU para usar
  memoria de la CPU sin copiarla), respaldados por memoria compartida (memfd en Linux y Android, CreateFileMapping en
  Windows) con su descriptor para pasarlos a un proceso hijo, referencias contadas y pools que reciclan bloques con una
  lista libre lock-free. El ultimo frame de la pantalla sale por HTTP directamente del bloque, sin copiarlo.
- **Buffers circulares lock-free** con atomicos: SPSC (un productor, un consumidor), MPMC (cola de Vyukov) y triple
  buffer para el ultimo frame. Paso entre hilos medido: **~0,3 µs** de ida (`GET /api/hal/latencia`).

### Pipelines desacoplados (`src/pipeline/`)
- **Percepcion**: los frames (camara del Estudio o `POST /api/pipeline/frame`) entran por un ring y un hilo propio los
  describe con la red de vision en la GPU/NPU (llama-server con mmproj) o, sin ella, con estadisticas y una huella
  visual; descarta los repetidos y guarda descripcion, embedding y huella en la **base vectorial local** (`percepcion`).
  El orquestador la consulta con la herramienta nueva `percepcion_buscar`.
- **Orquestador -> difusion**: `generate_image` ya no genera dentro del agente: envia un **descriptor de trabajo** a la
  cola del motor de difusion (con `asincrona: true` ni siquiera espera). El LLM sigue con su cuantizacion mixta (pesos
  GGUF Q4_K y cache KV en q8_0).
- **Difusion asincrona al buffer de pantalla**: un hilo propio ejecuta sd.cpp en la NPU/GPU; con sd-cli reciente publica
  la vista previa de cada paso, y al final la imagen, en el buffer de pantalla (triple buffer). Un presentador avisa por
  WebSocket y la interfaz lo pinta en un `OffscreenCanvas` dentro de un Web Worker, fuera del hilo principal.
- En el Estudio: paneles **Pantalla de difusion** (encargar sin esperar, ver cada paso) y **Percepcion** (camara,
  busqueda en lo percibido).

### Compilacion cruzada
- CMake 3.28 y `CMakePresets.json` con presets por plataforma: `linux` (ELF autocontenido), `windows` y `windows-dll`
  (exe + `nexus_core.dll`, con el runtime de MSVC al lado), `android` y `android-so` (`libnexus_core.so` para la APK,
  con `c++_shared`), y variantes con SDK.
- Opciones `NEXUS_CORE_SHARED` y `NEXUS_SDK_CUDA|VULKAN|OPENCL|QNN|NNAPI|DIRECTML|OPENVINO`, que activan cada SDK de
  hardware solo si se pide (por defecto ninguno: los motores se cargan en tiempo de ejecucion y el binario vale para
  cualquier equipo). `GET /api/hal` dice que SDK lleva la compilacion y que artefacto es.
- La entrega no cambia de forma: los paquetes siguen siendo el ejecutable autocontenido de siempre.

## Agente IA 3.0.0-cpp

Version mayor de **Agente IA** (antes Nexus-Office), con el nombre nuevo en todo lo que ve el usuario (interfaz, instaladores, manuales, web de
descarga y app de Android, con identificador `ai.agente.ia`). Reune en una sola version estable todo lo de la 2.10.0 y la
2.10.0-beta.1 (mas abajo): Laboratorio BETA de rendimiento y aspecto, imagenes con sd.cpp dentro de la APK, modelos
GGUF ligeros, generador de video y un asistente de agentes nuevo, mas sencillo.

### Asistente de configuración de agentes
- **✨ Asistente de agentes** en *Agentes*: crea o ajusta un agente («empleado») en cuatro pasos sencillos (sustituye al de siete pasos de la 2.10.0): puesto
  (más de 70 plantillas con buscador, o desde cero), forma de ser (trato, extensión, creatividad, iniciativa, idioma y
  reglas propias con un clic), qué sabe y qué hace (especialidad, palabras clave, paquetes de herramientas en lenguaje
  sencillo, equipo, modelo y memoria) y revisar (ficha, avisos e instrucciones redactadas, que se pueden retocar).
- Al guardar recibe el conocimiento de su oficio en su grafo y se abre el chat para probarlo. Lo elegido se guarda
  con el agente (`perfil`) para volver a abrir el asistente igual. El editor avanzado sigue disponible.
- Nueva API `GET/POST /api/agents/asistente`; la ficha y el prompt se redactan en C++ (`src/agents/asistente.cpp`).

## Agente IA 2.10.0-cpp

Version estable de la 2.10 (incluye todo lo de la 2.10.0-beta.1, mas abajo). La APK (versionCode 21001) va firmada con
la clave de Nexus, no con la de la beta.1: si tienes la beta.1 instalada, desinstalala antes de instalar esta.

### Imagenes en el movil: sd.cpp viene dentro de la APK
- **Arreglado: «no se puede instalar sd.cpp» en Snapdragon 8 (y en cualquier Android).** El instalador descargaba los
  binarios de Linux de escritorio, que Android no deja ejecutar, y la generacion de imagenes caia siempre al compositor
  de escenas. Ahora stable-diffusion.cpp se compila con el NDK y viaja en la APK en dos variantes: **GPU Vulkan**
  (Adreno, Mali, Xclipse) y **CPU** (ARMv8.2 con dotprod). «Instalar» en el movil ya no descarga nada.
- Si la GPU falla al generar (driver Vulkan inestable), se recuerda y la siguiente imagen va a la CPU sola.
- Windows en ARM usa la version x64 de sd.cpp (emulada), que si existe.
- Se usa `sd-server` junto a `sd-cli` en todas las plataformas (el modelo queda cargado entre imagenes).

### Texto a imagen con modelos GGUF ligeros
- Nuevos recomendados: **LCM SSD-1B Q4_K** (≈1 GB, 4 pasos, apto para movil) y **FLUX.2 klein 4B Q4_0** (calidad alta
  con 12 GB de RAM o 6 GB de VRAM).
- **TAESD**: decodificador diminuto que se usa solo cuando la VAE completa seria lenta (CPU, movil o poca VRAM);
  la imagen sale varias veces mas rapido. SDXS, SD-Turbo y SDXL-Turbo lo traen como componente.
- En el movil la pestaña *Recomendados* ya muestra los modelos de imagen marcados como aptos.

### Generador de video
- **Editor de video en HTML** (*Estudio → Generador de video* o `/static/video.html`): secuencia de imagenes con orden
  por arrastre, duracion por imagen, transiciones (fundido, a negro, deslizar, zoom, cortinilla, corte), movimiento
  Ken Burns, rotulos, tarjeta de titulo y musica. Exporta a MP4 o WebM en el propio navegador, sin conexion, y lo
  guarda en la galeria.
- **Herramienta `crear_video` para los agentes** (artista, diseñador grafico y comercial): monta un AVI (Motion JPEG)
  con las ultimas imagenes generadas o las que se le indiquen, en 16:9, 9:16, 1:1 o 4:5, y deja el proyecto listo para
  retocarlo en el editor. Tambien desde el boton *Video rapido con las ultimas* del Estudio (`POST /api/video/rapido`).

### Asistente visual de agentes
- **✨ Asistente de agentes** en la vista *Agentes*: siete pasos (plantilla, identidad, personalidad, conocimiento,
  herramientas, equipo y revision) con diez plantillas de oficina, iconos y colores, controles de tono, creatividad e
  iniciativa, rasgos, reglas, palabras clave, herramientas agrupadas por riesgo, modelo y delegacion. Una tarjeta
  en vivo con un radar de personalidad muestra el resultado, y el prompt se genera solo (editable).
- Cada agente tiene boton *Configurar* para abrirlo en el asistente; el editor avanzado sigue disponible.

### APK
- **Compartir con Agente IA** desde cualquier app (galeria, navegador, correo...): el texto va al chat y los ficheros a
  la carpeta de trabajo de los agentes (`compartido/`), listos para leerlos, retocarlos o montar un video.

## Agente IA 2.10.0-beta.1-cpp

**Nuevo nombre: Agente IA** (antes Nexus-Office). Cambia el nombre visible en la interfaz, los instaladores, los manuales, la web
de descarga y la app de Android. La APK tiene identificador nuevo (`ai.agente.ia`) y se firma con la clave «Agente IA», asi
que se instala junto a la anterior y no encima. El codigo interno,
los nombres de fichero y las rutas de instalacion no cambian, y los datos de las instalaciones de escritorio siguen sirviendo.

Incluye todo lo de la 2.9.0 y la 2.8.0 (más abajo).

Versión beta para probar el rendimiento y el aspecto. Detalles en [docs/BETA.md](docs/BETA.md).

### Agentes más seguros, que aprenden tu estilo y se miden (ver docs/AGENTES.md)
- **Contenido no fiable delimitado:** lo que traen la web, los documentos, los ficheros y la memoria entra al modelo
  marcado como datos de terceros, con las etiquetas del protocolo neutralizadas y un detector de frases de inyección
  (aviso en la interfaz).
- **Contaminación por turno:** tras leer contenido externo, guardar en memoria, escribir ficheros, ejecutar código u
  operar piden permiso, con el motivo en la tarjeta. `memory_save` en ese turno guarda con menos peso y sin crear pautas.
- **Mínimo privilegio:** el agente generalista ya no tiene herramientas de alto riesgo (delega en el especialista);
  las listas de herramientas admiten exclusiones (`"!web_fetch"`, `"!riesgo:high"`).
- **Aprobaciones que se reanudan:** si nadie responde (10 minutos), la acción queda pendiente, sobrevive a un reinicio
  y, al aprobarla, el agente termina la tarea.
- **Estilo de cada usuario:** tono, formalidad, longitud, formato, nivel técnico, emojis y rutinas de trabajo
  aprendidos de las correcciones, las valoraciones y la forma de escribir de cada persona (cada móvil emparejado o
  nombre de perfil), visibles y ajustables en *Herramientas → Mi estilo*.
- **Métricas clave por tarea:** tasa de éxito, latencia y coste, escalado humano, precisión de las herramientas y
  adaptación al estilo, en *Herramientas → Métricas clave* y `GET /api/metricas`.
- **Banco de evaluación:** `nexus-office evaluar` (rápido sin modelo, o `--completo` con el modelo activo y `--juez`).
- **Privacidad con el Profesor online:** DNI/NIE, CIF, IBAN, tarjetas, correos y teléfonos se ocultan antes de enviar
  y se restauran en la respuesta.

### Medir el rendimiento
- **Laboratorio BETA** en la interfaz: banco de pruebas de CPU (un hilo y todos), memoria, disco y la IA activa
  (primer token y tokens/s), con historial y diferencia con la medición anterior; exportación a JSON e informe en
  Markdown.
- Cada petición a la API se cronometra por ruta (p50, p95, máximo y errores) y lleva la cabecera `Server-Timing`; la
  interfaz compara el tiempo del núcleo con el que ve el navegador.
- Métricas del navegador (FCP, LCP, CLS, INP aproximado, tareas largas, memoria JS) y prueba de fluidez con los
  efectos completos y ligeros. HUD de rendimiento flotante con Alt+Mayús+P.
- Nueva orden `nexus-office bench` (`--rondas`, `--tokens`, `--sin-llm`, `--solo-llm`, `--json`, `--informe`).

### Evaluar el aspecto
- Vista previa de los temas sin guardarlos (y recorrido automático), efectos visuales completos, ligeros o
  automáticos por dispositivo, y disposición escritorio, tablet o móvil.
- Galería de componentes y comprobación de contraste WCAG 2.1 (AA/AAA) del tema.
- Opiniones de los probadores con nota por zona, guardadas con el tema, la pantalla y las métricas; resumen para el
  administrador.
- Etiqueta **BETA** junto al logo.
## Nexus-Office 2.9.0-cpp

### Igual que la edición Python, con todas las mejoras
Se ha comparado la edición C++ con la edición Python en marcha, las dos a la vez en el mismo equipo: las 201 rutas
de la API (mismas claves y tipos en las respuestas), las 12 vistas de la interfaz en capturas y los flujos de chat,
imagen, música, memoria y dispositivos. La C++ tenía ya todo lo de la Python salvo dos cosas, que llegan ahora:
- **Imagen sin modelo que sigue el prompt** (compositor de escenas, `src/media/scene.cpp`): sin modelo de imagen la
  edición C++ dibujaba siempre un campo de flujo abstracto que solo cambiaba de color. Ahora, como en la Python, lee
  el prompt en español o inglés y dibuja la hora (día, noche, atardecer, amanecer), el clima (lluvia, nieve, niebla,
  tormenta), el lugar (mar, playa, montaña, bosque, ciudad, espacio, desierto, lago, jardín, campo) y los sujetos
  (gato, perro, casa, castillo, árbol, flor, coche, barco, persona, astronauta, robot, faro...) con su color y
  cuántos hay. El arte abstracto queda para cuando se pide («abstracto», «fractal», «patrón»), y si no reconoce nada
  del prompt lo dice en la nota.
- **APK con Snapdragon cuyo backend Hexagon se cae**: si la sonda de llama-server con la NPU no devuelve nada, se
  repite sin la NPU (`GGML_HEXAGON_DEVICES=0`) para seguir usando la GPU, y la NPU queda marcada como no usable.
  La GPU ya no abre sesión en el DSP cuando el modelo no va en la NPU.

## Nexus-Office 2.8.0-cpp

### Todos los motores por hardware, elegibles a mano
- **NPU de Google Tensor (Pixel 6 a 10), Qualcomm Hexagon y MediaTek APU en Android**: LiteRT-LM se compila ahora
  desde el código fuente junto con sus *dispatch* de NPU (antes la APK traía el de PyPI, que no los tiene y solo
  usaba GPU y CPU), con las bibliotecas QNN HTP del QAIRT de Qualcomm y las de GPU de LiteRT-LM.
- **Qualcomm Genie** (motor nuevo, `genie`): modelos de Qualcomm AI Hub (carpeta con `genie_config.json` y binarios
  QNN) en la NPU Hexagon. Android, Windows ARM (Snapdragon X) y Linux ARM.
- **NVIDIA en Linux ARM** (DGX Spark, GH200, Jetson Thor y Orin con JetPack 7): llama.cpp con CUDA 13 compilado para
  ARM64, en el paquete aparte `Nexus-Office-runtime-cuda-…-linux-aarch64.zip`.
- **ONNX Runtime GenAI con CUDA** (Windows x64 y Linux x64, paquete aparte `runtime-onnx-cuda`) y con **OpenVINO**
  (NPU, GPU y CPU de Intel). Cada proveedor va en su carpeta y se carga el del acelerador elegido.
- **Intel NPU con LiteRT-LM** (Windows x64, modelos `.litertlm` de Lunar Lake y Panther Lake) si el *dispatch* de
  OpenVINO compila; **Huawei Ascend (CANN)**, **WebGPU** y **llama.cpp Hexagon en Windows ARM** cuando ggml-org
  publica esas builds.
- **Selector MOTOR Y ACELERADOR**: se elige el motor (llama-server, LiteRT-LM, ONNX Runtime GenAI, Genie) y, además
  de CPU/GPU/NPU, el **runtime concreto**: CUDA 12/13, ROCm, Vulkan, SYCL, OpenVINO, Metal, OpenCL Adreno, Hexagon,
  NPU de LiteRT-LM, CUDA/DirectML/QNN/OpenVINO/VitisAI de ONNX. Lo que no va en el paquete de ese sistema sale
  desactivado con el motivo. Terminal: `nexus-office run --motor onnx --runtime cuda`.
- Con un motor elegido a mano se usa su modelo activo aunque el modelo por defecto sea un GGUF.

### Modelos: filtro por formato
- En **Modelos**, filtro **Formato** (GGUF, ONNX, LiteRT-LM/Tensor, Genie, safetensors): filtra los instalados, la
  búsqueda en Hugging Face (etiquetas `gguf`, `onnx`, `litert-lm`; Genie = repositorios de Qualcomm) y los ficheros
  de cada repositorio, y dice con qué motor se ejecuta ese formato.
- Las carpetas con `genie_config.json` se reconocen como modelos Genie al importarlas o descargarlas.
- **Descarga en el formato que acelera en tu equipo**: nuevos recomendados Gemma 4 E2B en LiteRT-LM (baja el
  `.litertlm` compilado para la NPU de tu chip: Tensor G5, SM8750, Dimensity, Intel; si no hay, el general para GPU
  y CPU) y Phi-4 mini en ONNX (baja solo la carpeta de tu acelerador: CUDA, DirectML, QNN o CPU). Cada tarjeta dice
  con qué motor va y si ese motor está en el paquete.
- **Listo para usar al descargar**: un modelo de LiteRT-LM, ONNX o Genie se activa al terminar y deja configurado su
  motor (`llm.engine`) con el acelerador automático (NPU, GPU, CPU). Al volver a un GGUF, el motor vuelve a
  Automático. Los modelos de Genie salen de Qualcomm AI Hub (con cuenta): se descargan allí y se importan.
- **Importar modelo con explorador de archivos**: *Modelos > Importar modelo local* tiene **Buscar archivo…** y
  **Carpeta…**. En el escritorio abre el selector de archivos del sistema (Windows, macOS, Linux con zenity o kdialog);
  desde el móvil, el acceso remoto o un servicio sin escritorio, un explorador propio que recorre las carpetas del
  equipo de Nexus-Office (unidades, Descargas, almacenamiento y tarjetas SD de Android), marca los ficheros y carpetas
  de modelo (GGUF, ONNX, LiteRT-LM, safetensors, ONNX GenAI, Genie) e importa al elegir.

### Lo que no se puede incluir
- NVIDIA TensorRT-LLM (motor compilado por modelo y tarjeta), AMD Ryzen AI NPU (su SDK no es redistribuible),
  Apple Neural Engine (no hay motor de LLM abierto), Samsung Exynos NPU (SDK solo para socios), Hailo (driver del
  kernel y modelos con cuenta) y Google Coral (8 MB de memoria).
- Nada de esto se ha probado en NPU ni en GPU reales: solo en la CI y en el emulador de Android.

## Nexus-Office 2.7.0-cpp

### IA especializada para la bolsa
- Tres modelos financieros GGUF en el catálogo de la oficina de bolsa, elegidos según el equipo: Finanzas 1.5B
  (móvil), Fin-R1 7B (PC) y Qwen3.8 27B Finanzas con visión (GPU de 24 GB).
- Los agentes de la mesa usan `model: especialidad:finanzas`: la IA financiera instalada que mejor va o, si no hay,
  la general (`ia_especializada.finanzas`: auto, un modelo o no).
- El agente autónomo pide a esa IA que revise cada compra (comprar o esperar, con su motivo en el diario); se apaga
  con «La IA financiera revisa cada compra» o `bolsa.agente.ia = false`.

### El LLM y el modelo de imagen se entienden
- Comparten la GPU: desde la 2.6 el LLM llenaba la VRAM y el modelo de imagen se quedaba sin memoria (minutos en la
  CPU o imagen sin IA). Ahora el prompt se prepara con el LLM antes y, si los dos no caben, el LLM deja la GPU
  mientras se genera la imagen y vuelve a cargarse después; si caben, siguen los dos. Si aún así falta memoria, se
  repite en la GPU antes de pasar a la CPU (`media.gpu_compartida`: auto, siempre o no).
- El LLM que usan Z-Image, Qwen-Image y FLUX.2 para leer el prompt se comprueba en la cabecera GGUF (familia y
  tamaño): un LLM del chat que no encaja ya no se usa, y el aviso dice cuál descargar.

## Nexus-Office 2.6.0-cpp

### Velocidad con GPU dedicada y con NPU
- llama-server arranca con `-ngl auto`: llama.cpp pone en la VRAM todas las capas que caben con el contexto. Con 999
  fijo, un modelo que no cabía hacía que CUDA fallase y todo fuera a la CPU (Linux) o que la VRAM se desbordase a
  la RAM compartida (Windows). Con un llama-server antiguo se vuelve a 999 solo.
- El autoajuste ya no deja el modelo en 28 o 24 capas para siempre tras un pico de VRAM, descarta ese valor si lo
  guardó una versión anterior y no agranda el contexto si la VRAM va llena.
- Si la GPU no arranca, el paso a la CPU se recuerda en la sesión (antes se recargaba el modelo dos veces en cada
  respuesta).
- Móvil: un solo slot de llama-server con GPU o NPU (todo el contexto para una conversación) y NPU Hexagon usable
  aunque la sonda no dé nombre de GPU.
- ARM: se cuentan todos los núcleos (un Snapdragon X de 12 núcleos se veía como de 4).

### Capas en la GPU / NPU
- Nueva opción en *MOTOR Y CPU/GPU/NPU* (y en *Ajustes → Administración*): automático, todas, ninguna o un número de
  capas. Se guarda en `hardware.capas` y la respeta también el autoajuste del móvil.

## Nexus-Office 2.5.0-cpp

### Motores para cada acelerador
- **LiteRT-LM** (Google): modelos `.litertlm` en CPU, GPU y NPU (Google Tensor, Snapdragon 8 Elite, Dimensity, Intel).
  Va también en la APK autónoma.
- **ONNX Runtime GenAI** (Microsoft): modelos ONNX con DirectML (cualquier GPU en Windows), QNN (NPU de los
  Snapdragon X en Windows ARM), OpenVINO, CUDA y CPU.
- Cada motor corre en un proceso aparte y prueba NPU → GPU → CPU; se elige solo por el formato del modelo.

### Runtimes por fabricante
- llama.cpp con **CUDA 13** para las RTX 50, **ROCm** (AMD), **SYCL** (Intel Arc), **OpenCL Adreno** (Snapdragon X
  en Windows) y el paquete **Snapdragon** de Linux ARM (CPU, Adreno y Hexagon). Los nombres de las Releases oficiales
  de 2026 ya se reconocen al descargar.
- Las variantes pesadas van en paquetes aparte de la Release (`Nexus-Office-runtime-*.zip`) que se importan sin
  conexión.
- Las APU de AMD con Radeon 680M/780M/890M o Strix Halo usan Vulkan con 16 GB o más.

### Imagen con GGUF
- Los GGUF de **Flux, Z-Image, SD3, Qwen-Image, Chroma**... se reconocen como modelos de imagen. Si son solo el
  modelo de difusión, Nexus-Office busca sus piezas (VAE, clip_l, clip_g, T5-XXL o el LLM codificador) y dice cuál
  falta. En *Modelos* hay **Z-Image-Turbo** y **FLUX.1-schnell** en GGUF que se descargan con todas sus piezas.
- **sd-server** mantiene el modelo cargado entre imágenes y stable-diffusion.cpp se actualiza en los instaladores.
- Ajustes según la VRAM: atención rápida, codificadores en la CPU, pesos por partes y VAE por teselas.
- Si la GPU no consigue generar la imagen (por ejemplo, Metal en una máquina virtual), se reintenta en la CPU
  antes de recurrir a la imagen compuesta sin IA.

### Rendimiento
- Caché KV cuantizada (q8_0) cuando el modelo va justo de memoria, con vuelta atrás si la build no lo admite.
- Hilos de lectura del prompt aparte de los de generación.

## Nexus-Office 2.4.0-cpp

### Conversaciones largas sin perder el hilo
- Cada conversación tiene un hilo: resumen progresivo de lo antiguo, notas fijas del proyecto y recuperación de los
  mensajes anteriores que vienen al caso. Antes solo se veían los 12 últimos mensajes y lo demás se perdía.
- El contexto se reparte en tokens según el `n_ctx` real del modelo y del equipo, no con un número fijo de mensajes.
- La petición actual ya no se puede descartar al ajustar el contexto (antes, con resultados de herramientas largos,
  podía quedarse fuera la propia pregunta).
- Con GPU, llama-server reparte el contexto entre dos conversaciones a la vez: el reparto usa ya lo que le toca a cada
  una (antes se contaba el doble y el servidor rechazaba la petición). Las definiciones de herramientas también cuentan,
  y si aun así el servidor dice que no cabe, se reajusta con sus cifras y se reintenta.
- Panel **🧵 HILO** en el chat y API `/api/sessions/{id}/hilo`. Las conversaciones se abren enteras (hasta 1000 mensajes).

## Nexus-Office 2.3.0-cpp

### Todo el proyecto en C++
- La edición C++ ya no depende de la carpeta de la edición Python: la interfaz web, los perfiles de oficina, los
  agentes, los paquetes de conocimiento y las tablas de datos están en `recursos/`, y la app Android en `android/`.
- **nexus-construir**, la herramienta de construcción, está en C++ y sustituye a todos los scripts de Python:
  instaladores (.exe, .run, .deb, AppImage, Flatpak y macOS), código fuente y sumas, APK autónoma, manuales en PDF,
  prueba de humo con modelos reales, grafo de ejemplo de la Wikipedia, tablas de datos y el avatar 3D a partir de su
  hoja de personaje. Ninguna Action de la edición C++ usa ya Python.
- El firmware del Pico W (NexusPico) está en C++ con el Pico SDK y la herramienta `flash.py` desaparece: todas las
  placas se graban con `nexus-office firmware` o desde *Dispositivos*.
- La edición Python queda congelada: no recibe cambios nuevos.

### Documentación
- La documentación técnica y el manual describen la edición C++: órdenes `nexus-office`, carpetas e identificadores
  propios (`Nexus-Office-C`, `nexus-office-c`), instaladores sin Python, servicio con `nexus-office-w.exe` en Windows,
  arquitectura por módulos de `src/` y empaquetado con `nexus-construir`.

### Correcciones
- Los paquetes ya no llevan las cabeceras ni las bibliotecas de mbedTLS (unos 4 MB menos por instalador).
- Las peticiones HTTP que responden 204 (sin contenido) ya no se toman por un fallo de conexión.
- La APK autónoma lleva un versionCode que sube con cada versión, así Android la instala encima de la anterior.
- Prueba nueva: los 55 ejemplos de formatos de grafo se leen en todas las plataformas.

## Nexus-Office 2.2.0-cpp

### Todo en C++
- Desde esta versión todo lo nuevo de Nexus-Office se hace en la edición C++.

### Comprobación y autorreparación al arrancar
- En cada arranque se revisan la configuración, la base de datos, los modelos, el motor de IA, el puerto, las carpetas,
  el disco, los registros y los drivers de las placas, y se arregla solo lo que se pueda: configuración desde la última
  buena, base de datos rescatada tabla a tabla con la copia diaria (`data/copias`, las tres últimas), GGUF dañados
  apartados, llama-server con permiso de ejecución, sin cuarentena en macOS y de la GPU a la CPU si la GPU no arranca, y
  el siguiente puerto libre si el 8775 está ocupado.
- Panel en *Sistema → Comprobación y autorreparación*, `/api/salud` y orden `nexus-office comprobar`.

### Más rápido y más pequeño
- Optimización en el enlace (LTO) en Windows, Linux y macOS: el ejecutable ocupa un 19 % menos.
- La revisión completa de la base de datos solo tras un cierre brusco y una vez al día; el resto de arranques, la rápida.

### Toda la familia ESP y drivers USB-serie
- *Dispositivos > Grabar firmware* reconoce y graba toda la familia ESP con Wi-Fi: ESP8266 (ESP-01, NodeMCU, Wemos
  D1 mini), ESP32, S2, S3, C3, C5 y C6. El C2 se reconoce y graba cualquier .bin, pero aún no hay NexusLED para él;
  el H2 y el P4 se reconocen, aunque no tienen Wi-Fi propio.
- Detecta los puentes CH340/CH9102, CP210x y FTDI y si les falta driver; **INSTALAR DRIVERS** (o
  `nexus-office firmware drivers --instalar`) lo instala sin preguntar: pnputil en Windows con los drivers del paquete,
  el instalador en macOS y, en Linux, módulo del kernel, regla udev, grupo `dialout` y `brltty` apartado.
- Orden nueva `nexus-office firmware` para ver placas, puertos y drivers y grabar desde la terminal.
- Grabador nativo **nexus-flash** dentro de Nexus-Office: habla directamente con la ROM de Espressif, verifica por MD5
  y reinicia la placa. Ya no hace falta esptool ni Python (`NEXUS_GRABADOR=esptool` lo sigue usando si se quiere).
- ESP8266 con imagen por tamaño de flash: 1 MB (ESP-01S y mayores), 4 MB y 512 KB (ESP-01 antiguo).

## Nexus-Office 2.1.0-cpp

### La edición C++, completa
- **Antes**: la 2.0 beta C dejaba fuera la mesa de bolsa, la red COOP, la VPN, el acceso remoto, la importación de
  grafos en otros formatos, el rastreo web, guardar una URL como PDF, las herramientas de GIMP, varias órdenes de la
  terminal y la GPU y la NPU en la APK.
- **Ahora** todo eso está en C++ con las mismas respuestas que la edición Python: 108 flujos más de la API comparados
  (bolsa, grafos, web, red, VPN y GIMP), 55 formatos de grafo leídos igual y 73 ediciones de imagen comparadas píxel a
  píxel. `/api/tools` lista las mismas herramientas en las dos ediciones.

### Agente Profesor con IA online
- Nuevo agente **Profesor** que enseña y resuelve dudas con una IA online: Google Gemini, ChatGPT, Perplexity, Claude,
  Mistral, DeepSeek, Groq, OpenRouter u otra compatible con OpenAI. Con Claude usa `claude-opus-5-5` con pensamiento
  adaptativo; con Perplexity añade las fuentes.
- Un administrador pone la clave en *Ajustes → Profesor* (o `PUT /api/nube`) y la prueba con `POST /api/nube/probar`.
  Las claves son secretas: nunca se devuelven.
- Sin clave responde con el modelo local. La IA online nunca se usa como respaldo automático de los demás agentes.
- Los agentes le preguntan con `consultar_profesor` y guardan la lección en su memoria: lo aprendido se queda en local.

### Android: GPU, NPU y autoajuste en vivo
- La APK autónoma C++ usa la **NPU Hexagon** (Snapdragon 8 Gen 2 o posteriores), la GPU (OpenCL en Adreno, Vulkan en
  el resto) o la CPU. Si la NPU no arranca se pasa a la GPU, y si falla la GPU, a la CPU.
- Autoajuste en vivo: mide la velocidad real con GPU y con CPU y se queda con lo más rápido, prueba varios números de
  hilos, usa menos con el móvil caliente o con poca batería, reduce el contexto si falta memoria y propone otro modelo
  si va lento o sobra velocidad. Tras cada arranque limpia temporales, optimiza la base de datos y precarga el modelo.

### Bolsa con datos reales y agente autónomo
- **Antes**: los precios de la mesa eran ficticios.
- **Ahora**, con conexión, son cotizaciones reales (Yahoo Finance, con CNBC, Google Finance, Stooq, Coinbase, Kraken y el BCE
  de respaldo) y el cambio de divisa es el real. Sin conexión la mesa sigue simulada desde el último precio real y con
  el histórico diario guardado. Sigue siendo siempre una simulación: nunca hay dinero ni órdenes reales.
- Nuevo **agente de bolsa autónomo** con su propia cartera simulada: analiza, arriesga como mucho el 1 % por
  operación, pone stops vivos que suben con el precio, vende en el objetivo y aprende de sus resultados. Diario, curva
  de patrimonio y gráficos de cada orden en vivo.

### Dispositivos
- Luces NexusLED, reloj NexusWatch y avatar NexusPico para el Pico W: estado de Nexus en directo, efectos, brillo y
  modos preconfigurados que se cambian desde el ordenador, el móvil o el reloj, que además muestra qué hace cada
  agente. Las imágenes de tinta del Pico W se generan en C++.
- Grabar el firmware por USB sin internet desde *Dispositivos > Grabar firmware*: los binarios y esptool vienen en el
  paquete, con puertos serie nativos en Windows, Linux y macOS.

### Perfiles de hardware y elección de GPU
- Perfiles con nombre (Automático, Rendimiento, Equilibrado, Modelo grande, Ahorro, Solo CPU y los tuyos) que juntan
  motor, acelerador, GPU, modelo, capas en la VRAM, caché de contexto en VRAM o RAM, `--mlock`, contexto, hilos y lote.
  Se eligen en la barra superior, en Sistema o con los modos de los dispositivos (`/api/hw/perfiles`).
- Con varias GPU, llama-server usa solo una: la elegida (`POST /api/hw/gpu`) o la dedicada con más memoria. Se leen
  sus dispositivos con `--list-devices` y nunca se usa un renderizador por software (llvmpipe). Si la GPU elegida no
  arranca, el modelo sigue en la CPU y la causa se ve en `/api/accel`.
- Bolsa: velas de CNBC (y de Coinbase o Kraken para cripto) cuando Yahoo limita, y cierre anterior deducido de la variación si falta.

### Avatares 3D gratuitos
- Biblioteca de avatares en *Ajustes > Avatar*: busca y descarga avatares VRM de Open Source Avatars, modelos de
  referencia (VRM Consortium, Khronos, three.js) y modelos CC de Sketchfab. Si una pasarela IPFS falla prueba otra, solo
  descarga URLs del catálogo y lo descargado sirve sin conexión (`/api/avatares`).

### RAG que lee páginas como un navegador

- Las webs que se pintan con JavaScript (React, Vue, Angular, tiendas...) se leen con Chrome, Edge, Chromium o Brave sin
  interfaz si hay uno en el equipo; si no, todo sigue con la descarga normal. Lo usan el rastreador, «URL a PDF» y la
  herramienta web de los agentes (`rag.browser`: auto, siempre o nunca; `rag.browser_path` para elegir uno).
- El navegador sale por un proxy local que solo deja direcciones públicas (salvo `rag.allow_private_urls`), con un
  perfil propio limpio en cada lectura. API nueva `GET /api/rag/navegador`.

### Personajes con sexo elegible

- Cada agente (`sexo` en su ficha) y el propio usuario (`user.sexo`) pueden ser mujer u hombre; despachos y sala de
  descanso lo respetan.

### Ejemplos para importar
- **Grafo multicapa de la Wikipedia en español** (`ejemplos/grafo-wikipedia-es`): 2.251 nodos y 7.007 relaciones en
  cuatro capas (temas, artículos, enlaces entre artículos y hechos de Wikidata), en GraphML y JSON. Se importa con
  `nexus-office grafo importar ejemplos/grafo-wikipedia-es/grafo.graphml --en auto`.
- **Resumen del proyecto** (`ejemplos/resumen-proyecto/Nexus-Office-resumen.md`) para subirlo a la biblioteca: los
  agentes responden preguntas sobre el propio Nexus-Office.

### Terminal
- Órdenes nuevas en C++: `bolsa`, `grafo`, `setup`, `wheels` y `cleanup` (quita del paquete offline las variantes de
  runtime que el equipo no usa, con `--dry-run` y `--yes`).

## Nexus-Office 2.0.0-beta-cpp (2.0 beta C)

### Nexus-Office reescrito en C++
- **Antes**: Nexus-Office era una aplicación Python que llevaba dentro su propio intérprete y unos 60 paquetes; cada
  instalador ocupaba entre 0,5 y 1,7 GB y tardaba en arrancar.
- **Ahora** hay una edición C++ con la misma interfaz, la misma API, los mismos perfiles, agentes y datos: un único
  programa nativo de unos 15 MB que arranca en 0,13 s y usa 68 MB de memoria en reposo (la edición Python, 1,2 s y
  119 MB en el mismo equipo). Los modelos de IA y los motores (llama-server y stable-diffusion.cpp) son los mismos.
- Las dos ediciones se comparan automáticamente: 39 rutas de la API y 41 flujos completos (administrador, memoria,
  agentes, biblioteca, multimedia, grafo, oficina y chat) dan la misma respuesta.
- Instaladores 100 % offline para Windows (x64 y ARM64), Linux (x86_64 y ARM64: .run, .deb, AppImage y Flatpak),
  macOS (Apple Silicon e Intel) y Android, con servicio del sistema y elección del modelo de IA según el hardware. Se
  instala junto a la edición Python sin pisarla.
- Es una beta: la mesa de bolsa, la red COOP, la VPN, el acceso remoto, importar grafos en otros formatos, el rastreo
  web y las herramientas de GIMP siguen solo en la edición Python. Ver [docs/CPP.md](docs/CPP.md).
